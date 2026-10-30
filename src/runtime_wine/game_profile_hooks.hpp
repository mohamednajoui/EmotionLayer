#pragma once

#include "../common/types.hpp"
#include <cstdint>

namespace emotion {

class EEKernelRuntime;
class SifIopHle;

// Encapsulamiento y aislamiento estricto de hacks/quirks de juegos específicos (P3).
// Todas las direcciones fijas de memoria RAM y trampas de PC de títulos comerciales
// residen exclusivamente en esta clase y quedan desactivadas (retornando false/0)
// para cualquier perfil que no coincida explícitamente con su SHA-256.
class GameProfileHooks {
public:
    // Aplica el quirk de SIF BIND si el perfil activo tiene requires_legacy_cdvd_pokes.
    // Retorna true si el quirk manejó el bind.
    static bool handle_sif_bind(EEContext& ctx, u32 client_addr, u32 server_id, u32* serve_ptr) noexcept;

    // Aplica el quirk de SIF CALL si el perfil activo tiene requires_legacy_cdvd_pokes.
    // Retorna true si el quirk manejó la llamada.
    static bool handle_sif_call(
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
    ) noexcept;

    // Retorna la dirección de buffer fallback para Deci2Send si el perfil requiere deci2_fallback.
    static u32 get_deci2_fallback_addr() noexcept;

    // Aplica el quirk de FindAddress si el perfil requiere findaddress_rodata.
    // Retorna true si el quirk resolvió la dirección.
    static bool handle_find_address(
        EEContext& ctx,
        u32 start,
        u32 end,
        u32 target,
        u32 target_deref,
        u32& out_found_addr
    ) noexcept;

    // Aplica el trap de PCs específicos si el perfil requiere smc_pc_traps.
    // Retorna true si se activó algún trap.
    static bool handle_smc_pc_trap(EEContext& ctx, EEKernelRuntime* krnl) noexcept;
};

} // namespace emotion
