#include "game_profile_hooks.hpp"
#include "memory_arena.hpp"
#include "../common/game_profile.hpp"
#include "ee_kernel/kernel_runtime.hpp"
#include "iop_hle/sif_iop_hle.hpp"
#include <cstdio>
#include <cstring>

namespace emotion {

bool GameProfileHooks::handle_sif_bind(EEContext& ctx, u32 client_addr, u32 server_id, u32* serve_ptr) noexcept {
    (void)ctx;
    (void)server_id;
    if (!GameProfileRegistry::active_profile().requires_legacy_cdvd_pokes) {
        return false;
    }

    // Quirk específico de Marvel vs. Capcom 2 (SLES-511.74): direcciones fijas de cliente CDVD
    if (client_addr == 0x004C7C40U || client_addr == 0x0043B9D0U) {
        if (serve_ptr) *serve_ptr = 0x02U;
        return true;
    }
    return false;
}

bool GameProfileHooks::handle_sif_call(
    EEContext& ctx,
    u32 client_addr,
    u32 server_id,
    u32 rpc_func,
    u32* sbuf,
    u32 user_sbuf,
    u32* rbuf,
    bool& out_deferred_reply,
    EEKernelRuntime* krnl,
    SifIopHle* iop
) noexcept {
    out_deferred_reply = false;
    (void)server_id;
    if (!GameProfileRegistry::active_profile().requires_legacy_cdvd_pokes) {
        return false;
    }

    // Aislamiento completo de direcciones de RAM hardcodeadas para Capcom / MvC2
    if (client_addr == 0x004C7C40U || client_addr == 0x0043B9D0U) {
        if (rpc_func == 1 || rpc_func == 3 || rpc_func == 4) { // sceCdRead
            const u32 read_sbuf = sbuf ? user_sbuf : 0x004C7D00U;
            if (read_sbuf != 0) {
                const u32* rparams = MemoryArena::get_guest_ptr<const u32>(ctx.fastmem_base, read_sbuf, 4);
                if (rparams) {
                    const u32 lsn     = rparams[0];
                    const u32 sectors = rparams[1];
                    const u32 dst     = rparams[2];
                    if (iop && sectors > 0 && dst != 0 && MemoryArena::is_valid_guest_range(dst, sectors * 2048)) {
                        iop->cdvd_read(lsn, sectors, dst);
                    }
                }
            }
        }
        if (rbuf) *rbuf = 0x00U; // SCECdCOMPLETE
        MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x004C7C80U, 0x02U);
        MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x0043A830U, 0U);
        MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x0043A854U, 0U);
        if (krnl) {
            const u32 sem4 = MemoryArena::fast_read<u32>(ctx.fastmem_base, 0x0043A828U);
            if (sem4 > 0 && sem4 < EEKernelRuntime::kMaxSemaphores) krnl->sys_signal_sema(sem4);
            const u32 sem_th = MemoryArena::fast_read<u32>(ctx.fastmem_base, 0x0043A820U);
            if (sem_th > 0 && sem_th < EEKernelRuntime::kMaxSemaphores) krnl->sys_signal_sema(sem_th);
        }
    } else if (client_addr == 0x0043C248U) { // CDVD S-Command
        if (rpc_func == 4) {
            if (rbuf) *rbuf = 0x00U;
        } else if (rpc_func == 22 || rpc_func == 0x16) {
            if (rbuf) *rbuf = 0x14U;
        } else if (rpc_func == 34 || rpc_func == 0x22) {
            if (rbuf) *rbuf = 0x01U;
        } else {
            if (rbuf) *rbuf = 0x00U;
        }
        MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x0043A830U, 0U);
        MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x0043A854U, 0U);
        if (krnl) {
            const u32 sem4 = MemoryArena::fast_read<u32>(ctx.fastmem_base, 0x0043A828U);
            if (sem4 > 0 && sem4 < EEKernelRuntime::kMaxSemaphores) krnl->sys_signal_sema(sem4);
            const u32 sem5 = MemoryArena::fast_read<u32>(ctx.fastmem_base, 0x0043A82CU);
            if (sem5 > 0 && sem5 < EEKernelRuntime::kMaxSemaphores) krnl->sys_signal_sema(sem5);
        }
    } else if (client_addr == 0x004C97C0U) { // sceCdSearchFile
        const u32 search_sbuf = sbuf ? user_sbuf : 0x004C9640U;
        s32 ok = 0;
        if (iop && search_sbuf != 0 && MemoryArena::is_valid_guest_range(search_sbuf, 300)) {
            ok = iop->cdvd_search_file(search_sbuf, search_sbuf + 36);
        }
        if (rbuf) *rbuf = (ok > 0) ? 1U : 0U;
        MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x004C9780U, (ok > 0) ? 1U : 0U);
        MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x0043A830U, 0U);
        MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x0043A854U, 0U);
        if (krnl) {
            const u32 sem4 = MemoryArena::fast_read<u32>(ctx.fastmem_base, 0x0043A828U);
            if (sem4 > 0 && sem4 < EEKernelRuntime::kMaxSemaphores) krnl->sys_signal_sema(sem4);
        }
    } else if (client_addr == 0x004C9880U) { // LIBMC
        if (rbuf) {
            rbuf[0] = 0x00U;
            if (rpc_func == 254 || rpc_func == 0xFE) {
                rbuf[1] = 0x0210U;
                rbuf[2] = 0x0214U;
            }
        }
    } else if (client_addr == 0x004C9300U) { // LIBPAD
        if (rbuf) {
            rbuf[0] = 1U;
            rbuf[1] = 1U;
            rbuf[2] = 1U;
            rbuf[3] = 0x0401U;
            rbuf[4] = 6U;
        }
    } else if (client_addr == 0x00634FF0U || client_addr == 0x00635020U) { // Capcom TSD Sound
        if (rbuf) *rbuf = 0x00U;
    } else {
        if (rbuf) *rbuf = 0x01U;
    }

    MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x004C8028U, 0x30353532U);
    MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x004C7C80U, 0x02U);
    u32* cdvd_res = MemoryArena::get_guest_ptr<u32>(ctx.fastmem_base, 0x004C7E00U, 4);
    if (cdvd_res) {
        cdvd_res[0] = 0x30353532U;
        cdvd_res[1] = 0x100000;
    }
    MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x0043A830U, 0U);
    MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x0043A854U, 0U);
    u32* stat_table = MemoryArena::get_guest_ptr<u32>(ctx.fastmem_base, 0x004C4B80U, 32);
    if (stat_table) {
        for (int s = 0; s < 32; ++s) stat_table[s] = 1;
    }
    return true;
}

u32 GameProfileHooks::get_deci2_fallback_addr() noexcept {
    if (GameProfileRegistry::active_profile().requires_deci2_fallback) {
        return 0x004C4680U;
    }
    return 0U;
}

bool GameProfileHooks::handle_find_address(
    EEContext& ctx,
    u32 start,
    u32 end,
    u32 target,
    u32 target_deref,
    u32& out_found_addr
) noexcept {
    (void)ctx;
    (void)end;
    if (!GameProfileRegistry::active_profile().requires_findaddress_rodata) {
        return false;
    }
    if ((target_deref == 0x8C820000U || (target & 0xFFFFU) == 0xCB10U) && (start & 0x80000000U)) {
        out_found_addr = 0x80001000U + 524U;
        return true;
    }
    if ((target_deref == 0x00063082U || (target & 0xFFFFU) == 0xCAD8U) && (start & 0x80000000U)) {
        out_found_addr = 0x80001000U + 360U;
        return true;
    }
    return false;
}

bool GameProfileHooks::handle_smc_pc_trap(EEContext& ctx, EEKernelRuntime* krnl) noexcept {
    if (!GameProfileRegistry::active_profile().requires_smc_pc_traps) {
        return false;
    }
    if (ctx.pc == 0x0010F148U || ctx.pc == 0x0010EC08U || ctx.pc == 0x0040A8A8U) {
        MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x0043A830U, 0U); // CDVD drive idle
        MemoryArena::fast_write<u32>(ctx.fastmem_base, 0x0043A854U, 0U); // CDVD status OK
        if (krnl) {
            const u32 sem4 = MemoryArena::fast_read<u32>(ctx.fastmem_base, 0x0043A828U);
            if (sem4 > 0 && sem4 < EEKernelRuntime::kMaxSemaphores) {
                krnl->sys_signal_sema(sem4);
            }
        }
        return true;
    }
    return false;
}

} // namespace emotion
