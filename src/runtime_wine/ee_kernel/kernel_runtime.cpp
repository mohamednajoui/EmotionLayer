#include "kernel_runtime.hpp"
#include "../ee_syscalls.hpp"
#include "../diag.hpp"
#include "dmac_vif.hpp"
#include "gsvk/gsvk_renderer.hpp"
#include "../memory_arena.hpp"
#include "../smc_veh_guard.hpp"
#include "../iop_hle/sif_iop_hle.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace emotion {

EEKernelRuntime::FiberConversionHook EEKernelRuntime::s_fiber_hook_ = nullptr;
EEKernelRuntime::FiberCreateHook     EEKernelRuntime::s_fiber_create_hook_ = nullptr;
std::vector<EEKernelRuntime*> EEKernelRuntime::instances_;
EEKernelRuntime* EEKernelRuntime::active_instance_ = nullptr;

void EEKernelRuntime::register_instance(EEKernelRuntime* inst) noexcept {
    if (!inst) return;
    auto it = std::find(instances_.begin(), instances_.end(), inst);
    if (it == instances_.end()) {
        instances_.push_back(inst);
    }
    active_instance_ = inst;
}

void EEKernelRuntime::unregister_instance(EEKernelRuntime* inst) noexcept {
    if (!inst) return;
    auto it = std::find(instances_.begin(), instances_.end(), inst);
    if (it != instances_.end()) {
        instances_.erase(it);
    }
    if (active_instance_ == inst) {
        active_instance_ = instances_.empty() ? nullptr : instances_.back();
    }
    if (!active_instance_) {
        MemoryArena::unregister_mmio_callbacks();
    }
}

EEKernelRuntime::EEKernelRuntime() noexcept {
    threads_.resize(kMaxThreads);
    for (s32 i = 0; i < kMaxThreads; ++i) {
        threads_[i] = std::make_unique<GuestThread>();
        threads_[i]->thid  = i;
        threads_[i]->state = EEThreadState::Unused;
    }
}

EEKernelRuntime::~EEKernelRuntime() noexcept {
    shutdown();
}

bool EEKernelRuntime::initialize_main_fiber(u8* fastmem_base, u32 entry_point, EERecompiledRunnerFn runner_fn) {
    shutdown();
    fastmem_base_    = fastmem_base;
    runner_fn_       = runner_fn;
    register_instance(this);
    MemoryArena::register_mmio_callbacks(&EEKernelRuntime::mmio_write_trampoline, &EEKernelRuntime::mmio_read_trampoline);
    trace_           = (std::getenv("EMOTION_TRACE") != nullptr);

    // Convertir el hilo principal de Windows en una fibra nativa (o reutilizar si ya es fibra)
    void* main_fiber = s_fiber_hook_ ? s_fiber_hook_(nullptr, FIBER_FLAG_FLOAT_SWITCH)
                                     : ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (!main_fiber) {
        if (GetLastError() == ERROR_ALREADY_FIBER) {
            main_fiber = GetCurrentFiber();
            main_is_fiber_ = false;
        } else {
            unregister_instance(this);
            fastmem_base_ = nullptr;
            runner_fn_    = nullptr;
            return false;
        }
    } else {
        main_is_fiber_ = true;
    }

    auto& main_thr      = *threads_[1];
    main_thr.thid       = 1;
    main_thr.state      = EEThreadState::Running;
    main_thr.priority   = 64;
    main_thr.entry_pc   = entry_point;
    main_thr.stack_base = 0x01FF0000U;
    main_thr.stack_size = 0x00010000;
    main_thr.fiber      = main_fiber;
    main_thr.ctx.reset(fastmem_base_, entry_point);
    current_thid_ = 1;

    if (fastmem_base_) {
        // Sony PS2 EE BIOS Kernel detection vectors (esperados por libkernel CRT: FindAddress)
        // kernel_base = 0x80001000:
        // offset 360 (0x168): srl $a2, $a2, 2 (0x00063082)
        // offset 524 (0x20C): lw $v0, 0($a0)   (0x8C820000)
        MemoryArena::fast_write<u32>(fastmem_base_, 0x1000U + 360U, 0x00063082U);
        MemoryArena::fast_write<u32>(fastmem_base_, 0x1000U + 524U, 0x8C820000U);
    }
    EESyscalls::reset();
    return true;
}

void EEKernelRuntime::shutdown() noexcept {
    for (s32 i = 2; i < kMaxThreads; ++i) {
        if (threads_[i] && threads_[i]->fiber) {
            DeleteFiber(threads_[i]->fiber);
            threads_[i]->fiber = nullptr;
            threads_[i]->state = EEThreadState::Unused;
        }
    }
    if (main_is_fiber_) {
        ConvertFiberToThread();
        main_is_fiber_ = false;
    }
    if (threads_.size() > 1 && threads_[1]) {
        threads_[1]->fiber = nullptr;
        threads_[1]->state = EEThreadState::Unused;
    }
    current_thid_ = 0;
    ready_queue_.clear();
    for (auto& sema : semaphores_) {
        sema.active = false;
        sema.count = 0;
        sema.max_count = 1;
        sema.init_count = 0;
        sema.wait_queue.clear();
    }
    unregister_instance(this);
    DynamicRecompCache::reset();
    EESyscalls::reset();
    fastmem_base_ = nullptr;
    runner_fn_ = nullptr;
}

s32 EEKernelRuntime::run_main_loop() {
    auto& main_thr = *threads_[1];
    runner_fn_(main_thr.ctx);
    return main_thr.ctx.exit_code;
}

void __stdcall EEKernelRuntime::fiber_entry_trampoline(void* lpParameter) {
    auto* self = active_instance_;
    const s32 thid = static_cast<s32>(reinterpret_cast<std::intptr_t>(lpParameter));
    GuestThread& thr = *self->threads_[thid];

    while (!thr.ctx.halted) {
        self->runner_fn_(thr.ctx);
        // Si el hilo retornó a kThreadExitReturnSentinel o terminó su función, salimos del hilo invitado
        self->sys_exit_thread();
    }
}

void EEKernelRuntime::enqueue_ready(s32 thid) {
    remove_from_ready(thid);
    const s32 prio = threads_[thid]->priority;
    // Insertar manteniendo orden por prioridad del Emotion Engine (menor valor numérico = mayor prioridad)
    auto it = std::find_if(ready_queue_.begin(), ready_queue_.end(), [&](s32 other_id) {
        return threads_[other_id]->priority > prio;
    });
    ready_queue_.insert(it, thid);
}

void EEKernelRuntime::remove_from_ready(s32 thid) {
    auto it = std::find(ready_queue_.begin(), ready_queue_.end(), thid);
    if (it != ready_queue_.end()) {
        ready_queue_.erase(it);
    }
}

s32 EEKernelRuntime::pick_next_ready_thread() const noexcept {
    if (ready_queue_.empty()) return -1;
    return ready_queue_.front();
}

void EEKernelRuntime::schedule_next(EEThreadState current_new_state) {
    const s32 prev_thid = current_thid_;
    GuestThread& prev   = *threads_[prev_thid];
    prev.state          = current_new_state;
    if (current_new_state == EEThreadState::Ready) {
        enqueue_ready(prev_thid);
    }

    s32 next_thid = pick_next_ready_thread();
    if (next_thid < 0) {
        // Disparar alarmas activas que puedan desbloquear hilos esperando en semáforos o en SleepThread
        fire_expired_alarms(prev.ctx);
        next_thid = pick_next_ready_thread();
    }
    if (next_thid < 0) {
        // En emulación HLE nativa en Windows, si la cola de listos está vacía pero hay hilos
        // esperando respuesta de SIF RPC (semáforos de fin de RPC con max_count == 1 creados por sceSifCallRpc),
        // señalamos el semáforo para que el hilo principal retome la ejecución tras la RPC completada.
        if (!in_irq_ && !idle_wait_ && hw_irq_enabled_ && current_new_state == EEThreadState::Waiting) {
            // Kernel idle (comportamiento real del EE): si el hilo actual se bloquea y no hay ningún otro
            // listo, la CPU queda en el hilo idle con interrupciones habilitadas hasta que un VBlank/DMAC/alarma
            // despierte a algún hilo. El hilo bloqueado NO debe continuar su ejecución.
            idle_wait_ = true;
            const bool saved_ei = cpu_int_enabled_;
            cpu_int_enabled_ = true;
            const ULONGLONG t0 = GetTickCount64();
            while (next_thid < 0 && GetTickCount64() - t0 < 3000) {
                poll_interrupts(prev.ctx);
                fire_expired_alarms(prev.ctx);
                if (SifIopHle::active_instance() && SifIopHle::active_instance()->has_pending_command()) {
                    SifIopHle::active_instance()->count_ticks(1, this);
                }
                next_thid = pick_next_ready_thread();
                if (next_thid < 0) Sleep(1);
            }
            cpu_int_enabled_ = saved_ei;
            idle_wait_ = false;
            if (next_thid < 0) {
                if (Diag::enabled()) Diag::get().event("schedule_next: IDLE agotado 3s sin hilo listo (posible deadlock)");
                dump_state("IDLE timeout (deadlock)");
            } else if (Diag::enabled()) {
                Diag::get().event("schedule_next: idle OK (hilo despertado por hardware/IRQ)");
            }
        }
        for (s32 s = 1; next_thid < 0 && s < kMaxSemaphores; ++s) {
            if (semaphores_[s].active && semaphores_[s].max_count == 1 && !semaphores_[s].wait_queue.empty()) {
                if (Diag::enabled()) Diag::get().event("schedule_next: FORCED signal sema (hack)"); sys_signal_sema(s);
                break;
            }
        }
        next_thid = pick_next_ready_thread();
    }
    if (next_thid < 0 || next_thid == prev_thid) {
        if (Diag::enabled()) { char b[128]; std::snprintf(b, sizeof b, "schedule_next: SIN HILO LISTO, hilo %d (estado nuevo=%d) sigue ejecutando (next=%d)", prev_thid, (int)current_new_state, next_thid); Diag::get().event(b); }
        prev.state = EEThreadState::Running;
        remove_from_ready(prev_thid);
        return;
    }

    remove_from_ready(next_thid);
    current_thid_ = next_thid;
    GuestThread& next = *threads_[next_thid];
    next.state = EEThreadState::Running;

    SwitchToFiber(next.fiber);
}

s32 EEKernelRuntime::sys_create_thread(u32 param_vaddr) {
    if (!fastmem_base_ || !MemoryArena::is_valid_guest_range(param_vaddr, sizeof(EEThreadParam))) return -1;
    const auto param = MemoryArena::fast_read<EEThreadParam>(fastmem_base_, param_vaddr);
    if (param.func == 0 || param.stack == 0 || param.stack_size <= 0) return -1;
    for (s32 id = 2; id < kMaxThreads; ++id) {
        GuestThread& t = *threads_[id];
        if (t.state == EEThreadState::Unused) {
            t.state      = EEThreadState::Dormant;
            t.priority   = (param.initial_priority > 0) ? param.initial_priority : 64;
            t.entry_pc   = param.func;
            t.stack_base = param.stack;
            t.stack_size = param.stack_size;
            t.gp_reg     = param.gp_reg;
            t.wakeup_cnt = 0;
            t.ctx.reset(fastmem_base_, param.func);
            t.ctx.gpr[28].ud[0] = param.gp_reg;
            t.ctx.gpr[29].ud[0] = (param.stack + static_cast<u32>(param.stack_size)) & ~0xFU;
            t.ctx.gpr[31].ud[0] = kThreadExitReturnSentinel;

            if (!t.fiber) {
                t.fiber = s_fiber_create_hook_
                    ? s_fiber_create_hook_(256 * 1024, 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH,
                                           &EEKernelRuntime::fiber_entry_trampoline,
                                           reinterpret_cast<void*>(static_cast<std::intptr_t>(id)))
                    : CreateFiberEx(
                        256 * 1024,
                        1024 * 1024,
                        FIBER_FLAG_FLOAT_SWITCH,
                        &EEKernelRuntime::fiber_entry_trampoline,
                        reinterpret_cast<void*>(static_cast<std::intptr_t>(id))
                    );
            }
            if (!t.fiber) {
                t.state = EEThreadState::Unused;
                return -1;
            }
            return id;
        }
    }
    return -1;
}

s32 EEKernelRuntime::sys_delete_thread(s32 thid) {
    if (thid <= 1 || thid >= kMaxThreads) return -1;
    GuestThread& t = *threads_[thid];
    if (t.state == EEThreadState::Running) return -1;
    remove_from_ready(thid);
    if (t.fiber) {
        DeleteFiber(t.fiber);
        t.fiber = nullptr;
    }
    t.state = EEThreadState::Unused;
    return thid;
}


s32 EEKernelRuntime::sys_start_thread(s32 thid, u32 arg_vaddr) {
    if (thid <= 1 || thid >= kMaxThreads) return -1;
    GuestThread& t = *threads_[thid];
    if (t.state != EEThreadState::Dormant) return -1;

    t.ctx.reset(fastmem_base_, t.entry_pc);
    t.ctx.gpr[4].sd[0]  = static_cast<s64>(static_cast<s32>(arg_vaddr)); // $a0 = arg
    t.ctx.gpr[28].ud[0] = t.gp_reg;
    t.ctx.gpr[29].ud[0] = (t.stack_base + static_cast<u32>(t.stack_size)) & ~0xFU;
    t.ctx.gpr[31].ud[0] = kThreadExitReturnSentinel;

    t.state = EEThreadState::Ready;
    enqueue_ready(thid);

    // Si el nuevo hilo tiene mayor prioridad que el actual, cedemos la CPU de inmediato
    if (t.priority < threads_[current_thid_]->priority) {
        schedule_next(EEThreadState::Ready);
    }
    return thid;
}


void EEKernelRuntime::sys_exit_thread() {
    if (current_thid_ == 1) {
        threads_[1]->ctx.halted = true;
        return;
    }
    schedule_next(EEThreadState::Dormant);
}

s32 EEKernelRuntime::sys_rotate_thread_ready_queue(s32 prio) {
    (void)prio;
    schedule_next(EEThreadState::Ready);
    return prio;
}

s32 EEKernelRuntime::sys_sleep_thread() {
    GuestThread& cur = *threads_[current_thid_];
    if (cur.wakeup_cnt > 0) {
        cur.wakeup_cnt--;
        return 0;
    }
    schedule_next(EEThreadState::Waiting);
    return 0;
}

s32 EEKernelRuntime::sys_wakeup_thread(s32 thid) {
    if (thid < 1 || thid >= kMaxThreads || thid == current_thid_) return -1;
    GuestThread& t = *threads_[thid];
    if (t.state == EEThreadState::Waiting) {
        t.state = EEThreadState::Ready;
        enqueue_ready(thid);
        if (t.priority < threads_[current_thid_]->priority) {
            request_preempt();
        }
    } else {
        t.wakeup_cnt++;
    }
    return thid;
}


s32 EEKernelRuntime::sys_change_thread_priority(s32 thid, s32 prio) {
    if (thid == 0) thid = current_thid_;
    if (thid < 1 || thid >= kMaxThreads) return -1;
    GuestThread& t = *threads_[thid];
    if (t.state == EEThreadState::Unused) return -1;
    const s32 old_prio = t.priority;
    t.priority = (prio > 0 && prio < 256) ? prio : 64;
    if (t.state == EEThreadState::Ready) {
        remove_from_ready(thid);
        enqueue_ready(thid);
    }
    if (t.priority < threads_[current_thid_]->priority) {
        request_preempt();
    }
    return old_prio;
}

s32 EEKernelRuntime::sys_suspend_thread(s32 thid) {
    if (thid == 0) thid = current_thid_;
    if (thid < 1 || thid >= kMaxThreads) return -1;
    GuestThread& t = *threads_[thid];
    if (t.state == EEThreadState::Unused || t.state == EEThreadState::Dormant) return -1;
    if (thid == current_thid_) {
        schedule_next(EEThreadState::Waiting);
    } else {
        remove_from_ready(thid);
        t.state = EEThreadState::Waiting;
    }
    return 0;
}

s32 EEKernelRuntime::sys_resume_thread(s32 thid) {
    if (thid < 1 || thid >= kMaxThreads) return -1;
    GuestThread& t = *threads_[thid];
    if (t.state == EEThreadState::Waiting) {
        t.state = EEThreadState::Ready;
        enqueue_ready(thid);
        if (t.priority < threads_[current_thid_]->priority) {
            request_preempt();
        }
    }
    return 0;
}

s32 EEKernelRuntime::sys_refer_thread_status(s32 thid, u32 status_vaddr) {
    if (thid == 0) thid = current_thid_;
    if (thid < 1 || thid >= kMaxThreads || status_vaddr == 0 || !fastmem_base_) return -1;
    const GuestThread& t = *threads_[thid];
    if (t.state == EEThreadState::Unused) return -1;

    struct EEThreadStatusGuest {
        s32 status;
        u32 func;
        u32 stack;
        s32 stack_size;
        u32 gp_reg;
        s32 initial_priority;
        s32 current_priority;
        u32 wait_type;
        s32 wait_id;
        s32 wakeup_count;
    } st{};

    st.status = static_cast<s32>(t.state);
    st.func = t.entry_pc;
    st.stack = t.stack_base;
    st.stack_size = t.stack_size;
    st.gp_reg = t.gp_reg;
    st.initial_priority = t.priority;
    st.current_priority = t.priority;
    st.wakeup_count = t.wakeup_cnt;

    if (!MemoryArena::safe_write_bytes(fastmem_base_, status_vaddr, reinterpret_cast<const u8*>(&st), sizeof(st))) {
        return -1;
    }
    return 0;
}

s32 EEKernelRuntime::sys_create_sema(u32 sema_param_vaddr) {
    if (!fastmem_base_ || !MemoryArena::is_valid_guest_range(sema_param_vaddr, sizeof(EESemaParam))) return -1;
    const auto param = MemoryArena::fast_read<EESemaParam>(fastmem_base_, sema_param_vaddr);
    if (param.max_count <= 0 || param.init_count < 0 || param.init_count > param.max_count) return -1;
    for (s32 i = 1; i < kMaxSemaphores; ++i) {
        if (!semaphores_[i].active) {
            semaphores_[i].active     = true;
            semaphores_[i].count      = param.init_count;
            semaphores_[i].max_count  = param.max_count;
            semaphores_[i].init_count = param.init_count;
            semaphores_[i].attr       = param.attr;
            semaphores_[i].option     = param.option;
            if (Diag::enabled()) { char b[128]; std::snprintf(b, sizeof b, "CreateSema -> id=%d init=%d max=%d thid=%d", i, param.init_count, param.max_count, current_thid_); Diag::get().event(b); }
            semaphores_[i].wait_queue.clear();
            return i;
        }
    }
    return -1;
}

s32 EEKernelRuntime::sys_delete_sema(s32 semid) {
    if (Diag::enabled()) { char b[96]; std::snprintf(b, sizeof b, "DeleteSema id=%d thid=%d active=%d", semid, current_thid_, (semid >= 1 && semid < kMaxSemaphores) ? (semaphores_[semid].active ? 1 : 0) : -1); Diag::get().event(b); }
    if (semid < 1 || semid >= kMaxSemaphores || !semaphores_[semid].active) { if (Diag::enabled()) { char b[96]; std::snprintf(b, sizeof b, "SEMA INVALIDA/inactiva id=%d thid=%d (syscall retorna -1)", semid, current_thid_); Diag::get().event(b); } return -1; }
    auto& sem = semaphores_[semid];
    while (!sem.wait_queue.empty()) {
        const s32 waiter = sem.wait_queue.front();
        sem.wait_queue.pop_front();
        threads_[waiter]->state = EEThreadState::Ready;
        enqueue_ready(waiter);
    }
    sem.active = false;
    return semid;
}

s32 EEKernelRuntime::sys_signal_sema(s32 semid) {
    if (Diag::enabled()) { char b[96]; std::snprintf(b, sizeof b, "SignalSema sem=%d thid=%d in_irq=%d", semid, current_thid_, in_irq_ ? 1 : 0); Diag::get().event(b); }
    if (semid < 1 || semid >= kMaxSemaphores || !semaphores_[semid].active) { if (Diag::enabled()) { char b[96]; std::snprintf(b, sizeof b, "SEMA INVALIDA/inactiva id=%d thid=%d (syscall retorna -1)", semid, current_thid_); Diag::get().event(b); } return -1; }
    auto& sem = semaphores_[semid];
    if (!sem.wait_queue.empty()) {
        const s32 woken_thid = sem.wait_queue.front();
        sem.wait_queue.pop_front();
        GuestThread& woken = *threads_[woken_thid];
        woken.state = EEThreadState::Ready;
        enqueue_ready(woken_thid);

        if (woken.priority < threads_[current_thid_]->priority) {
            request_preempt();
        }
        return semid;
    }
    if (sem.count >= sem.max_count) {
        return -1; // Desbordamiento de contador de semaforo
    }
    sem.count++;
    return semid;
}

s32 EEKernelRuntime::sys_wait_sema(s32 semid) {
    if (Diag::enabled()) { char b[64]; std::snprintf(b, sizeof b, "WaitSema sem=%d thid=%d", semid, current_thid_); Diag::get().event(b); }
    if (semid < 1 || semid >= kMaxSemaphores || !semaphores_[semid].active) { if (Diag::enabled()) { char b[96]; std::snprintf(b, sizeof b, "SEMA INVALIDA/inactiva id=%d thid=%d (syscall retorna -1)", semid, current_thid_); Diag::get().event(b); } return -1; }
    auto& sem = semaphores_[semid];
    if (sem.count > 0) {
        sem.count--;
        return semid;
    }
    sem.wait_queue.push_back(current_thid_);
    schedule_next(EEThreadState::Waiting);
    return semid;
}

s32 EEKernelRuntime::sys_poll_sema(s32 semid) {
    if (semid < 1 || semid >= kMaxSemaphores || !semaphores_[semid].active) { if (Diag::enabled()) { char b[96]; std::snprintf(b, sizeof b, "SEMA INVALIDA/inactiva id=%d thid=%d (syscall retorna -1)", semid, current_thid_); Diag::get().event(b); } return -1; }
    auto& sem = semaphores_[semid];
    if (sem.count > 0) {
        sem.count--;
        return semid;
    }
    return -1;
}

// ===========================================================================
// Sistema de interrupciones del Emotion Engine (INTC / DMAC / VBlank)
//
// Modelo:
//  - Los registros INTC_STAT (0x1000F000) e INTC_MASK (0x1000F010) viven en la arena. El intérprete
//    redirige sus escrituras a mmio_write() para respetar la semántica de hardware
//    (STAT: escribir 1 limpia el bit; MASK: escribir 1 invierte el bit).
//  - Un reloj de tiempo real (QueryPerformanceCounter) genera VBLANK_START / VBLANK_END a 50 Hz (PAL)
//    o 59.94 Hz (NTSC). Se comprueba cada 1024 bloques básicos, donde el estado de CPU es consistente.
//  - Al detectar (STAT & MASK) con interrupciones habilitadas (EI), el kernel reconoce el bit de STAT y
//    ejecuta los handlers del juego sobre el mismo fiber, con el contexto guardado y restaurado.
//  - Las i-variantes de syscalls invocadas dentro de un handler (iSignalSema, iWakeupThread) no
//    replanifican inmediatamente: se aplaza el cambio de hilo hasta que el handler retorna.
// ===========================================================================

u32 EEKernelRuntime::read_phys32(u32 addr) const noexcept {
    if (!fastmem_base_) return 0;
    u32 v = 0;
    std::memcpy(&v, fastmem_base_ + (addr & 0x1FFFFFFFU), sizeof(v));
    return v;
}

u64 EEKernelRuntime::read_phys64(u32 addr) const noexcept {
    if (!fastmem_base_) return 0;
    u64 v = 0;
    std::memcpy(&v, fastmem_base_ + (addr & 0x1FFFFFFFU), sizeof(v));
    return v;
}

void EEKernelRuntime::write_phys32(u32 addr, u32 value) noexcept {
    if (!fastmem_base_) return;
    std::memcpy(fastmem_base_ + (addr & 0x1FFFFFFFU), &value, sizeof(value));
}

void EEKernelRuntime::write_phys64(u32 addr, u64 value) noexcept {
    if (!fastmem_base_) return;
    std::memcpy(fastmem_base_ + (addr & 0x1FFFFFFFU), &value, sizeof(value));
}

void EEKernelRuntime::write_phys16(u32 addr, u16 value) noexcept {
    if (!fastmem_base_) return;
    std::memcpy(fastmem_base_ + (addr & 0x1FFFFFFFU), &value, sizeof(value));
}

namespace {
static inline s32 dmac_channel_from_addr(u32 pa, u32& out_base, u32& out_reg) {
    if ((pa & ~0x00FFU) == 0x10008000U) { out_base = 0x10008000U; out_reg = pa & 0xFFU; return 0; }
    if ((pa & ~0x00FFU) == 0x10009000U) { out_base = 0x10009000U; out_reg = pa & 0xFFU; return 1; }
    if ((pa & ~0x00FFU) == 0x1000A000U) { out_base = 0x1000A000U; out_reg = pa & 0xFFU; return 2; }
    if ((pa & ~0x00FFU) == 0x1000B000U) { out_base = 0x1000B000U; out_reg = pa & 0xFFU; return 3; }
    if ((pa & ~0x00FFU) == 0x1000B400U) { out_base = 0x1000B400U; out_reg = pa & 0xFFU; return 4; }
    if ((pa & ~0x00FFU) == 0x1000C000U) { out_base = 0x1000C000U; out_reg = pa & 0xFFU; return 5; }
    if ((pa & ~0x00FFU) == 0x1000C400U) { out_base = 0x1000C400U; out_reg = pa & 0xFFU; return 6; }
    if ((pa & ~0x00FFU) == 0x1000C800U) { out_base = 0x1000C800U; out_reg = pa & 0xFFU; return 7; }
    if ((pa & ~0x00FFU) == 0x1000D000U) { out_base = 0x1000D000U; out_reg = pa & 0xFFU; return 8; }
    if ((pa & ~0x00FFU) == 0x1000D400U) { out_base = 0x1000D400U; out_reg = pa & 0xFFU; return 9; }
    return -1;
}

void insert_handler(std::vector<GuestIrqHandler>& list, const GuestIrqHandler& h, s32 next) {
    if (next == 0) { // 0 = al principio de la cadena
        list.insert(list.begin(), h);
        return;
    }
    if (next > 0) { // id de un handler existente: insertar antes de él
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->id == next) {
                list.insert(it, h);
                return;
            }
        }
    }
    list.push_back(h); // -1 (o id inexistente) = al final
}

bool remove_handler(std::vector<GuestIrqHandler>& list, s32 id) {
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->id == id) {
            list.erase(it);
            return true;
        }
    }
    return false;
}
} // namespace

// --- Syscalls de Alarmas del Emotion Engine (0xFC / 0xFD / 0x3C / 0x3D) ---
s32 EEKernelRuntime::sys_set_alarm(u16 time, u32 func, u32 arg, u32 gp) {
    if (func == 0) return -1;
    for (s32 i = 0; i < kMaxAlarms; ++i) {
        if (!alarms_[i].active) {
            alarms_[i].id = i + 1;
            alarms_[i].time = time;
            alarms_[i].func = func;
            alarms_[i].arg = arg;
            alarms_[i].gp = gp;
            alarms_[i].fire_counter = poll_counter_ + (time > 0 ? (static_cast<u64>(time) * 4U) : 1U);
            alarms_[i].active = true;
            return alarms_[i].id;
        }
    }
    return -1;
}

s32 EEKernelRuntime::sys_release_alarm(s32 id) {
    if (id >= 1 && id <= kMaxAlarms && alarms_[id - 1].active) {
        alarms_[id - 1].active = false;
        return id;
    }
    return -1;
}

void EEKernelRuntime::run_single_alarm(EEContext& ctx, const GuestAlarm& a) {
    const EEContext saved = ctx;
    const bool saved_int_enabled = cpu_int_enabled_;
    const u32 irq_sp = (saved.gpr[29].uw[0] - 0x400U) & ~0xFU;

    in_irq_ = true;
    cpu_int_enabled_ = false;

    ctx.gpr[4].sd[0]  = static_cast<s64>(a.id);
    ctx.gpr[5].sd[0]  = static_cast<s64>(a.time);
    ctx.gpr[6].sd[0]  = static_cast<s64>(static_cast<s32>(a.arg));
    ctx.gpr[7].sd[0]  = 0;
    ctx.gpr[28].sd[0] = static_cast<s64>(static_cast<s32>(a.gp));
    ctx.gpr[29].sd[0] = static_cast<s64>(static_cast<s32>(irq_sp));
    ctx.gpr[31].sd[0] = static_cast<s64>(static_cast<s32>(kIrqReturnSentinel));
    ctx.pc = a.func;

    constexpr u32 kMaxHandlerBlocks = 4000000U;
    u32 executed = 0;
    while (!ctx.halted && ctx.pc != kIrqReturnSentinel) {
        if (!DynamicRecompCache::execute_or_compile(ctx)) {
            break;
        }
        if (++executed > kMaxHandlerBlocks) {
            break;
        }
    }

    in_irq_ = false;
    cpu_int_enabled_ = saved_int_enabled;
    ctx = saved;
}

void EEKernelRuntime::fire_expired_alarms(EEContext& ctx) {
    for (s32 i = 0; i < kMaxAlarms; ++i) {
        if (alarms_[i].active) {
            alarms_[i].active = false;
            run_single_alarm(ctx, alarms_[i]);
        }
    }
}


s32 EEKernelRuntime::sys_add_intc_handler(u32 cause, u32 func, s32 next, u32 arg, u32 gp) {
    if (cause >= kIntcCauseCount) return -1;
    GuestIrqHandler h;
    h.id = next_handler_id_++;
    h.func = func;
    h.gp = gp;
    h.arg = arg;
    insert_handler(intc_handlers_[cause], h, next);
    if (trace_) std::fprintf(stderr, "[EmotionLayer TRACE] AddIntcHandler cause=%u func=0x%08X gp=0x%08X -> id=%d\n", cause, func, gp, h.id);
    return h.id;
}

s32 EEKernelRuntime::sys_remove_intc_handler(u32 cause, s32 id) {
    if (cause >= kIntcCauseCount) return -1;
    return remove_handler(intc_handlers_[cause], id) ? 0 : -1;
}

s32 EEKernelRuntime::sys_add_dmac_handler(u32 cause, u32 func, s32 next, u32 arg, u32 gp) {
    if (cause >= kDmacCauseCount) return -1;
    GuestIrqHandler h;
    h.id = next_handler_id_++;
    h.func = func;
    h.gp = gp;
    h.arg = arg;
    insert_handler(dmac_handlers_[cause], h, next);
    return h.id;
}

s32 EEKernelRuntime::sys_remove_dmac_handler(u32 cause, s32 id) {
    if (cause >= kDmacCauseCount) return -1;
    return remove_handler(dmac_handlers_[cause], id) ? 0 : -1;
}

s32 EEKernelRuntime::sys_enable_intc(u32 cause) {
    if (cause >= kIntcCauseCount || !hw_irq_enabled_) return 0;
    const u32 mask = read_phys32(kIntcMaskAddr);
    const u32 bit  = 1U << cause;
    write_phys32(kIntcMaskAddr, mask | bit);
    return (mask & bit) ? 0 : 1; // 1 si cambió de deshabilitado a habilitado
}

s32 EEKernelRuntime::sys_disable_intc(u32 cause) {
    if (cause >= kIntcCauseCount || !hw_irq_enabled_) return 0;
    const u32 mask = read_phys32(kIntcMaskAddr);
    const u32 bit  = 1U << cause;
    write_phys32(kIntcMaskAddr, mask & ~bit);
    return (mask & bit) ? 1 : 0; // 1 si estaba habilitado
}

s32 EEKernelRuntime::sys_enable_dmac(u32 cause) {
    if (cause >= kDmacCauseCount || !hw_irq_enabled_) return 0;
    const u32 stat = read_phys32(kDmacStatAddr);
    const u32 bit  = 1U << (16U + cause);
    write_phys32(kDmacStatAddr, stat | bit);
    return (stat & bit) ? 0 : 1;
}

s32 EEKernelRuntime::sys_disable_dmac(u32 cause) {
    if (cause >= kDmacCauseCount || !hw_irq_enabled_) return 0;
    const u32 stat = read_phys32(kDmacStatAddr);
    const u32 bit  = 1U << (16U + cause);
    write_phys32(kDmacStatAddr, stat & ~bit);
    return (stat & bit) ? 1 : 0;
}

void EEKernelRuntime::sys_set_vsync_flag(u32 flag0_vaddr, u32 flag1_vaddr) {
    vsync_flag0_addr_ = flag0_vaddr;
    vsync_flag1_addr_ = flag1_vaddr;
}

u32 EEKernelRuntime::sys_gs_put_imt(u32 imr) {
    const u32 old = gs_imr_;
    gs_imr_ = imr;
    return old;
}

void EEKernelRuntime::set_video_standard(bool pal) noexcept {
    pal_ = pal;
    if (timing_started_ && qpc_freq_ > 0) {
        frame_period_ = pal_ ? (qpc_freq_ / 50) : (qpc_freq_ * 1001 / 60000);
    }
}

u32 EEKernelRuntime::cop0_read(u32 reg) const noexcept {
    switch (reg) {
        case 9: { // Count: 294.912 MHz (reloj del bus del EE / 2)
            LARGE_INTEGER freq, now;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);
            const double secs = static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
            return static_cast<u32>(static_cast<u64>(secs * 294912000.0));
        }
        case 12: // Status: CU0..CU2 usables + EIE/IE según el estado de DI/EI
            return 0x70000000U | (cpu_int_enabled_ ? 0x00010001U : 0U);
        case 15: // PRId del R5900
            return 0x00002E20U;
        default:
            return 0;
    }
}

void EEKernelRuntime::cop0_write(u32 reg, u32 value) noexcept {
    if (reg == 12) {
        cpu_int_enabled_ = (value & 0x00010001U) == 0x00010001U;
    }
}

bool EEKernelRuntime::mmio_write(u32 vaddr, u32 size_bytes, u64 value) {
    const u32 pa = vaddr & 0x1FFFFFFFU;
    const u32 w  = static_cast<u32>(value);

    // 1. INTC y GS
    switch (pa) {
        case kIntcStatAddr: // INTC_STAT: escribir 1 limpia el bit
            write_phys32(kIntcStatAddr, read_phys32(kIntcStatAddr) & ~w);
            return true;
        case kIntcMaskAddr: // INTC_MASK: escribir 1 invierte el bit
            write_phys32(kIntcMaskAddr, read_phys32(kIntcMaskAddr) ^ w);
            return true;
        case kGsCsrAddr: {  // GS_CSR: bits 0..4 son flags W1C, RESET (bit 9) reinicia, REV/ID/FIELD son de sólo lectura
            u32 csr = read_phys32(kGsCsrAddr);
            if (w & 0x200U) {
                csr = (csr & 0xFFFF0000U) | 0x4000U; // FIFO vacía
            } else {
                csr &= ~(w & 0x1FU);
            }
            write_phys32(kGsCsrAddr, csr);
            return true;
        }
    }

    // 1b. Registros Privilegiados de Display del GS (0x12000000..0x120000C0: PMODE, DISPFB1/2, DISPLAY1/2)
    if (pa >= 0x12000000U && pa <= 0x120000C0U) {
        if (GsvkRenderer::active_instance()) {
            GsvkRenderer::active_instance()->write_privileged_register(pa, value);
        }
        write_phys64(pa, value);
        return true;
    }

    // 2. DMAC Global
    if (pa == 0x1000E000U) { // D_CTRL
        write_phys32(0x1000E000U, w);
        return true;
    }
    if (pa == kDmacStatAddr) { // D_STAT: bits 0..15 limpia estado (W1C), bits 16..31 invierte máscara
        u32 cur = read_phys32(kDmacStatAddr);
        const u32 clr_status = w & 0x0000FFFFU;
        const u32 xor_mask   = w & 0xFFFF0000U;
        cur = (cur & ~clr_status) ^ xor_mask;
        write_phys32(kDmacStatAddr, cur);
        return true;
    }
    if (pa >= 0x1000E020U && pa <= 0x1000E060U) {
        write_phys32(pa, w);
        return true;
    }

    // 3. DMAC Canales (0..9)
    u32 ch_base = 0;
    u32 ch_reg  = 0;
    const s32 ch = dmac_channel_from_addr(pa, ch_base, ch_reg);
    if (ch >= 0) {
        if (ch_reg == 0x00U) { // Dn_CHCR
            write_phys32(ch_base, w);
            if (w & 0x0100U) { // Bit 8: STR (Start Transfer)
                auto* dmac = DmacVifController::active_instance();
                const u32 mod = (w >> 2) & 0x03U;
                // std::printf("[DMAC Start] ch=%d mod=%u w=0x%08X\n", ch, mod, w);
                if (mod == 0) { // Normal mode
                    const u32 madr = read_phys32(ch_base + 0x10U);
                    const u32 qwc  = read_phys32(ch_base + 0x20U) & 0xFFFFU;
                    if (dmac && qwc > 0) {
                        dmac->transfer_normal(static_cast<DmaChannel>(ch), madr, qwc);
                    }
                    write_phys32(ch_base + 0x10U, madr + qwc * 16U);
                    write_phys32(ch_base + 0x20U, 0);

                    // Normal mode: limpiar el bit STR en CHCR
                    write_phys32(ch_base, read_phys32(ch_base) & ~0x0100U);
                } else if (mod == 1) { // Source Chain mode
                    const u32 tadr = read_phys32(ch_base + 0x30U);
                    u32 last_tag_bits = 0;
                    if (dmac) {
                        const auto res = dmac->transfer_source_chain(static_cast<DmaChannel>(ch), tadr);
                        last_tag_bits = res.last_tag_bits;
                        write_phys32(ch_base + 0x30U, res.final_tadr);
                    }
                    write_phys32(ch_base + 0x20U, 0);

                    // Source Chain mode: limpiar STR (bit 8) y actualizar TAG (bits 16..31)
                    // con los bits del último DMAtag (incluyendo el flag IRQ en bit 31).
                    // Para finalización de cadena aseguramos bit 31 activo para que
                    // los manejadores de interrupción de finalización (p. ej. en libdma/Capcom)
                    // verifiquen correctamente la señalización de fin de paquete.
                    u32 chcr = read_phys32(ch_base);
                    chcr &= ~0x0100U; // Limpiar bit STR
                    chcr &= 0x0000FFFFU; // Limpiar campo TAG previo
                    chcr |= (last_tag_bits & 0xFFFF0000U);
                    write_phys32(ch_base, chcr);
                }

                // Marcar canal completado en D_STAT
                u32 d_stat = read_phys32(kDmacStatAddr) | (1U << ch);
                write_phys32(kDmacStatAddr, d_stat);

                // Señalizar canal DMAC pendiente para entrega al procesador
                dmac_pending_mask_ |= (1U << ch);
            }
            return true;
        } else if (ch_reg == 0x10U || ch_reg == 0x20U || ch_reg == 0x30U ||
                   ch_reg == 0x40U || ch_reg == 0x50U || ch_reg == 0x80U) {
            if (size_bytes == 2) {
                write_phys16(pa, static_cast<u16>(w));
            } else {
                write_phys32(pa, w);
            }
            return true;
        }
    }

    return false;
}

bool EEKernelRuntime::mmio_read(u32 vaddr, u32 size_bytes, u64& out_val) {
    const u32 pa = vaddr & 0x1FFFFFFFU;
    switch (pa) {
        case kIntcStatAddr:
            out_val = read_phys32(kIntcStatAddr);
            return true;
        case kIntcMaskAddr:
            out_val = read_phys32(kIntcMaskAddr);
            return true;
        case kGsCsrAddr:
            out_val = read_phys64(kGsCsrAddr);
            return true;
        case kDmacStatAddr:
            out_val = read_phys32(kDmacStatAddr);
            return true;
        case 0x1000E000U: // D_CTRL
            out_val = read_phys32(0x1000E000U);
            return true;
        default:
            break;
    }
    if (pa >= 0x12000000U && pa <= 0x120000C0U) {
        out_val = read_phys64(pa);
        return true;
    }
    return false;
}

bool EEKernelRuntime::mmio_write_trampoline(u32 vaddr, u32 size_bytes, u64 value) {
    if (auto* k = active_instance()) {
        return k->mmio_write(vaddr, size_bytes, value);
    }
    return false;
}

bool EEKernelRuntime::mmio_read_trampoline(u32 vaddr, u32 size_bytes, u64& out_val) {
    if (auto* k = active_instance()) {
        return k->mmio_read(vaddr, size_bytes, out_val);
    }
    return false;
}

void EEKernelRuntime::raise_intc(u32 cause) {
    write_phys32(kIntcStatAddr, read_phys32(kIntcStatAddr) | (1U << cause));

    if (cause != kIntcVblankStart) return;

    // GS_CSR: VSINT (bit 3) y alternancia del bit FIELD (bit 13) en cada VBlank
    u32 csr = read_phys32(kGsCsrAddr);
    csr |= 0x08U;
    csr ^= 0x2000U;
    write_phys32(kGsCsrAddr, csr);

    if (auto* gs = GsvkRenderer::active_instance()) {
        static u32 frame_dump_count = 0;
        ++frame_dump_count;
        std::string dump_name = "";
        if (frame_dump_count == 120 || (frame_dump_count % 300 == 0 && frame_dump_count <= 3600)) {
            dump_name = "build\\frame_" + std::to_string(frame_dump_count) + ".bmp";
        }
        gs->present_frame(dump_name, false);
    }


    // SetVSyncFlag: el kernel escribe flag0 = 1 y flag1 = contador de VBlank. El registro es de un
    // solo uso: los punteros suelen apuntar a variables locales de pila que dejan de ser válidas.
    ++vsync_counter_;
    if (vsync_flag0_addr_ != 0 && MemoryArena::is_valid_guest_range(vsync_flag0_addr_, sizeof(u32))) {
        MemoryArena::fast_write<u32>(fastmem_base_, vsync_flag0_addr_, 1U);
    }
    if (vsync_flag1_addr_ != 0 && MemoryArena::is_valid_guest_range(vsync_flag1_addr_, sizeof(u64))) {
        MemoryArena::fast_write<u64>(fastmem_base_, vsync_flag1_addr_, vsync_counter_);
    }
    vsync_flag0_addr_ = 0;
    vsync_flag1_addr_ = 0;
}

void EEKernelRuntime::poll_interrupts(EEContext& ctx) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const s64 t = now.QuadPart;

    if (!timing_started_) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpc_freq_     = freq.QuadPart;
        qpc_start_    = t;
        frame_period_ = pal_ ? (qpc_freq_ / 50) : (qpc_freq_ * 1001 / 60000);
        next_vstart_  = t + frame_period_;
        timing_started_ = true;
        return;
    }

    if (t >= next_vstart_) {
        if (t - next_vstart_ > 4 * frame_period_) {
            next_vstart_ = t; // el proceso estuvo detenido: no encadenar VBlanks atrasados
        }
        const s64 start = next_vstart_;
        raise_intc(kIntcVblankStart);
        ++vblank_count_;
        if (trace_ && (vblank_count_ == 1 || vblank_count_ % 300 == 0)) dump_state("VBlank");
        next_vstart_ += frame_period_;
        vblank_end_pending_ = true;
        next_vend_ = start + frame_period_ / 12; // el intervalo de VBlank dura ~8% de la trama
    }
    if (vblank_end_pending_ && t >= next_vend_) {
        vblank_end_pending_ = false;
        raise_intc(kIntcVblankEnd);
    }

    deliver_pending_intc(ctx);
}

void EEKernelRuntime::deliver_pending_intc(EEContext& ctx) {
    if (!cpu_int_enabled_) return; // DI: las interrupciones quedan pendientes en INTC_STAT

    for (u32 cause = 0; cause < kIntcCauseCount; ++cause) {
        const u32 bit  = 1U << cause;
        const u32 stat = read_phys32(kIntcStatAddr);
        const u32 mask = read_phys32(kIntcMaskAddr);
        if (!((stat & mask) & bit)) continue;

        // El kernel reconoce la interrupción (limpia el bit de STAT) antes de invocar los handlers
        write_phys32(kIntcStatAddr, stat & ~bit);

        if (intc_handlers_[cause].empty()) continue;
        const std::vector<GuestIrqHandler> handlers = intc_handlers_[cause];
        run_irq_handlers(ctx, cause, handlers);
    }

    // Entrega de canales DMAC completados (R5900 COP0 IP3)
    if (dmac_pending_mask_ != 0) {
        const u32 d_stat = read_phys32(kDmacStatAddr);
        for (u32 c = 0; c < kDmacCauseCount; ++c) {
            const u32 ch_bit  = 1U << c;
            const u32 ch_mask = 1U << (16U + c);
            if ((dmac_pending_mask_ & ch_bit) && (!dmac_handlers_[c].empty() || (d_stat & ch_mask))) {
                dmac_pending_mask_ &= ~ch_bit;
                if (!dmac_handlers_[c].empty()) {
                    const std::vector<GuestIrqHandler> handlers = dmac_handlers_[c];
                    // std::printf("[DMAC Irq] ch=%u handlers=%zu\n", c, handlers.size());
                    run_irq_handlers(ctx, c, handlers);
                }
            }
        }
    }

    // Comprobar alarmas activas
    for (s32 i = 0; i < kMaxAlarms; ++i) {
        if (alarms_[i].active && poll_counter_ >= alarms_[i].fire_counter) {
            alarms_[i].active = false;
            run_single_alarm(ctx, alarms_[i]);
        }
    }
}

void EEKernelRuntime::run_irq_handlers(EEContext& ctx, u32 cause, const std::vector<GuestIrqHandler>& handlers) {
    const EEContext saved = ctx;
    const bool saved_int_enabled = cpu_int_enabled_;
    const u32 irq_sp = (saved.gpr[29].uw[0] - 0x400U) & ~0xFU; // evita pisar la zona roja del hilo interrumpido

    in_irq_ = true;
    cpu_int_enabled_ = false; // el handler corre con interrupciones enmascaradas (EXL=1)

    for (const GuestIrqHandler& h : handlers) {
        ctx.gpr[4].sd[0]  = static_cast<s64>(cause);
        ctx.gpr[5].sd[0]  = static_cast<s64>(static_cast<s32>(h.arg));
        ctx.gpr[6].sd[0]  = static_cast<s64>(static_cast<s32>(saved.pc));
        ctx.gpr[7].sd[0]  = 0;
        ctx.gpr[28].sd[0] = static_cast<s64>(static_cast<s32>(h.gp));
        ctx.gpr[29].sd[0] = static_cast<s64>(static_cast<s32>(irq_sp));
        ctx.gpr[31].sd[0] = static_cast<s64>(static_cast<s32>(kIrqReturnSentinel));
        ctx.pc = h.func;

        constexpr u32 kMaxHandlerBlocks = 4000000U;
        u32 executed = 0;
        while (!ctx.halted && ctx.pc != kIrqReturnSentinel) {
            if (++executed > kMaxHandlerBlocks) {
                std::fprintf(stderr, "[EmotionLayer IRQ] Handler 0x%08X (causa %u) excedio %u bloques; abortado\n",
                    h.func, cause, kMaxHandlerBlocks);
                break;
            }
            if (!DynamicRecompCache::execute_or_compile(ctx)) break;
        }
    }

    const bool halted = ctx.halted;
    const s32 exit_code = ctx.exit_code;
    ctx = saved;
    ctx.halted = halted;
    ctx.exit_code = exit_code;

    cpu_int_enabled_ = saved_int_enabled;
    in_irq_ = false;

    if (irq_resched_pending_) {
        irq_resched_pending_ = false;
        const s32 next = pick_next_ready_thread();
        if (next >= 0 && !idle_wait_ && threads_[next]->priority < threads_[current_thid_]->priority) {
            schedule_next(EEThreadState::Ready);
        }
    }
}

void EEKernelRuntime::request_preempt() {
    if (in_irq_) {
        irq_resched_pending_ = true; // se aplicará al retornar del handler
    } else {
        schedule_next(EEThreadState::Ready);
    }
}

void EEKernelRuntime::dump_state(const char* reason) const {
    static const char* kState[] = { "Unused", "Dormant", "Ready", "Running", "Waiting" };
    std::fprintf(stderr, "[EmotionLayer TRACE] ---- %s (vblank=%llu, EI=%d, INTC_STAT=%08X INTC_MASK=%08X) ----\n",
        reason, static_cast<unsigned long long>(vblank_count_), cpu_int_enabled_ ? 1 : 0,
        hw_irq_enabled_ ? read_phys32(kIntcStatAddr) : 0U, hw_irq_enabled_ ? read_phys32(kIntcMaskAddr) : 0U);
    for (s32 i = 1; i < kMaxThreads; ++i) {
        const GuestThread& t = *threads_[i];
        if (t.state == EEThreadState::Unused) continue;
        std::fprintf(stderr, "  thread %2d  %-8s prio=%3d pc=0x%08X entry=0x%08X wakeups=%d%s\n",
            i, kState[static_cast<int>(t.state)], t.priority, t.ctx.pc, t.entry_pc, t.wakeup_cnt,
            i == current_thid_ ? "  <== actual" : "");
    }
    for (s32 s = 1; s < kMaxSemaphores; ++s) {
        const GuestSemaphore& sem = semaphores_[s];
        if (!sem.active) continue;
        std::fprintf(stderr, "  sema %2d count=%d/%d waiting=%zu\n", s, sem.count, sem.max_count, sem.wait_queue.size());
    }
    for (u32 c = 0; c < kIntcCauseCount; ++c) {
        for (const auto& h : intc_handlers_[c]) {
            std::fprintf(stderr, "  intc cause %u -> handler id=%d func=0x%08X gp=0x%08X\n", c, h.id, h.func, h.gp);
        }
    }
    for (u32 c = 0; c < kDmacCauseCount; ++c) {
        for (const auto& h : dmac_handlers_[c]) {
            std::fprintf(stderr, "  dmac cause %u -> handler id=%d func=0x%08X gp=0x%08X\n", c, h.id, h.func, h.gp);
        }
    }
    std::fflush(stderr);
}

} // namespace emotion
