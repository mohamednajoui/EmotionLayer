#pragma once

#include "../common/types.hpp"
#include "../lifter/r5900_decoder.hpp"
#include <unordered_map>
#include <vector>
#include <string>
#include <functional>

namespace emotion {

struct RecompiledBlock {
    u32 start_pc   = 0;
    u32 end_pc     = 0;
    u32 insn_count = 0;
    u64 code_hash  = 0;
    std::vector<DecodedInsn> insns;
};

using BlockExecFn = void (*)(EEContext& ctx);

// Demonio de Recompilación en Caliente y Caché de Overlays Dinámicos
// Maneja código cargado dinámicamente desde el DVD / packers en RAM
class DynamicRecompCache {
public:
    DynamicRecompCache() noexcept = default;
    ~DynamicRecompCache() noexcept = default;

    // Resuelve o recompila al vuelo un bloque MIPS en ctx.pc
    // Retorna true si ejecutó el bloque y actualizó ctx.pc
    static bool execute_or_compile(EEContext& ctx);

    // Invalida bloques cacheados en un rango de memoria (tras FlushCache syscall)
    static void invalidate_range(u32 start_vaddr, u32 size_bytes);

    static u32 compiled_blocks_count() noexcept { return static_cast<u32>(blocks_.size()); }
    static u32 dynamic_executions_count() noexcept { return executions_count_; }

    static void reset();

    static RecompiledBlock analyze_block(const EEContext& ctx, u32 pc);
    static void execute_block_interp(const RecompiledBlock& block, EEContext& ctx);

private:
    static std::unordered_map<u32, RecompiledBlock> blocks_;
    static u32 executions_count_;
};

} // namespace emotion
