#pragma once

#include "../common/types.hpp"
#include <string>
#include <vector>
#include <functional>

namespace emotion {

struct alignas(16) IopContext {
    u32 gpr[32];
    u32 pc;
    u32 hi;
    u32 lo;
    bool halted;
    s32 exit_code;
    u8* iop_ram; // 2 MB de memoria del IOP

    void reset(u8* ram, u32 entry = 0x00010000) {
        std::memset(gpr, 0, sizeof(gpr));
        pc = entry;
        hi = 0; lo = 0;
        halted = false;
        exit_code = 0;
        iop_ram = ram;
        gpr[29] = 0x001FFFC0U; // Stack pointer al final de los 2 MB de RAM del IOP
    }
};

// Recompilador y Ejecutor de Módulos .IRX Personalizados (MIPS R3000A)
class IopR3000Lifter {
public:
    static constexpr u32 kIopRamSize = 2 * 1024 * 1024; // 2 MB RAM del IOP

    IopR3000Lifter() noexcept;
    ~IopR3000Lifter() noexcept = default;

    // Carga un módulo .IRX (ELF MIPS I) en la memoria del IOP
    bool load_irx(const std::vector<u8>& irx_bytes, u32 load_addr, u32& out_entry, std::string& out_error);

    // Ejecuta el módulo .IRX hasta su retorno o inicialización
    s32 execute(u32 entry_pc, s32 argc = 0, u32 argv_ptr = 0);

    // Decompila el módulo .IRX a código fuente C++20 nativo
    std::string decompile_to_cpp(u32 start_addr, u32 size_bytes) const;

    IopContext& context() noexcept { return ctx_; }
    u8* ram() noexcept { return iop_ram_.data(); }

private:
    std::vector<u8> iop_ram_;
    IopContext ctx_;
};

} // namespace emotion
