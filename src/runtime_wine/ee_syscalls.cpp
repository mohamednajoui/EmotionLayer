#include "ee_syscalls.hpp"
#include "ee_kernel/kernel_runtime.hpp"
#include "ee_kernel/dmac_vif.hpp"
#include "iop_hle/sif_iop_hle.hpp"
#include "smc_veh_guard.hpp"
#include "memory_arena.hpp"
#include "diag.hpp"
#include "game_profile_hooks.hpp"
#include "../gsvk/gsvk_renderer.hpp"
#include "../common/game_profile.hpp"
#include <cstdio>
#include <filesystem>
#include <unordered_map>

namespace emotion {

namespace {
static std::unordered_map<u32, u32> s_sif_client_servers;
static u32 s_heap_end = 0x01F00000U;
static u32 s_sif_regs[32] = {
    0x00000000U,              // 0: SIF_REG_MAINADDR
    0x00000000U,              // 1
    0x00010000U,              // 2: SIF_REG_SUBADDR
    0x00070000U,              // 3: SIF_REG_MSFLAG
    0x00070000U,              // 4: SIF_REG_SMFLAG (SIF_STAT_BOOTEND | SIF_STAT_SIFINIT | SIF_STAT_CMDINIT)
    0
};
static u32 s_osd_config_val = 0;
}

void EESyscalls::reset() noexcept {
    s_sif_client_servers.clear();
    s_heap_end = 0x01F00000U;
    std::memset(s_sif_regs, 0, sizeof(s_sif_regs));
    s_sif_regs[0] = 0x00000000U;
    s_sif_regs[2] = 0x00010000U;
    s_sif_regs[3] = 0x00070000U;
    s_sif_regs[4] = 0x00070000U;
    s_osd_config_val = 0;
}

void EESyscalls::dispatch(EEContext& ctx) noexcept {
    const s32 raw_num = ctx.gpr[3].sw[0];
    const u32 sys_num = static_cast<u32>(raw_num < 0 ? -raw_num : raw_num);

    if (Diag::enabled()) Diag::get().on_syscall(sys_num, ctx.pc);
    auto* krnl = EEKernelRuntime::active_instance();
    auto* iop  = SifIopHle::active_instance();
    auto* dmac = DmacVifController::active_instance();
    auto* gs   = GsvkRenderer::active_instance();

    switch (sys_num) {
        case 0x02: { // SetGsCrt(interlace, display_mode, frame_mode)
            // display_mode: 2 = NTSC, 3 = PAL (determina la frecuencia de VBlank: 59.94 / 50 Hz)
            const u32 mode = ctx.gpr[5].uw[0];
            if (krnl && (mode == 2U || mode == 3U)) krnl->set_video_standard(mode == 3U);
            break;
        }
        case 0x04: { // Exit(status)
            ctx.exit_code = ctx.gpr[4].sw[0];
            ctx.halted    = true;
            break;
        }

        // --- Interrupciones: handlers y máscaras (INTC / DMAC) ---
        case 0x10: { // AddIntcHandler(cause, handler, next)  -> id del handler
            const s32 id = krnl ? krnl->sys_add_intc_handler(ctx.gpr[4].uw[0], ctx.gpr[5].uw[0],
                ctx.gpr[6].sw[0], 0, ctx.gpr[28].uw[0]) : 1;
            ctx.gpr[2].sd[0] = static_cast<s64>(id);
            break;
        }
        case 0x11: { // RemoveIntcHandler(cause, id)
            ctx.gpr[2].sd[0] = krnl ? krnl->sys_remove_intc_handler(ctx.gpr[4].uw[0], ctx.gpr[5].sw[0]) : 0;
            break;
        }
        case 0x12: { // AddDmacHandler(cause, handler, next)  -> id del handler
            const s32 id = krnl ? krnl->sys_add_dmac_handler(ctx.gpr[4].uw[0], ctx.gpr[5].uw[0],
                ctx.gpr[6].sw[0], 0, ctx.gpr[28].uw[0]) : 1;
            ctx.gpr[2].sd[0] = static_cast<s64>(id);
            break;
        }
        case 0x13: { // RemoveDmacHandler(cause, id)
            ctx.gpr[2].sd[0] = krnl ? krnl->sys_remove_dmac_handler(ctx.gpr[4].uw[0], ctx.gpr[5].sw[0]) : 0;
            break;
        }
        case 0x14:   // _EnableIntc(cause)
        case 0x1A: { // _iEnableIntc(cause)
            ctx.gpr[2].sd[0] = krnl ? krnl->sys_enable_intc(ctx.gpr[4].uw[0]) : 1;
            break;
        }
        case 0x15:   // _DisableIntc(cause)
        case 0x1B: { // _iDisableIntc(cause)
            ctx.gpr[2].sd[0] = krnl ? krnl->sys_disable_intc(ctx.gpr[4].uw[0]) : 1;
            break;
        }
        case 0x16:   // _EnableDmac(cause)
        case 0x1C: { // _iEnableDmac(cause)
            ctx.gpr[2].sd[0] = krnl ? krnl->sys_enable_dmac(ctx.gpr[4].uw[0]) : 1;
            break;
        }
        case 0x17:   // _DisableDmac(cause)
        case 0x1D: { // _iDisableDmac(cause)
            ctx.gpr[2].sd[0] = krnl ? krnl->sys_disable_dmac(ctx.gpr[4].uw[0]) : 1;
            break;
        }

        // --- Syscalls de Multihilo Cooperativo del Emotion Engine (Win32 Fibers) ---
        case 0x20: { // CreateThread(EEThreadParam* param)
            const s32 thid = krnl ? krnl->sys_create_thread(ctx.gpr[4].uw[0]) : -1;
            std::printf("[Syscall CreateThread] created thid=%d param=0x%08X\n", thid, ctx.gpr[4].uw[0]);
            std::fflush(stdout);
            ctx.gpr[2].sd[0] = static_cast<s64>(thid);
            break;
        }
        case 0x21: { // DeleteThread(s32 thid)
            const s32 res = krnl ? krnl->sys_delete_thread(ctx.gpr[4].sw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x22: { // StartThread(s32 thid, void* arg)
            std::printf("[Syscall StartThread] starting thid=%d arg=0x%08X\n", ctx.gpr[4].sw[0], ctx.gpr[5].uw[0]);
            std::fflush(stdout);
            const s32 res = krnl ? krnl->sys_start_thread(ctx.gpr[4].sw[0], ctx.gpr[5].uw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x23:   // ExitThread()
        case 0x24: { // ExitDeleteThread()
            if (krnl) krnl->sys_exit_thread();
            break;
        }
        case 0x29:   // ChangeThreadPriority(s32 thid, s32 prio)
        case 0x2A: { // iChangeThreadPriority(s32 thid, s32 prio)
            const s32 old_prio = krnl ? krnl->sys_change_thread_priority(ctx.gpr[4].sw[0], ctx.gpr[5].sw[0]) : 64;
            ctx.gpr[2].sd[0] = static_cast<s64>(old_prio);
            break;
        }
        case 0x2B: { // RotateThreadReadyQueue(s32 prio)
            const s32 res = krnl ? krnl->sys_rotate_thread_ready_queue(ctx.gpr[4].sw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x2F: { // GetThreadId()
            const s32 thid = krnl ? krnl->current_thread_id() : 1;
            ctx.gpr[2].sd[0] = static_cast<s64>(thid);
            break;
        }
        case 0x30:   // ReferThreadStatus(s32 thid, u32 status_vaddr)
        case 0x31: { // iReferThreadStatus(s32 thid, u32 status_vaddr)
            const s32 res = krnl ? krnl->sys_refer_thread_status(ctx.gpr[4].sw[0], ctx.gpr[5].uw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x32: { // SleepThread()
            const s32 res = krnl ? krnl->sys_sleep_thread() : 0;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x33:   // WakeupThread(s32 thid)
        case 0x34: { // iWakeupThread(s32 thid): variante de handler; la replanificación se aplaza al retornar del handler
            const s32 res = krnl ? krnl->sys_wakeup_thread(ctx.gpr[4].sw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x37:   // SuspendThread(s32 thid)
        case 0x38: { // iSuspendThread(s32 thid)
            const s32 res = krnl ? krnl->sys_suspend_thread(ctx.gpr[4].sw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x39:   // ResumeThread(s32 thid)
        case 0x3A: { // iResumeThread(s32 thid)
            const s32 res = krnl ? krnl->sys_resume_thread(ctx.gpr[4].sw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }

        case 0x3C: { // InitMainThread(gp, stack, stack_size, args, root)
            const u32 gp         = ctx.gpr[4].uw[0];
            const u32 stack_base = ctx.gpr[5].uw[0];
            const u32 stack_size = ctx.gpr[6].uw[0];
            if (gp != 0 && gp != 0xFFFFFFFFU) {
                ctx.gpr[28].ud[0] = static_cast<u64>(gp);
            }
            if (stack_base != 0xFFFFFFFFU && stack_base != 0) {
                ctx.gpr[29].ud[0] = static_cast<u64>(stack_base + stack_size - 0x10U);
                s_heap_end = stack_base;
            }
            ctx.gpr[2].ud[0] = ctx.gpr[29].ud[0];
            break;
        }
        case 0x3D: { // InitHeap(heap, heap_size)
            const u32 heap_base = ctx.gpr[4].uw[0];
            const s32 heap_size = ctx.gpr[5].sw[0];

            // 1. Validar que heap_base se encuentre en la memoria física de 32 MB (MainRam)
            const auto base_info = MemoryArena::translate_guest_addr(heap_base, 1);
            if (!base_info.is_valid || base_info.region != GuestMemoryRegion::MainRam) {
                ctx.gpr[2].sd[0] = -1;
                break;
            }

            // Identificar el segmento virtual (KUSEG 0x00.., KSEG0 0x80.., KSEG1 0xA0.., etc.)
            const u32 seg_base = heap_base & 0xE0000000U;
            const u32 default_end = seg_base | 0x01F00000U;

            if (heap_size < 0) {
                if (heap_size != -1) {
                    ctx.gpr[2].sd[0] = -1; // Solo heap_size == -1 es el modo automatico legal en libkernel
                    break;
                }
                // Modo automático: confina el heap hasta 0x01F00000 del segmento actual
                if (heap_base >= default_end) {
                    ctx.gpr[2].sd[0] = -1; // Sin espacio dentro de los 32 MB
                } else {
                    s_heap_end = default_end;
                    ctx.gpr[2].sd[0] = static_cast<s64>(static_cast<s32>(s_heap_end));
                }
            } else {
                // Modo explícito: validar desbordamiento aritmético de 32 bits y límite estricto de 32 MB
                const u64 total = static_cast<u64>(heap_base) + static_cast<u32>(heap_size);
                if (total > 0xFFFFFFFFULL) {
                    ctx.gpr[2].sd[0] = -1;
                    break;
                }
                if (heap_size > 0) {
                    const auto range_info = MemoryArena::translate_guest_addr(heap_base, static_cast<u32>(heap_size));
                    if (!range_info.is_valid || range_info.region != GuestMemoryRegion::MainRam) {
                        ctx.gpr[2].sd[0] = -1;
                        break;
                    }
                }
                s_heap_end = static_cast<u32>(total);
                ctx.gpr[2].sd[0] = static_cast<s64>(static_cast<s32>(s_heap_end));
            }
            break;
        }
        case 0x3E: { // EndOfHeap()
            if (s_heap_end == 0) s_heap_end = 0x01F00000U;
            ctx.gpr[2].sd[0] = static_cast<s64>(static_cast<s32>(s_heap_end));
            break;
        }

        // --- Syscalls de Semáforos del Emotion Engine ---
        case 0x40: { // CreateSema(EESemaParam* param)
            const s32 semid = krnl ? krnl->sys_create_sema(ctx.gpr[4].uw[0]) : -1;
            std::printf("[Syscall CreateSema] semid=%d param=0x%08X (thid=%d)\n",
                semid, ctx.gpr[4].uw[0], krnl ? krnl->current_thread_id() : -1);
            std::fflush(stdout);
            ctx.gpr[2].sd[0] = static_cast<s64>(semid);
            break;
        }
        case 0x41: { // DeleteSema(s32 semid)
            const s32 res = krnl ? krnl->sys_delete_sema(ctx.gpr[4].sw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x42:   // SignalSema(s32 semid)
        case 0x43: { // iSignalSema(s32 semid)
            static s32 s_sig_cnt = 0;
            if (s_sig_cnt++ < 30) {
                std::printf("[Syscall SignalSema] semid=%d (thid=%d)\n",
                    ctx.gpr[4].sw[0], krnl ? krnl->current_thread_id() : -1);
                std::fflush(stdout);
            }
            const s32 res = krnl ? krnl->sys_signal_sema(ctx.gpr[4].sw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x44: { // WaitSema(s32 semid)
            static s32 s_wait_cnt = 0;
            if (s_wait_cnt++ < 30) {
                std::printf("[Syscall WaitSema] semid=%d (thid=%d)\n",
                    ctx.gpr[4].sw[0], krnl ? krnl->current_thread_id() : -1);
                std::fflush(stdout);
            }
            const s32 res = krnl ? krnl->sys_wait_sema(ctx.gpr[4].sw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x45:   // PollSema(s32 semid)
        case 0x46: { // iPollSema(s32 semid)
            const s32 res = krnl ? krnl->sys_poll_sema(ctx.gpr[4].sw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0x4A: { // SetOsdConfigParam(void* addr)
            const u32 param_vaddr = ctx.gpr[4].uw[0];
            if (param_vaddr != 0 && ctx.fastmem_base && MemoryArena::is_valid_guest_range(param_vaddr, 4)) {
                s_osd_config_val = MemoryArena::fast_read<u32>(ctx.fastmem_base, param_vaddr);
            }
            ctx.gpr[2].sd[0] = 0;
            break;
        }
        case 0x4B: { // GetOsdConfigParam(void* addr)
            // En PS2 la configuracion OSD es un unico bitfield de 32 bits (4 bytes), NO 32 bytes.
            // Escribir 32 bytes machacaba los registros guardados en la pila ($ra, $s0) de la funcion llamadora!
            const u32 param_vaddr = ctx.gpr[4].uw[0];
            if (param_vaddr != 0 && ctx.fastmem_base && MemoryArena::is_valid_guest_range(param_vaddr, 4)) {
                MemoryArena::fast_write<u32>(ctx.fastmem_base, param_vaddr, s_osd_config_val);
            }
            ctx.gpr[2].sd[0] = 0;
            break;
        }
        case 0x5A: { // QueryBootMode
            ctx.gpr[2].ud[0] = 0;
            break;
        }
        case 0x5B: { // _SyncDCache
            ctx.gpr[2].sd[0] = 0;
            break;
        }

        case 0x64: { // FlushCache(mode)
            break;
        }
        case 0x6A: { // _DisableDmac(s32 dmac_id)
            ctx.gpr[2].sd[0] = 1;
            break;
        }
        case 0x6B: { // _EnableDmac(s32 dmac_id)
            ctx.gpr[2].sd[0] = 1;
            break;
        }
        case 0x70: { // GsGetIMT() -> IMR actual del GS
            ctx.gpr[2].ud[0] = krnl ? krnl->gs_get_imt() : 0;
            break;
        }
        case 0x71: { // GsPutIMT(imr) -> IMR anterior
            ctx.gpr[2].ud[0] = krnl ? krnl->sys_gs_put_imt(ctx.gpr[4].uw[0]) : 0;
            break;
        }
        case 0x72: { // SetPgifHandler(handler) -> sin PGIF (no es un TOOL de desarrollo)
            ctx.gpr[2].sd[0] = 0;
            break;
        }
        case 0x73: { // SetVSyncFlag(u32* flag0, u64* flag1)
            if (krnl) krnl->sys_set_vsync_flag(ctx.gpr[4].uw[0], ctx.gpr[5].uw[0]);
            ctx.gpr[2].sd[0] = 0;
            break;
        }
        case 0x74: { // SetSyscall(num, address) -> el juego no reemplaza syscalls del kernel
            ctx.gpr[2].sd[0] = 0;
            break;
        }
        case 0x75: { // _print(fmt, ...): salida de depuración del kernel, sin consumidor
            break;
        }
        case 0x76: { // sceSifStopDma()
            ctx.gpr[2].sd[0] = 0;
            break;
        }
        case 0x77: { // sceSifDmaTransfer()
            const u32 dma_vaddr = ctx.gpr[4].uw[0];
            const u32 count = ctx.gpr[5].uw[0];
            if (dma_vaddr == 0 || !ctx.fastmem_base) {
                ctx.gpr[2].ud[0] = 0;
                break;
            }
            const u32 safe_count = (count > 0 && count <= 32) ? count : 1;
            if (!MemoryArena::is_valid_guest_range(dma_vaddr, safe_count * 16)) {
                ctx.gpr[2].ud[0] = 0;
                break;
            }
            const u32* p = MemoryArena::get_guest_ptr<const u32>(ctx.fastmem_base, dma_vaddr, safe_count * 4);
            if (!p) {
                ctx.gpr[2].ud[0] = 0;
                break;
            }
            for (u32 i = 0; i < safe_count; ++i) {
                const u32 data_addr = p[i * 4 + 0];
                const u32 size      = p[i * 4 + 2];
                if (data_addr == 0 || size < 32) {
                    continue;
                }
                u32* pkt = MemoryArena::get_guest_ptr<u32>(ctx.fastmem_base, data_addr, size / 4);
                if (!pkt) {
                    continue;
                }
                const u32 cmd_id = pkt[2];
                if (cmd_id == 0x80000009U) { // SIF_CMD_RPC_BIND
                    const u32 server_id   = pkt[8];
                    const u32 client_addr = pkt[7];
                    std::printf("[SIF RPC BIND] Transfer %u/%u Client=0x%08X Server=0x%08X\n",
                        i, count, client_addr, server_id);
                    if (client_addr != 0 && MemoryArena::is_valid_guest_range(client_addr, 40)) {
                        s_sif_client_servers[client_addr] = server_id;
                        u32* serve_ptr = MemoryArena::get_guest_ptr<u32>(ctx.fastmem_base, client_addr + 0x24U, 1);
                        if (!GameProfileHooks::handle_sif_bind(ctx, client_addr, server_id, serve_ptr)) {
                            if (serve_ptr) {
                                *serve_ptr = (server_id ^ 0x12345678U) != 0 ? (server_id ^ 0x12345678U) : 0xDEADBEEFU;
                            }
                        }
                        const u32 client_pkt_addr = MemoryArena::fast_read<u32>(ctx.fastmem_base, client_addr);
                        if (client_pkt_addr != 0) {
                            u32* cp = MemoryArena::get_guest_ptr<u32>(ctx.fastmem_base, client_pkt_addr, 7);
                            if (cp) {
                                cp[4] &= ~1U;
                            }
                        }
                        const u32 sema_id = MemoryArena::fast_read<u32>(ctx.fastmem_base, client_addr + 8U);
                        if (krnl && sema_id > 0 && sema_id < EEKernelRuntime::kMaxSemaphores) {
                            krnl->sys_signal_sema(static_cast<s32>(sema_id));
                        }
                        MemoryArena::fast_write<u32>(ctx.fastmem_base, client_addr, 0);
                    }
                    pkt[4] &= ~1U;
                } else if (cmd_id == 0x8000000AU && size >= 48) { // SIF_CMD_RPC_CALL
                    const u32 rpc_func    = pkt[8];
                    const u32 client_addr = pkt[7];
                    const u32 user_rbuf   = pkt[10];
                    const u32 user_sbuf   = (i > 0) ? p[(i - 1) * 4 + 0] : 0;
                    u32* rbuf = MemoryArena::get_guest_ptr<u32>(ctx.fastmem_base, user_rbuf, 5);
                    u32* sbuf = MemoryArena::get_guest_ptr<u32>(ctx.fastmem_base, user_sbuf, 5);

                            u32 server_id = 0;
                            if (size >= 56 && pkt[13] != 0 && pkt[13] != 0xDEADBEEFU) {
                                server_id = pkt[13] ^ 0x12345678U;
                            }
                            if (server_id == 0) {
                                auto it = s_sif_client_servers.find(client_addr);
                                if (it != s_sif_client_servers.end()) server_id = it->second;
                            }

                            std::printf("[SIF RPC CALL] Transfer %u/%u Server=0x%08X Func=0x%08X Client=0x%08X Size=%u\n",
                                i, count, server_id, rpc_func, client_addr, size);

                            bool deferred_reply = false;
                            const bool hook_handled = GameProfileHooks::handle_sif_call(
                                ctx, client_addr, server_id, rpc_func, sbuf, user_sbuf, rbuf,
                                deferred_reply, krnl, iop
                            );

                            if (!hook_handled) {
                                // RUTA GENÉRICA SIF RPC (CERO HACKS DE JUEGO)
                                switch (server_id) {
                                    case 0x80000592: { // CDVD Init (cdvdfsv MODULE_ID_1)
                                        if (rpc_func == 0 && rbuf && pkt[11] >= 16) {
                                            rbuf[1] = 0x2550; // Versión cdvdfsv
                                            rbuf[2] = 0x2550; // Versión cdvdman
                                            rbuf[3] = 0xFF;   // Código de retorno OK
                                        }
                                        break;
                                    }
                                    case 0x80000593: { // CDVD S-Commands (cdvdfsv MODULE_ID_2)
                                        if (rpc_func == 1 && rbuf) {
                                            rbuf[0] = 0; // ReadClock
                                        } else if (rpc_func == 3 && rbuf) {
                                            *rbuf = 0x14U; // GetDiskType: PS2DVD
                                        } else if (rpc_func == 4 && rbuf) {
                                            *rbuf = 0x00U; // GetError: Sin error
                                        } else if (rpc_func == 5 && rbuf && pkt[11] >= 8) {
                                            rbuf[0] = 1; rbuf[1] = 0; // TrayReq
                                        } else if (rpc_func == 0x0C && rbuf) {
                                            *rbuf = 2; // Status: Paused / Idle
                                        } else if (rpc_func == 0x16 && rbuf) {
                                            *rbuf = 1; // Break: 1 = Exito
                                        } else if (rpc_func == 0x22 && rbuf) {
                                            *rbuf = 1; // SetMediaMode: 1 = Exito
                                        } else if (rpc_func == 0x27 && rbuf) {
                                            rbuf[0] = 1; // ReadDvdDualInfo
                                            if (pkt[11] >= 8) rbuf[1] = 0;
                                        } else if (rbuf) {
                                            *rbuf = 0;
                                        }
                                        break;
                                    }
                                    case 0x80000595: { // CDVD N-Commands (cdvdfsv MODULE_ID_4)
                                        if (rpc_func == 1) { // CdRead (asincrono diferido)
                                            bool queued = false;
                                            if (sbuf && iop) {
                                                const u32 lsn     = sbuf[0];
                                                const u32 sectors = sbuf[1];
                                                const u32 dst     = sbuf[2];
                                                const u32 sem_id  = (client_addr != 0 && MemoryArena::is_valid_guest_range(client_addr, 12))
                                                    ? MemoryArena::fast_read<u32>(ctx.fastmem_base, client_addr + 8U) : 0U;
                                                u32 client_pkt_addr = (client_addr != 0 && MemoryArena::is_valid_guest_range(client_addr, 4))
                                                    ? MemoryArena::fast_read<u32>(ctx.fastmem_base, client_addr) : 0U;
                                                if (client_pkt_addr == 0 && size >= 24) {
                                                    client_pkt_addr = pkt[5];
                                                }

                                                if (sectors > 0 && dst != 0 && MemoryArena::is_valid_guest_range(dst, sectors * 2048)) {
                                                    const u32 delay = 2U + sectors;
                                                    queued = iop->queue_cdvd_read(lsn, sectors, dst, client_addr, client_pkt_addr, sem_id, user_rbuf, pkt[11], delay);
                                                }
                                            }
                                            if (queued) {
                                                deferred_reply = true; // Respuesta diferida hasta completar timing o sincronizacion
                                                if (rbuf) *rbuf = 0;  // Encolado OK
                                            } else {
                                                deferred_reply = false;
                                                if (rbuf) *rbuf = 0xFFFFFFFFU; // Error al encolar (ocupado o parametros invalidos)
                                            }
                                        } else if (rpc_func == 4 && rbuf) {
                                            *rbuf = 1; // GetToc
                                        } else if (rpc_func == 0x0E && rbuf) { // NDiskReady (no bloqueante)
                                            *rbuf = iop ? iop->get_ndiskready_status() : 2U;
                                        } else if (rbuf) {
                                            *rbuf = 0;
                                        }
                                        break;
                                    }
                                    case 0x80000597: { // CDVD SearchFile (cdvdfsv MODULE_ID_6)
                                        if (rpc_func == 0 && sbuf && iop) {
                                            const u32 send_sz = pkt[9];
                                            const u32 path_off = (send_sz == 0x124) ? 0x20 : 0x24;
                                            bool ok = false;
                                            if (path_off < send_sz) {
                                                const u32 max_len = send_sz - path_off;
                                                const char* path_ptr = reinterpret_cast<const char*>(sbuf) + path_off;
                                                std::size_t len = 0;
                                                while (len < max_len && path_ptr[len] != '\0') {
                                                    len++;
                                                }
                                                std::string path_str(path_ptr, len);
                                                IsoFileEntry entry{};
                                                if (path_str.empty() || path_str == "\\" || path_str == "/") {
                                                    sbuf[0] = 23;
                                                    sbuf[1] = 2048;
                                                    ok = true;
                                                } else if (iop->iso_reader().find_file(path_str, entry)) {
                                                    sbuf[0] = entry.lba;
                                                    sbuf[1] = entry.size;
                                                    ok = true;
                                                }
                                            }
                                            if (rbuf) *rbuf = ok ? 1U : 0U;
                                        }
                                        break;
                                    }
                                    case 0x8000059A: case 0x8000059C: { // CDVD DiskReady blocking (MODULE_ID_7 / _8)
                                        // Play! Iop_Cdvdfsv.cpp:409: si hay comando pendiente, se espera y finaliza
                                        if (iop && iop->has_pending_command()) {
                                            iop->finish_pending_command(krnl);
                                        }
                                        if (rbuf) *rbuf = 2; // 2 = CDVD_DRV_READY
                                        break;
                                    }
                                    case 0x80000100: { // PADMAN
                                        if (rbuf && pkt[11] >= 20) {
                                            rbuf[0] = 1; rbuf[1] = 1; rbuf[2] = 1; rbuf[3] = 0x0401; rbuf[4] = 6;
                                        }
                                        break;
                                    }
                                    case 0x80000400: { // MCSERV
                                        if (rbuf && pkt[11] >= 12) {
                                            rbuf[0] = 0; rbuf[1] = 0x0210; rbuf[2] = 0x0214;
                                        }
                                        break;
                                    }
                                    default: {
                                        if (rbuf) *rbuf = 1;
                                        break;
                                    }
                                }
                            }

                            // Finalización común de la llamada RPC hacia el cliente del EE
                            if (!deferred_reply) {
                                if (client_addr != 0 && MemoryArena::is_valid_guest_range(client_addr, 40)) {
                                    u32 client_pkt_addr = MemoryArena::fast_read<u32>(ctx.fastmem_base, client_addr);
                                    if (client_pkt_addr == 0 && size >= 24) {
                                        client_pkt_addr = pkt[5];
                                    }
                                    if (client_pkt_addr != 0) {
                                        u32* cp = MemoryArena::get_guest_ptr<u32>(ctx.fastmem_base, client_pkt_addr, 7);
                                        if (cp) {
                                            cp[4] &= ~1U;
                                        }
                                    }
                                    const u32 sema_id = MemoryArena::fast_read<u32>(ctx.fastmem_base, client_addr + 8U);
                                    if (krnl && sema_id > 0 && sema_id < EEKernelRuntime::kMaxSemaphores) {
                                        krnl->sys_signal_sema(static_cast<s32>(sema_id));
                                    }
                                    const u32 end_func  = MemoryArena::fast_read<u32>(ctx.fastmem_base, client_addr + 0x1CU);
                                    const u32 end_param = MemoryArena::fast_read<u32>(ctx.fastmem_base, client_addr + 0x20U);
                                    if (krnl && end_func != 0 && end_param > 0 && end_param < EEKernelRuntime::kMaxSemaphores) {
                                        krnl->sys_signal_sema(static_cast<s32>(end_param));
                                    }
                                    MemoryArena::fast_write<u32>(ctx.fastmem_base, client_addr, 0);
                                }
                                pkt[4] &= ~1U;
                            }
                            std::fflush(stdout);
                        }
                    }
            ctx.gpr[2].sd[0] = 1; // SIF transaction id
            break;
        }
        case 0x78: { // sceSifSetDChain()
            ctx.gpr[2].sd[0] = 1;
            break;
        }
        case 0x79: { // sceSifSetReg(u32 reg, u32 val)
            const u32 r = ctx.gpr[4].uw[0] & 0x1FU;
            if (r == 3 || r == 4) {
                s_sif_regs[r] = ctx.gpr[5].uw[0] | 0x00070000U;
            } else if (r == 2) {
                s_sif_regs[2] = ctx.gpr[5].uw[0] != 0 ? ctx.gpr[5].uw[0] : 0x00010000U;
            } else {
                s_sif_regs[r] = ctx.gpr[5].uw[0];
            }
            ctx.gpr[2].sd[0] = 0;
            break;
        }
        case 0x7A: { // sceSifGetReg(u32 reg)
            const u32 r = ctx.gpr[4].uw[0] & 0x1FU;
            if (r == 3 || r == 4) {
                ctx.gpr[2].ud[0] = s_sif_regs[r] | 0x00070000U;
            } else if (r == 2) {
                ctx.gpr[2].ud[0] = (s_sif_regs[2] != 0) ? s_sif_regs[2] : 0x00010000U;
            } else {
                ctx.gpr[2].ud[0] = s_sif_regs[r];
            }
            break;
        }
        case 0x7B: { // sceSifSetSubAddr(u32 addr)
            s_sif_regs[2] = ctx.gpr[4].uw[0];
            ctx.gpr[2].sd[0] = 0;
            break;
        }
        case 0x7C: { // Deci2Call(func, params): interfaz de depuracion / stdio de la TOOL / Sony Libc
            const u32 func = ctx.gpr[4].uw[0];
            const u32 param_vaddr = ctx.gpr[5].uw[0];
            static u32 s_deci2_cookie = 0;
            if (func == 1) { // Deci2Open: socket descriptor 1
                if (param_vaddr != 0 && MemoryArena::is_valid_guest_range(param_vaddr, 8)) {
                    s_deci2_cookie = MemoryArena::fast_read<u32>(ctx.fastmem_base, param_vaddr + 4);
                }
                ctx.gpr[2].sd[0] = 1;
            } else if (func == 2) { // Deci2Close
                ctx.gpr[2].sd[0] = 0;
            } else if (func == 3) { // Deci2Send(socket, packet)
                const u32 fallback_addr = GameProfileHooks::get_deci2_fallback_addr();
                const u32 pkt_vaddr = (ctx.gpr[6].uw[0] != 0) ? ctx.gpr[6].uw[0] : fallback_addr;
                if (MemoryArena::is_valid_guest_range(pkt_vaddr, 12)) {
                    const u16 len = MemoryArena::fast_read<u16>(ctx.fastmem_base, pkt_vaddr);
                    if (len > 12 && len <= 4096) {
                        const char* text = MemoryArena::get_guest_ptr<const char>(ctx.fastmem_base, pkt_vaddr + 12, len - 12);
                        if (text) {
                            std::fwrite(text, 1, len - 12, stdout);
                            std::fflush(stdout);
                        }
                    }
                }
                if (s_deci2_cookie != 0) {
                    MemoryArena::fast_write<u32>(ctx.fastmem_base, s_deci2_cookie + 12, 0U);
                }
                ctx.gpr[2].sd[0] = 1;
            } else if (func == 4) { // Deci2Poll / Deci2Receive: no incoming debug packets
                if (s_deci2_cookie != 0) {
                    MemoryArena::fast_write<u32>(ctx.fastmem_base, s_deci2_cookie + 12, 0U);
                }
                ctx.gpr[2].sd[0] = 0;
            } else if (func == 16 && param_vaddr != 0 && MemoryArena::is_valid_guest_range(param_vaddr, 4)) {
                const u32 str_vaddr = MemoryArena::fast_read<u32>(ctx.fastmem_base, param_vaddr);
                std::string debug_str;
                if (MemoryArena::safe_read_string(ctx.fastmem_base, str_vaddr, debug_str, 1024)) {
                    std::fputs(debug_str.c_str(), stdout);
                    std::fflush(stdout);
                }
                ctx.gpr[2].sd[0] = 1;
            } else {
                ctx.gpr[2].sd[0] = 0;
            }
            break;
        }
        case 0x7F: { // GetMemorySize() -> 32 MB (0x02000000)
            ctx.gpr[2].ud[0] = 32 * 1024 * 1024;
            break;
        }

        // --- Syscalls Oficiales del Kernel de Emotion Engine (0x80..0x8F) ---
        case 0x82: { // _InitTLB()
            ctx.gpr[2].sd[0] = 0;
            break;
        }
        case 0x83: { // FindAddress(u32 start, u32 end, u32 target)
            u32 start = ctx.gpr[4].uw[0];
            u32 end   = ctx.gpr[5].uw[0];
            const u32 target = ctx.gpr[6].uw[0];
            const u32 target_deref = MemoryArena::is_valid_guest_range(target, 4)
                ? MemoryArena::fast_read<u32>(ctx.fastmem_base, target) : 0;
            std::printf("[Syscall FindAddress] start=0x%08X end=0x%08X target=0x%08X (*target=0x%08X)\n",
                start, end, target, target_deref);
            std::fflush(stdout);

            auto normalize_kseg = [](u32 addr) noexcept -> u32 {
                if (addr >= 0x80000000U && addr < 0xC0000000U) {
                    return addr & 0x1FFFFFFFU;
                }
                return addr;
            };

            const u32 target_norm = normalize_kseg(target);

            // Quirk específico de FindAddress para títulos con desreferenciación invertida (P3)
            u32 quirk_found = 0;
            if (GameProfileHooks::handle_find_address(ctx, start, end, target, target_deref, quirk_found)) {
                ctx.gpr[2].ud[0] = quirk_found;
                break;
            }

            // Búsqueda real en la memoria de 32 MB
            start = (start + 3U) & ~3U;
            end   &= ~3U;
            u32 found = 0;
            if (start < end) {
                for (u32 addr = start; addr < end; addr += 4) {
                    if (MemoryArena::is_valid_guest_range(addr, 4)) {
                        const u32 val = MemoryArena::fast_read<u32>(ctx.fastmem_base, addr);
                        if (val == target || normalize_kseg(val) == target_norm) {
                            found = addr;
                            break;
                        }
                    }
                }
            }
            ctx.gpr[2].ud[0] = static_cast<u64>(found);
            break;
        }
        case 0x85: { // SetMemoryMode()
            ctx.gpr[2].sd[0] = 0;
            break;
        }

        // --- Puente SIF RPC / Intercepción HLE de Módulos .IRX (Fase 2 Suite Sintética) ---
        case 0xE0: { // sceSifLoadModule(const char* path)
            const s32 mod_id = iop ? iop->sif_load_module(ctx.gpr[4].uw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(mod_id);
            break;
        }
        case 0xE1: { // sceCdSearchFile(Ps2CdlFile* fp, const char* path)
            const s32 ok = iop ? iop->cdvd_search_file(ctx.gpr[4].uw[0], ctx.gpr[5].uw[0]) : 0;
            ctx.gpr[2].sd[0] = static_cast<s64>(ok);
            break;
        }
        case 0xE2: { // sceCdRead(u32 lsn, u32 sectors, void* buf)
            const s32 ok = iop ? iop->cdvd_read(ctx.gpr[4].uw[0], ctx.gpr[5].uw[0], ctx.gpr[6].uw[0]) : 0;
            ctx.gpr[2].sd[0] = static_cast<s64>(ok);
            break;
        }
        case 0xE3: { // scePadPortOpen(s32 port, s32 slot, void* pad_area)
            const s32 ok = iop ? iop->pad_port_open(ctx.gpr[4].sw[0], ctx.gpr[5].sw[0], ctx.gpr[6].uw[0]) : 0;
            ctx.gpr[2].sd[0] = static_cast<s64>(ok);
            break;
        }
        case 0xE4: { // scePadRead(s32 port, s32 slot, u8* data32)
            const s32 bytes = iop ? iop->pad_read(ctx.gpr[4].sw[0], ctx.gpr[5].sw[0], ctx.gpr[6].uw[0]) : 0;
            ctx.gpr[2].sd[0] = static_cast<s64>(bytes);
            break;
        }
        case 0xE5: { // sceMcWriteFile(const char* mc_path, const void* src, u32 size)
            const s32 written = iop ? iop->mc_write_file(ctx.gpr[4].uw[0], ctx.gpr[5].uw[0], ctx.gpr[6].uw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(written);
            break;
        }
        case 0xE6: { // sceMcReadFile(const char* mc_path, void* dst, u32 max_size)
            const s32 read_b = iop ? iop->mc_read_file(ctx.gpr[4].uw[0], ctx.gpr[5].uw[0], ctx.gpr[6].uw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(read_b);
            break;
        }

        // --- Subsistema Gráfico GSVK, DMAC, VIF1 y VU1 (Fase 3) ---
        case 0x90: { // sceDmaSendN(DmaChannel ch, u32 madr, u32 qwc)
            const auto ch = static_cast<DmaChannel>(ctx.gpr[4].uw[0]);
            const u32 qwc = dmac ? dmac->transfer_normal(ch, ctx.gpr[5].uw[0], ctx.gpr[6].uw[0]) : 0;
            ctx.gpr[2].ud[0] = qwc;
            break;
        }
        case 0x91: { // sceDmaSend(DmaChannel ch, u32 tadr_source_chain)
            const auto ch = static_cast<DmaChannel>(ctx.gpr[4].uw[0]);
            const u32 qwc = dmac ? dmac->transfer_source_chain(ch, ctx.gpr[5].uw[0]).total_qwc : 0;
            ctx.gpr[2].ud[0] = qwc;
            break;
        }
        case 0x92: { // sceGsSyncV(const char* optional_bmp_dump_path)
            if (iop && iop->has_pending_command()) {
                iop->count_ticks(1, krnl);
            }
            if (gs) {
                const u32 path_vaddr = ctx.gpr[4].uw[0];
                std::string safe_bmp_path = "build\\gsvk_frame_output.bmp";
                if (path_vaddr != 0) {
                    std::string guest_bmp_str;
                    if (MemoryArena::safe_read_string(ctx.fastmem_base, path_vaddr, guest_bmp_str, 260)) {
                        std::filesystem::path p(guest_bmp_str);
                        std::string clean_name = p.filename().string();
                        if (!clean_name.empty()) {
                            if (!clean_name.ends_with(".bmp")) clean_name += ".bmp";
                            safe_bmp_path = "build\\" + clean_name;
                        }
                    }
                }
                gs->present_frame(safe_bmp_path.c_str(), false);
                ctx.gpr[2].ud[0] = gs->stats().lit_pixels_count;
                ctx.gpr[3].ud[0] = gs->stats().primitives_drawn;
            }
            break;
        }
        case 0x93: { // sceGsSetResolutionScale(u32 scale) (Fase 5)
            if (gs) {
                gs->set_resolution_scale(ctx.gpr[4].uw[0]);
                ctx.gpr[2].ud[0] = gs->resolution_scale();
            }
            break;
        }
        case 0x94: { // sceGsSetFrameLimit(u32 target_fps) (Fase 5)
            if (gs) {
                gs->set_target_framerate(ctx.gpr[4].uw[0]);
                ctx.gpr[2].ud[0] = gs->target_framerate();
            }
            break;
        }
        case 0x95: { // sceGsLoadHdTexture(u32 key_lo, u32 key_hi, u32 w, u32 h, u32 px_vaddr) (Fase 5)
            if (gs && ctx.gpr[8].uw[0] != 0) {
                const u64 key = (static_cast<u64>(ctx.gpr[5].uw[0]) << 32) | ctx.gpr[4].uw[0];
                const u32 w   = ctx.gpr[6].uw[0];
                const u32 h   = ctx.gpr[7].uw[0];
                const u32* px = MemoryArena::get_guest_ptr<const u32>(ctx.fastmem_base, ctx.gpr[8].uw[0], static_cast<std::size_t>(w) * h);
                if (px) {
                    gs->hd_textures().register_texture(key, w, h, px);
                }
                ctx.gpr[2].ud[0] = gs->hd_textures().loaded_count();
            }
            break;
        }
        case 0x96: { // sceGsGetEnhancementStats() -> $v0=scale, $v1=w, $a0=h, $a1=hd_samples (Fase 5)
            if (gs) {
                ctx.gpr[2].ud[0] = gs->resolution_scale();
                ctx.gpr[3].ud[0] = gs->render_width();
                ctx.gpr[4].ud[0] = gs->render_height();
                ctx.gpr[5].ud[0] = gs->hd_textures().samples_count();
            }
            break;
        }

        // --- Subsistema de Audio SPU2, Custom IRX y Overlays Dinámicos (Fase 4) ---
        case 0xA0: { // sceSpuUploadSample(u32 sram_addr, u32 src_vaddr, u32 bytes)
            const s32 written = iop ? iop->spu_upload_sample(ctx.gpr[4].uw[0], ctx.gpr[5].uw[0], ctx.gpr[6].uw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(written);
            break;
        }
        case 0xA1: { // sceSpuKeyOn(u32 voice, u32 sram_addr, u32 pitch, u32 vol_l, u32 vol_r)
            const s32 res = iop ? iop->spu_voice_key_on(ctx.gpr[4].uw[0], ctx.gpr[5].uw[0], static_cast<u16>(ctx.gpr[6].uw[0]), static_cast<s16>(ctx.gpr[7].sw[0]), static_cast<s16>(ctx.gpr[8].sw[0])) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0xA2: { // sceSpuExportWav(const char* wav_path, u32 frames)
            const s32 res = iop ? iop->spu_export_wav(ctx.gpr[4].uw[0], ctx.gpr[5].uw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }
        case 0xA3: { // sceIopRunCustomModule(const char* irx_path)
            const s32 ret = iop ? iop->run_custom_irx_module(ctx.gpr[4].uw[0]) : -1;
            ctx.gpr[2].sd[0] = static_cast<s64>(ret);
            break;
        }
        case 0xA4: { // sceDynamicOverlayStats() -> returns count of dynamically executed blocks
            ctx.gpr[2].ud[0] = DynamicRecompCache::dynamic_executions_count();
            break;
        }

        // --- Syscalls de Diagnóstico / Salida Nativa en Consola Win32 ---
        case 0xF0: { // Host Print String ($a0 = guest pointer to C-string)
            const u32 str_vaddr = ctx.gpr[4].uw[0];
            std::string host_str;
            if (MemoryArena::safe_read_string(ctx.fastmem_base, str_vaddr, host_str, 4096)) {
                std::fputs(host_str.c_str(), stdout);
                std::fflush(stdout);
            }
            break;
        }
        case 0xF1: { // Host Print Hex/Int ($a0 = label addr, $a1 = value)
            const u32 str_vaddr = ctx.gpr[4].uw[0];
            const u64 val       = ctx.gpr[5].ud[0];
            std::string label;
            if (MemoryArena::safe_read_string(ctx.fastmem_base, str_vaddr, label, 128)) {
                std::printf("%s0x%016llX (%lld)\n", label.c_str(), static_cast<unsigned long long>(val), static_cast<long long>(val));
                std::fflush(stdout);
            }
            break;
        }

        // --- Alarmas del Kernel de EE ---
        case 0xFC: { // SetAlarm(u16 time, u32 func, u32 arg) -> alarm_id
            const u16 time = static_cast<u16>(ctx.gpr[4].uw[0]);
            const u32 func = ctx.gpr[5].uw[0];
            const u32 arg  = ctx.gpr[6].uw[0];
            const u32 gp   = ctx.gpr[28].uw[0];
            const s32 id   = krnl ? krnl->sys_set_alarm(time, func, arg, gp) : 1;
            ctx.gpr[2].sd[0] = static_cast<s64>(id);
            break;
        }
        case 0xFD: { // ReleaseAlarm(s32 id) -> id
            const s32 id  = ctx.gpr[4].sw[0];
            const s32 res = krnl ? krnl->sys_release_alarm(id) : id;
            ctx.gpr[2].sd[0] = static_cast<s64>(res);
            break;
        }

        default: {
            // Syscall sin implementar: se registra una vez por número para detectar qué pide el juego.
            static u32 s_reported[8] = {};
            const u32 word = (sys_num >> 5) & 7U;
            const u32 bit  = 1U << (sys_num & 31U);
            if (!(s_reported[word] & bit)) {
                s_reported[word] |= bit;
                std::fprintf(stderr, "[EmotionLayer] Syscall no implementado: 0x%02X (raw %d) PC=0x%08X a0=0x%08X a1=0x%08X\n",
                    sys_num, raw_num, ctx.pc, ctx.gpr[4].uw[0], ctx.gpr[5].uw[0]);
            }
            break;
        }
    }
    ctx.gpr[0] = r128::zero();
}

} // namespace emotion
