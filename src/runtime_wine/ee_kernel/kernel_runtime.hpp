#pragma once

#include "../../common/types.hpp"
#include <vector>
#include <deque>
#include <memory>

namespace emotion {

// Estructura exacta de parámetros de hilo del Kernel del Emotion Engine (PS2 SDK <kernel.h>)
struct EEThreadParam {
    s32 status;
    u32 func;             // Dirección virtual de la función en el EE
    u32 stack;            // Base del stack en los 32 MB de RAM
    s32 stack_size;       // Tamaño del stack en bytes
    u32 gp_reg;           // Valor del registro $gp
    s32 initial_priority; // Prioridad inicial (1 = máxima, 127 = mínima)
    s32 current_priority;
    u32 attr;
    u32 option;
};

// Estructura exacta de parámetros de semáforo del Kernel del Emotion Engine (PS2 SDK <kernel.h>)
struct EESemaParam {
    s32 count;
    s32 max_count;
    s32 init_count;
    s32 wait_threads;
    u32 attr;
    u32 option;
};

enum class EEThreadState : u8 {
    Unused  = 0,
    Dormant = 1,
    Ready   = 2,
    Running = 3,
    Waiting = 4
};

struct GuestThread {
    s32           thid        = 0;
    EEThreadState state       = EEThreadState::Unused;
    s32           priority    = 64;
    s32           wakeup_cnt  = 0;
    u32           entry_pc    = 0;
    u32           stack_base  = 0;
    s32           stack_size  = 0;
    u32           gp_reg      = 0;
    void*         fiber       = nullptr;
    EEContext     ctx;
};

struct GuestSemaphore {
    bool              active     = false;
    s32               count      = 0;
    s32               max_count  = 1;
    s32               init_count = 0;
    u32               attr       = 0;
    u32               option     = 0;
    std::deque<s32>   wait_queue; // IDs de hilos esperando en este semáforo
};

// Handler de interrupción registrado con AddIntcHandler / AddDmacHandler.
// El kernel real guarda el $gp del registrante y lo restaura al invocar el handler.
struct GuestIrqHandler {
    s32 id   = 0;
    u32 func = 0;
    u32 gp   = 0;
    u32 arg  = 0;
};

// Alarma del Kernel de EE (SetAlarm / iSetAlarm / ReleaseAlarm / iReleaseAlarm)
struct GuestAlarm {
    s32  id           = 0;
    u16  time         = 0;
    u32  func         = 0;
    u32  arg          = 0;
    u32  gp           = 0;
    u64  fire_counter = 0;
    bool active       = false;
};

// Causas del controlador de interrupciones INTC del Emotion Engine (registro INTC_STAT/INTC_MASK)
enum IntcCause : u32 {
    kIntcGs          = 0,
    kIntcSbus        = 1,
    kIntcVblankStart = 2,
    kIntcVblankEnd   = 3,
    kIntcVif0        = 4,
    kIntcVif1        = 5,
    kIntcVu0         = 6,
    kIntcVu1         = 7,
    kIntcIpu         = 8,
    kIntcTimer0      = 9,
    kIntcTimer1      = 10,
    kIntcTimer2      = 11,
    kIntcTimer3      = 12,
    kIntcCauseCount  = 15
};

using EERecompiledRunnerFn = void(*)(EEContext& ctx);

// Planificador cooperativo determinista del Emotion Engine basado en Win32 Fibers
class EEKernelRuntime {
public:
    static constexpr u32 kThreadExitReturnSentinel = 0x000FFFF0U;
    static constexpr s32 kMaxThreads    = 64;
    static constexpr s32 kMaxSemaphores = 128;

    EEKernelRuntime() noexcept;
    ~EEKernelRuntime() noexcept;

    // Inicializa el hilo principal (thid = 1) convirtiendo el hilo actual de Windows en una Win32 Fiber
    bool initialize_main_fiber(u8* fastmem_base, u32 entry_point, EERecompiledRunnerFn runner_fn);
    void shutdown() noexcept;

    // Ejecuta el hilo principal hasta que el programa llame a Exit()
    s32 run_main_loop();

    // Acceso al contexto del hilo actualmente en ejecución
    EEContext& current_context() noexcept { return threads_[current_thid_]->ctx; }
    s32 current_thread_id() const noexcept { return current_thid_; }
    EEThreadState thread_state(s32 thid) const noexcept {
        if (thid >= 0 && thid < static_cast<s32>(threads_.size()) && threads_[thid]) {
            return threads_[thid]->state;
        }
        return EEThreadState::Unused;
    }

    // --- Syscalls de Hilos del Emotion Engine ---
    s32  sys_create_thread(u32 param_vaddr);
    s32  sys_delete_thread(s32 thid);
    s32  sys_start_thread(s32 thid, u32 arg_vaddr);
    void sys_exit_thread();
    s32  sys_rotate_thread_ready_queue(s32 prio);
    s32  sys_sleep_thread();
    s32  sys_wakeup_thread(s32 thid);
    s32  sys_change_thread_priority(s32 thid, s32 prio);
    s32  sys_suspend_thread(s32 thid);
    s32  sys_resume_thread(s32 thid);
    s32  sys_refer_thread_status(s32 thid, u32 status_vaddr);

    // --- Syscalls de Semáforos del Emotion Engine ---
    s32  sys_create_sema(u32 sema_param_vaddr);
    s32  sys_delete_sema(s32 semid);
    s32  sys_signal_sema(s32 semid);
    s32  sys_wait_sema(s32 semid);
    s32  sys_poll_sema(s32 semid);

    // --- Syscalls de Alarmas del Emotion Engine (0xFC / 0xFD / 0x3C / 0x3D) ---
    static constexpr s32 kMaxAlarms = 16;
    s32  sys_set_alarm(u16 time, u32 func, u32 arg, u32 gp);
    s32  sys_release_alarm(s32 id);
    void fire_expired_alarms(EEContext& ctx);
    void run_single_alarm(EEContext& ctx, const GuestAlarm& a);

    // --- Sistema de interrupciones (INTC / DMAC / VBlank) ---
    // Direcciones físicas de los registros MMIO modelados
    static constexpr u32 kIntcStatAddr = 0x1000F000U;
    static constexpr u32 kIntcMaskAddr = 0x1000F010U;
    static constexpr u32 kDmacStatAddr = 0x1000E010U; // D_STAT: bits 0..15 estado, bits 16..31 máscara
    static constexpr u32 kGsCsrAddr    = 0x12001000U;
    static constexpr u32 kDmacCauseCount = 16;
    static constexpr u32 kIrqReturnSentinel = 0x000FFFE0U;

    // Activa la generación de interrupciones de hardware (VBlank). Sólo debe llamarse cuando la
    // arena mapea los registros MMIO (0x1000F000 y 0x12001000), es decir con MemoryArena real.
    void enable_hw_interrupts() noexcept {
        hw_irq_enabled_ = true;
        write_phys32(0x1000E000U, 1U); // D_CTRL: DMAE = 1
        write_phys32(0x1000E010U, 0U); // D_STAT: inicializar
    }

    // Syscalls de gestión de handlers y máscaras
    s32  sys_add_intc_handler(u32 cause, u32 func, s32 next, u32 arg, u32 gp);
    s32  sys_remove_intc_handler(u32 cause, s32 id);
    s32  sys_add_dmac_handler(u32 cause, u32 func, s32 next, u32 arg, u32 gp);
    s32  sys_remove_dmac_handler(u32 cause, s32 id);
    s32  sys_enable_intc(u32 cause);
    s32  sys_disable_intc(u32 cause);
    s32  sys_enable_dmac(u32 cause);
    s32  sys_disable_dmac(u32 cause);
    void sys_set_vsync_flag(u32 flag0_vaddr, u32 flag1_vaddr);
    u32  sys_gs_put_imt(u32 imr);
    u32  gs_get_imt() const noexcept { return gs_imr_; }
    void set_video_standard(bool pal) noexcept;

    // Estado de interrupciones de la CPU (COP0 Status.EIE / IE) modelado por DI / EI / MTC0 / MFC0
    void cpu_set_int_enable(bool enabled) noexcept { cpu_int_enabled_ = enabled; }
    bool cpu_int_enabled() const noexcept { return cpu_int_enabled_; }
    u32  cop0_read(u32 reg) const noexcept;
    void cop0_write(u32 reg, u32 value) noexcept;

    // Escrituras y lecturas a registros MMIO con semántica de hardware (INTC_STAT, INTC_MASK, GS_CSR, DMAC).
    // Devuelve true si la operación fue consumida por el kernel; false si debe tratarse como memoria normal.
    bool mmio_write(u32 vaddr, u32 size_bytes, u64 value);
    bool mmio_read(u32 vaddr, u32 size_bytes, u64& out_val);

    static bool mmio_write_trampoline(u32 vaddr, u32 size_bytes, u64 value);
    static bool mmio_read_trampoline(u32 vaddr, u32 size_bytes, u64& out_val);

    // Llamado por el intérprete al inicio de cada bloque básico (estado de CPU consistente).
    // Si hay transferencias DMAC pendientes se entregan inmediatamente para no demorar
    // la sincronización de buffers; periódicamente consulta el reloj y genera VBlank.
    void on_block_boundary(EEContext& ctx) {
        if (!hw_irq_enabled_ || in_irq_) return;
        if (dmac_pending_mask_ != 0) {
            deliver_pending_intc(ctx);
        }
        if (((++poll_counter_) & 0x3FFU) == 0) {
            poll_interrupts(ctx);
        }
    }
    bool in_interrupt() const noexcept { return in_irq_; }

    // Diagnóstico: vuelca el estado de hilos, semáforos e interrupciones (activo con EMOTION_TRACE=1)
    void dump_state(const char* reason) const;
    bool trace_enabled() const noexcept { return trace_; }

    static EEKernelRuntime* active_instance() noexcept { return active_instance_; }
    static void set_active_instance(EEKernelRuntime* inst) noexcept { active_instance_ = inst; }
    static void register_instance(EEKernelRuntime* inst) noexcept;
    static void unregister_instance(EEKernelRuntime* inst) noexcept;

    using FiberConversionHook = void* (*)(void*, unsigned long);
    static void set_fiber_hook_for_testing(FiberConversionHook hook) noexcept { s_fiber_hook_ = hook; }

    using FiberCreateHook = void* (*)(std::size_t, std::size_t, unsigned long, void(__stdcall*)(void*), void*);
    static void set_fiber_create_hook_for_testing(FiberCreateHook hook) noexcept { s_fiber_create_hook_ = hook; }

private:
    void poll_interrupts(EEContext& ctx);
    void raise_intc(u32 cause);
    void deliver_pending_intc(EEContext& ctx);
    void run_irq_handlers(EEContext& ctx, u32 cause, const std::vector<GuestIrqHandler>& handlers);
    void request_preempt();
    u32  read_phys32(u32 addr) const noexcept;
    u64  read_phys64(u32 addr) const noexcept;
    void write_phys32(u32 addr, u32 value) noexcept;
    void write_phys64(u32 addr, u64 value) noexcept;
    void write_phys16(u32 addr, u16 value) noexcept;

    static void __stdcall fiber_entry_trampoline(void* lpParameter);
    void enqueue_ready(s32 thid);
    void remove_from_ready(s32 thid);
    s32  pick_next_ready_thread() const noexcept;
    void schedule_next(EEThreadState current_new_state);

    u8*                  fastmem_base_ = nullptr;
    EERecompiledRunnerFn runner_fn_    = nullptr;
    s32                  current_thid_ = 1;
    bool                 main_is_fiber_ = false;
    std::vector<std::unique_ptr<GuestThread>> threads_;
    std::deque<s32>      ready_queue_;
    GuestSemaphore       semaphores_[kMaxSemaphores];
    GuestAlarm           alarms_[kMaxAlarms] = {};

    // --- Estado del subsistema de interrupciones ---
    bool                          hw_irq_enabled_    = false;
    bool                          trace_             = false;
    u64                           vblank_count_      = 0;
    bool                          in_irq_            = false;
    bool                          idle_wait_         = false; // el kernel está en "idle": ningún hilo listo, avanzando el tiempo de hardware
    bool                          irq_resched_pending_ = false;
    bool                          cpu_int_enabled_   = true;
    u32                           poll_counter_      = 0;
    s32                           next_handler_id_   = 1;
    u32                           gs_imr_            = 0;
    std::vector<GuestIrqHandler>  intc_handlers_[kIntcCauseCount];
    std::vector<GuestIrqHandler>  dmac_handlers_[kDmacCauseCount];
    u32                           dmac_pending_mask_ = 0;

    // SetVSyncFlag: el kernel escribe flag0 = 1 y flag1 = contador en el siguiente VBlank
    u32 vsync_flag0_addr_ = 0;
    u32 vsync_flag1_addr_ = 0;
    u64 vsync_counter_    = 0;

    // Temporización de VBlank en tiempo real (QueryPerformanceCounter)
    bool timing_started_      = false;
    bool vblank_end_pending_  = false;
    bool pal_                 = false;
    s64  qpc_freq_            = 0;
    s64  qpc_start_           = 0;
    s64  next_vstart_         = 0;
    s64  next_vend_           = 0;
    s64  frame_period_        = 0;

    static FiberConversionHook    s_fiber_hook_;
    static FiberCreateHook        s_fiber_create_hook_;
    static std::vector<EEKernelRuntime*> instances_;
    static EEKernelRuntime*       active_instance_;
};

} // namespace emotion
