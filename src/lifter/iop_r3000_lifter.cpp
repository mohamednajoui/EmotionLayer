#include "iop_r3000_lifter.hpp"
#include "../extractor/elf_analyzer.hpp"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace emotion {

IopR3000Lifter::IopR3000Lifter() noexcept {
    iop_ram_.assign(kIopRamSize, 0);
    ctx_.reset(iop_ram_.data());
}

bool IopR3000Lifter::load_irx(const std::vector<u8>& irx_bytes, u32 load_addr, u32& out_entry, std::string& out_error) {
    if (irx_bytes.size() < sizeof(Elf32_Ehdr)) {
        out_error = "Tamano de archivo IRX invalido.";
        return false;
    }

    Elf32_Ehdr ehdr{};
    std::memcpy(&ehdr, irx_bytes.data(), sizeof(Elf32_Ehdr));
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F') {
        out_error = "Cabecera ELF no valida en modulo IRX.";
        return false;
    }

    out_entry = (ehdr.e_entry != 0) ? ehdr.e_entry : load_addr;

    // Cargar segmentos PT_LOAD en la RAM del IOP
    if (ehdr.e_phoff > 0 && ehdr.e_phnum > 0) {
        for (u16 i = 0; i < ehdr.e_phnum; ++i) {
            const std::size_t off = ehdr.e_phoff + static_cast<std::size_t>(i) * ehdr.e_phentsize;
            if (off + sizeof(Elf32_Phdr) > irx_bytes.size()) break;

            Elf32_Phdr phdr{};
            std::memcpy(&phdr, irx_bytes.data() + off, sizeof(Elf32_Phdr));
            if (phdr.p_type != ElfAnalyzer::kPT_LOAD || phdr.p_memsz == 0) continue;

            u32 dst_vaddr = phdr.p_vaddr ? phdr.p_vaddr : load_addr;
            if (dst_vaddr + phdr.p_memsz > kIopRamSize) {
                out_error = "El modulo IRX excede los 2 MB de memoria del IOP.";
                return false;
            }

            std::memset(iop_ram_.data() + dst_vaddr, 0, phdr.p_memsz);
            if (phdr.p_filesz > 0 && phdr.p_offset + phdr.p_filesz <= irx_bytes.size()) {
                std::memcpy(iop_ram_.data() + dst_vaddr, irx_bytes.data() + phdr.p_offset, phdr.p_filesz);
            }
        }
    } else {
        // Fallback: cargar plano si no tiene headers
        if (load_addr + irx_bytes.size() <= kIopRamSize) {
            std::memcpy(iop_ram_.data() + load_addr, irx_bytes.data(), irx_bytes.size());
        }
    }

    return true;
}

s32 IopR3000Lifter::execute(u32 entry_pc, s32 argc, u32 argv_ptr) {
    ctx_.reset(iop_ram_.data(), entry_pc);
    ctx_.gpr[4] = static_cast<u32>(argc);     // $a0 = argc
    ctx_.gpr[5] = argv_ptr;                   // $a1 = argv
    ctx_.gpr[31] = 0x00000000U;               // $ra = 0 (retorno)

    u32 steps = 0;
    while (!ctx_.halted && steps < 100000) {
        steps++;
        if (ctx_.pc == 0x00000000U) {
            // Retorno exitoso de la función principal del módulo
            break;
        }

        const u32 cur_pc = ctx_.pc;
        if (cur_pc + 4 > kIopRamSize) {
            ctx_.halted = true;
            break;
        }

        u32 raw = 0;
        std::memcpy(&raw, iop_ram_.data() + cur_pc, 4);

        const u8 opcode = static_cast<u8>((raw >> 26) & 0x3FU);
        const u8 rs     = static_cast<u8>((raw >> 21) & 0x1FU);
        const u8 rt     = static_cast<u8>((raw >> 16) & 0x1FU);
        const u8 rd     = static_cast<u8>((raw >> 11) & 0x1FU);
        const u8 sa     = static_cast<u8>((raw >> 6)  & 0x1FU);
        const u8 funct  = static_cast<u8>(raw & 0x3FU);
        const s16 simm  = static_cast<s16>(raw & 0xFFFFU);
        const u16 uimm  = static_cast<u16>(raw & 0xFFFFU);

        ctx_.pc += 4;

        auto read_delay = [&]() -> u32 {
            u32 delay_raw = 0;
            if (ctx_.pc + 4 <= kIopRamSize) {
                std::memcpy(&delay_raw, iop_ram_.data() + ctx_.pc, 4);
            }
            return delay_raw;
        };

        switch (opcode) {
            case 0x00: { // SPECIAL
                switch (funct) {
                    case 0x00: // SLL
                        if (rd) ctx_.gpr[rd] = ctx_.gpr[rt] << sa;
                        break;
                    case 0x02: // SRL
                        if (rd) ctx_.gpr[rd] = ctx_.gpr[rt] >> sa;
                        break;
                    case 0x08: { // JR
                        const u32 target = ctx_.gpr[rs];
                        const u32 delay = read_delay();
                        ctx_.pc = target;
                        if (delay != 0) {
                            // Ejecutar delay slot básico
                            const u8 dop = static_cast<u8>((delay >> 26) & 0x3FU);
                            const u8 drs = static_cast<u8>((delay >> 21) & 0x1FU);
                            const u8 drt = static_cast<u8>((delay >> 16) & 0x1FU);
                            const s16 dsimm = static_cast<s16>(delay & 0xFFFFU);
                            if (dop == 0x09 && drt) { // ADDIU
                                ctx_.gpr[drt] = ctx_.gpr[drs] + static_cast<u32>(static_cast<s32>(dsimm));
                            }
                        }
                        break;
                    }
                    case 0x0C: // SYSCALL
                        break;
                    case 0x20: // ADD
                    case 0x21: // ADDU
                        if (rd) ctx_.gpr[rd] = ctx_.gpr[rs] + ctx_.gpr[rt];
                        break;
                    case 0x22: // SUB
                    case 0x23: // SUBU
                        if (rd) ctx_.gpr[rd] = ctx_.gpr[rs] - ctx_.gpr[rt];
                        break;
                    case 0x24: // AND
                        if (rd) ctx_.gpr[rd] = ctx_.gpr[rs] & ctx_.gpr[rt];
                        break;
                    case 0x25: // OR
                        if (rd) ctx_.gpr[rd] = ctx_.gpr[rs] | ctx_.gpr[rt];
                        break;
                    default:
                        break;
                }
                break;
            }
            case 0x08: // ADDI
            case 0x09: // ADDIU
                if (rt) ctx_.gpr[rt] = ctx_.gpr[rs] + static_cast<u32>(static_cast<s32>(simm));
                break;
            case 0x0C: // ANDI
                if (rt) ctx_.gpr[rt] = ctx_.gpr[rs] & static_cast<u32>(uimm);
                break;
            case 0x0D: // ORI
                if (rt) ctx_.gpr[rt] = ctx_.gpr[rs] | static_cast<u32>(uimm);
                break;
            case 0x0F: // LUI
                if (rt) ctx_.gpr[rt] = static_cast<u32>(uimm) << 16;
                break;
            case 0x23: { // LW
                const u32 addr = ctx_.gpr[rs] + static_cast<u32>(static_cast<s32>(simm));
                if (addr + 4 <= kIopRamSize && rt) {
                    std::memcpy(&ctx_.gpr[rt], iop_ram_.data() + addr, 4);
                }
                break;
            }
            case 0x2B: { // SW
                const u32 addr = ctx_.gpr[rs] + static_cast<u32>(static_cast<s32>(simm));
                if (addr + 4 <= kIopRamSize) {
                    std::memcpy(iop_ram_.data() + addr, &ctx_.gpr[rt], 4);
                }
                break;
            }
            default:
                break;
        }

        ctx_.gpr[0] = 0; // $zero siempre es 0
    }

    return static_cast<s32>(ctx_.gpr[2]); // Retorno en $v0
}

std::string IopR3000Lifter::decompile_to_cpp(u32 start_addr, u32 size_bytes) const {
    std::ostringstream ss;
    ss << "// Modulo IOP MIPS R3000A decompilado a C++20 nativo\n";
    ss << "void execute_iop_module(IopContext& ctx) {\n";
    for (u32 pc = start_addr; pc < start_addr + size_bytes && pc + 4 <= kIopRamSize; pc += 4) {
        u32 raw = 0;
        std::memcpy(&raw, iop_ram_.data() + pc, 4);
        ss << "  /* 0x" << std::hex << std::setw(8) << std::setfill('0') << pc << " */ insn_0x" << raw << ";\n";
    }
    ss << "}\n";
    return ss.str();
}

} // namespace emotion
