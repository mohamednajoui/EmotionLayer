#include "cpp_emitter.hpp"
#include <fstream>
#include <sstream>
#include <set>
#include <iomanip>
#include <cstdlib>

namespace emotion {

std::string CppEmitter::emit_branch_condition(const DecodedInsn& insn) {
    char buf[256];
    switch (insn.op) {
        case R5900Op::J:
        case R5900Op::JAL:
        case R5900Op::JR:
        case R5900Op::JALR:
            return "true";
        case R5900Op::BEQ:
        case R5900Op::BEQL:
            std::snprintf(buf, sizeof(buf), "(ctx.gpr[%u].ud[0] == ctx.gpr[%u].ud[0])", insn.rs, insn.rt);
            return buf;
        case R5900Op::BNE:
        case R5900Op::BNEL:
            std::snprintf(buf, sizeof(buf), "(ctx.gpr[%u].ud[0] != ctx.gpr[%u].ud[0])", insn.rs, insn.rt);
            return buf;
        case R5900Op::BLEZ:
        case R5900Op::BLEZL:
            std::snprintf(buf, sizeof(buf), "(ctx.gpr[%u].sd[0] <= 0)", insn.rs);
            return buf;
        case R5900Op::BGTZ:
        case R5900Op::BGTZL:
            std::snprintf(buf, sizeof(buf), "(ctx.gpr[%u].sd[0] > 0)", insn.rs);
            return buf;
        case R5900Op::BLTZ:
        case R5900Op::BLTZL:
        case R5900Op::BLTZAL:
            std::snprintf(buf, sizeof(buf), "(ctx.gpr[%u].sd[0] < 0)", insn.rs);
            return buf;
        case R5900Op::BGEZ:
        case R5900Op::BGEZL:
        case R5900Op::BGEZAL:
            std::snprintf(buf, sizeof(buf), "(ctx.gpr[%u].sd[0] >= 0)", insn.rs);
            return buf;
        case R5900Op::BC1F:
        case R5900Op::BC1FL:
            return "((ctx.fcr31 & (1u << 23)) == 0)";
        case R5900Op::BC1T:
        case R5900Op::BC1TL:
            return "((ctx.fcr31 & (1u << 23)) != 0)";
        default:
            return "false";
    }
}

std::string CppEmitter::emit_single_insn(const DecodedInsn& d) {
    std::ostringstream ss;
    const u8 rs = d.rs;
    const u8 rt = d.rt;
    const u8 rd = d.rd;
    const u8 sa = d.sa;

    // Protección hardware del registro $zero (r0 nunca se modifica)
    auto write_gpr64_sext32 = [&](u8 reg, const std::string& expr32) {
        if (reg == 0) {
            ss << "        (void)(" << expr32 << ");";
        } else {
            ss << "        ctx.gpr[" << unsigned(reg) << "].sd[0] = static_cast<s64>(static_cast<s32>(" << expr32 << "));";
        }
    };

    auto write_gpr64 = [&](u8 reg, const std::string& expr64) {
        if (reg == 0) {
            ss << "        (void)(" << expr64 << ");";
        } else {
            ss << "        ctx.gpr[" << unsigned(reg) << "].ud[0] = static_cast<u64>(" << expr64 << ");";
        }
    };

    auto write_gpr128_vec = [&](u8 reg, const std::string& vec_expr) {
        if (reg == 0) {
            ss << "        (void)(" << vec_expr << ");";
        } else {
            ss << "        ctx.gpr[" << unsigned(reg) << "].vec_i = " << vec_expr << ";";
        }
    };

    switch (d.op) {
        case R5900Op::NOP:
        case R5900Op::SYNC:
            ss << "        /* nop */ ;";
            break;

        // --- ALU 32/64 bits (con extensión de signo de 32 a 64 bits exacta del R5900) ---
        case R5900Op::ADD:
        case R5900Op::ADDU:
            write_gpr64_sext32(rd, "ctx.gpr[" + std::to_string(rs) + "].uw[0] + ctx.gpr[" + std::to_string(rt) + "].uw[0]");
            break;
        case R5900Op::ADDI:
        case R5900Op::ADDIU:
            write_gpr64_sext32(rt, "ctx.gpr[" + std::to_string(rs) + "].uw[0] + static_cast<u32>(" + std::to_string(static_cast<s32>(d.simm)) + ")");
            break;
        case R5900Op::SUB:
        case R5900Op::SUBU:
            write_gpr64_sext32(rd, "ctx.gpr[" + std::to_string(rs) + "].uw[0] - ctx.gpr[" + std::to_string(rt) + "].uw[0]");
            break;
        case R5900Op::DADDU:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rs) + "].ud[0] + ctx.gpr[" + std::to_string(rt) + "].ud[0]");
            break;
        case R5900Op::DADDIU:
            write_gpr64(rt, "ctx.gpr[" + std::to_string(rs) + "].ud[0] + static_cast<u64>(static_cast<s64>(" + std::to_string(static_cast<s32>(d.simm)) + "))");
            break;
        case R5900Op::DSUBU:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rs) + "].ud[0] - ctx.gpr[" + std::to_string(rt) + "].ud[0]");
            break;

        case R5900Op::AND:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rs) + "].ud[0] & ctx.gpr[" + std::to_string(rt) + "].ud[0]");
            break;
        case R5900Op::OR:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rs) + "].ud[0] | ctx.gpr[" + std::to_string(rt) + "].ud[0]");
            break;
        case R5900Op::XOR:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rs) + "].ud[0] ^ ctx.gpr[" + std::to_string(rt) + "].ud[0]");
            break;
        case R5900Op::NOR:
            write_gpr64(rd, "~(ctx.gpr[" + std::to_string(rs) + "].ud[0] | ctx.gpr[" + std::to_string(rt) + "].ud[0])");
            break;
        case R5900Op::ANDI:
            write_gpr64(rt, "ctx.gpr[" + std::to_string(rs) + "].ud[0] & " + std::to_string(d.uimm) + "ULL");
            break;
        case R5900Op::ORI:
            write_gpr64(rt, "ctx.gpr[" + std::to_string(rs) + "].ud[0] | " + std::to_string(d.uimm) + "ULL");
            break;
        case R5900Op::XORI:
            write_gpr64(rt, "ctx.gpr[" + std::to_string(rs) + "].ud[0] ^ " + std::to_string(d.uimm) + "ULL");
            break;
        case R5900Op::LUI:
            write_gpr64_sext32(rt, "static_cast<u32>(" + std::to_string(d.uimm) + "U) << 16");
            break;

        case R5900Op::SLT:
            write_gpr64(rd, "(ctx.gpr[" + std::to_string(rs) + "].sd[0] < ctx.gpr[" + std::to_string(rt) + "].sd[0]) ? 1ULL : 0ULL");
            break;
        case R5900Op::SLTU:
            write_gpr64(rd, "(ctx.gpr[" + std::to_string(rs) + "].ud[0] < ctx.gpr[" + std::to_string(rt) + "].ud[0]) ? 1ULL : 0ULL");
            break;
        case R5900Op::SLTI:
            write_gpr64(rt, "(ctx.gpr[" + std::to_string(rs) + "].sd[0] < static_cast<s64>(" + std::to_string(static_cast<s32>(d.simm)) + ")) ? 1ULL : 0ULL");
            break;
        case R5900Op::SLTIU:
            write_gpr64(rt, "(ctx.gpr[" + std::to_string(rs) + "].ud[0] < static_cast<u64>(static_cast<s64>(" + std::to_string(static_cast<s32>(d.simm)) + "))) ? 1ULL : 0ULL");
            break;

        // --- Shifts ---
        case R5900Op::SLL:
            write_gpr64_sext32(rd, "ctx.gpr[" + std::to_string(rt) + "].uw[0] << " + std::to_string(sa));
            break;
        case R5900Op::SRL:
            write_gpr64_sext32(rd, "ctx.gpr[" + std::to_string(rt) + "].uw[0] >> " + std::to_string(sa));
            break;
        case R5900Op::SRA:
            write_gpr64_sext32(rd, "ctx.gpr[" + std::to_string(rt) + "].sw[0] >> " + std::to_string(sa));
            break;
        case R5900Op::SLLV:
            write_gpr64_sext32(rd, "ctx.gpr[" + std::to_string(rt) + "].uw[0] << (ctx.gpr[" + std::to_string(rs) + "].uw[0] & 0x1FU)");
            break;
        case R5900Op::SRLV:
            write_gpr64_sext32(rd, "ctx.gpr[" + std::to_string(rt) + "].uw[0] >> (ctx.gpr[" + std::to_string(rs) + "].uw[0] & 0x1FU)");
            break;
        case R5900Op::SRAV:
            write_gpr64_sext32(rd, "ctx.gpr[" + std::to_string(rt) + "].sw[0] >> (ctx.gpr[" + std::to_string(rs) + "].uw[0] & 0x1FU)");
            break;
        case R5900Op::DSLL:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rt) + "].ud[0] << " + std::to_string(sa));
            break;
        case R5900Op::DSRL:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rt) + "].ud[0] >> " + std::to_string(sa));
            break;
        case R5900Op::DSRA:
            write_gpr64(rd, "static_cast<u64>(ctx.gpr[" + std::to_string(rt) + "].sd[0] >> " + std::to_string(sa) + ")");
            break;
        case R5900Op::DSLL32:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rt) + "].ud[0] << " + std::to_string(sa + 32));
            break;
        case R5900Op::DSRL32:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rt) + "].ud[0] >> " + std::to_string(sa + 32));
            break;
        case R5900Op::DSRA32:
            write_gpr64(rd, "static_cast<u64>(ctx.gpr[" + std::to_string(rt) + "].sd[0] >> " + std::to_string(sa + 32) + ")");
            break;
        case R5900Op::DSLLV:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rt) + "].ud[0] << (ctx.gpr[" + std::to_string(rs) + "].uw[0] & 0x3FU)");
            break;
        case R5900Op::DSRLV:
            write_gpr64(rd, "ctx.gpr[" + std::to_string(rt) + "].ud[0] >> (ctx.gpr[" + std::to_string(rs) + "].uw[0] & 0x3FU)");
            break;
        case R5900Op::DSRAV:
            write_gpr64(rd, "static_cast<u64>(ctx.gpr[" + std::to_string(rt) + "].sd[0] >> (ctx.gpr[" + std::to_string(rs) + "].uw[0] & 0x3FU))");
            break;

        case R5900Op::MOVZ:
            if (rd != 0) {
                ss << "        if (ctx.gpr[" << unsigned(rt) << "].ud[0] == 0) ctx.gpr[" << unsigned(rd) << "].ud[0] = ctx.gpr[" << unsigned(rs) << "].ud[0];";
            }
            break;
        case R5900Op::MOVN:
            if (rd != 0) {
                ss << "        if (ctx.gpr[" << unsigned(rt) << "].ud[0] != 0) ctx.gpr[" << unsigned(rd) << "].ud[0] = ctx.gpr[" << unsigned(rs) << "].ud[0];";
            }
            break;

        // --- Multiplicación y División (En R5900 MULT también escribe en rd si rd != 0) ---
        case R5900Op::MULT:
            ss << "        { const s64 prod = static_cast<s64>(ctx.gpr[" << unsigned(rs) << "].sw[0]) * static_cast<s64>(ctx.gpr[" << unsigned(rt) << "].sw[0]); "
               << "ctx.lo.sd[0] = static_cast<s64>(static_cast<s32>(prod)); "
               << "ctx.hi.sd[0] = static_cast<s64>(static_cast<s32>(prod >> 32)); ";
            if (rd != 0) ss << "ctx.gpr[" << unsigned(rd) << "].sd[0] = ctx.lo.sd[0]; ";
            ss << "}";
            break;
        case R5900Op::MULTU:
            ss << "        { const u64 prod = static_cast<u64>(ctx.gpr[" << unsigned(rs) << "].uw[0]) * static_cast<u64>(ctx.gpr[" << unsigned(rt) << "].uw[0]); "
               << "ctx.lo.sd[0] = static_cast<s64>(static_cast<s32>(prod)); "
               << "ctx.hi.sd[0] = static_cast<s64>(static_cast<s32>(prod >> 32)); ";
            if (rd != 0) ss << "ctx.gpr[" << unsigned(rd) << "].sd[0] = ctx.lo.sd[0]; ";
            ss << "}";
            break;
        case R5900Op::DIV:
            ss << "        { const s32 num = ctx.gpr[" << unsigned(rs) << "].sw[0]; const s32 den = ctx.gpr[" << unsigned(rt) << "].sw[0]; "
               << "if (den != 0) { "
               << "if (num == (-2147483647 - 1) && den == -1) { ctx.lo.sd[0] = static_cast<s64>(num); ctx.hi.sd[0] = 0; } "
               << "else { ctx.lo.sd[0] = static_cast<s64>(num / den); ctx.hi.sd[0] = static_cast<s64>(num % den); } "
               << "} else { ctx.lo.sd[0] = (num < 0) ? 1 : -1; ctx.hi.sd[0] = static_cast<s64>(num); } }";
            break;
        case R5900Op::DIVU:
            ss << "        { const u32 num = ctx.gpr[" << unsigned(rs) << "].uw[0]; const u32 den = ctx.gpr[" << unsigned(rt) << "].uw[0]; "
               << "if (den != 0) { "
               << "ctx.lo.sd[0] = static_cast<s64>(static_cast<s32>(num / den)); ctx.hi.sd[0] = static_cast<s64>(static_cast<s32>(num % den)); "
               << "} else { ctx.lo.sd[0] = -1; ctx.hi.sd[0] = static_cast<s64>(static_cast<s32>(num)); } }";
            break;
        case R5900Op::MULT1:
            ss << "        { const s64 prod = static_cast<s64>(ctx.gpr[" << unsigned(rs) << "].sw[0]) * static_cast<s64>(ctx.gpr[" << unsigned(rt) << "].sw[0]); "
               << "ctx.lo.sd[1] = static_cast<s64>(static_cast<s32>(prod)); "
               << "ctx.hi.sd[1] = static_cast<s64>(static_cast<s32>(prod >> 32)); ";
            if (rd != 0) ss << "ctx.gpr[" << unsigned(rd) << "].sd[0] = ctx.lo.sd[1]; ";
            ss << "}";
            break;
        case R5900Op::MULTU1:
            ss << "        { const u64 prod = static_cast<u64>(ctx.gpr[" << unsigned(rs) << "].uw[0]) * static_cast<u64>(ctx.gpr[" << unsigned(rt) << "].uw[0]); "
               << "ctx.lo.sd[1] = static_cast<s64>(static_cast<s32>(prod)); "
               << "ctx.hi.sd[1] = static_cast<s64>(static_cast<s32>(prod >> 32)); ";
            if (rd != 0) ss << "ctx.gpr[" << unsigned(rd) << "].sd[0] = ctx.lo.sd[1]; ";
            ss << "}";
            break;
        case R5900Op::DIV1:
            ss << "        { const s32 num = ctx.gpr[" << unsigned(rs) << "].sw[0]; const s32 den = ctx.gpr[" << unsigned(rt) << "].sw[0]; "
               << "if (den != 0) { "
               << "if (num == (-2147483647 - 1) && den == -1) { ctx.lo.sd[1] = static_cast<s64>(num); ctx.hi.sd[1] = 0; } "
               << "else { ctx.lo.sd[1] = static_cast<s64>(num / den); ctx.hi.sd[1] = static_cast<s64>(num % den); } "
               << "} else { ctx.lo.sd[1] = (num < 0) ? 1 : -1; ctx.hi.sd[1] = static_cast<s64>(num); } }";
            break;
        case R5900Op::DIVU1:
            ss << "        { const u32 num = ctx.gpr[" << unsigned(rs) << "].uw[0]; const u32 den = ctx.gpr[" << unsigned(rt) << "].uw[0]; "
               << "if (den != 0) { "
               << "ctx.lo.sd[1] = static_cast<s64>(static_cast<s32>(num / den)); ctx.hi.sd[1] = static_cast<s64>(static_cast<s32>(num % den)); "
               << "} else { ctx.lo.sd[1] = -1; ctx.hi.sd[1] = static_cast<s64>(static_cast<s32>(num)); } }";
            break;
        case R5900Op::MFHI: write_gpr64(rd, "ctx.hi.ud[0]"); break;
        case R5900Op::MFLO: write_gpr64(rd, "ctx.lo.ud[0]"); break;
        case R5900Op::MTHI: ss << "        ctx.hi.ud[0] = ctx.gpr[" << unsigned(rs) << "].ud[0];"; break;
        case R5900Op::MTLO: ss << "        ctx.lo.ud[0] = ctx.gpr[" << unsigned(rs) << "].ud[0];"; break;
        case R5900Op::MFHI1: write_gpr64(rd, "ctx.hi.ud[1]"); break;
        case R5900Op::MFLO1: write_gpr64(rd, "ctx.lo.ud[1]"); break;
        case R5900Op::MTHI1: ss << "        ctx.hi.ud[1] = ctx.gpr[" << unsigned(rs) << "].ud[0];"; break;
        case R5900Op::MTLO1: ss << "        ctx.lo.ud[1] = ctx.gpr[" << unsigned(rs) << "].ud[0];"; break;

        // --- Accesos directos a la arena (la validez y el tratamiento de MMIO siguen pendientes) ---
        case R5900Op::LB:
            write_gpr64(rt, "static_cast<s64>(MemoryArena::fast_read<s8>(ctx.fastmem_base, ctx.gpr[" + std::to_string(rs) + "].uw[0] + " + std::to_string(static_cast<s32>(d.simm)) + "))");
            break;
        case R5900Op::LBU:
            write_gpr64(rt, "static_cast<u64>(MemoryArena::fast_read<u8>(ctx.fastmem_base, ctx.gpr[" + std::to_string(rs) + "].uw[0] + " + std::to_string(static_cast<s32>(d.simm)) + "))");
            break;
        case R5900Op::LH:
            write_gpr64(rt, "static_cast<s64>(MemoryArena::fast_read<s16>(ctx.fastmem_base, ctx.gpr[" + std::to_string(rs) + "].uw[0] + " + std::to_string(static_cast<s32>(d.simm)) + "))");
            break;
        case R5900Op::LHU:
            write_gpr64(rt, "static_cast<u64>(MemoryArena::fast_read<u16>(ctx.fastmem_base, ctx.gpr[" + std::to_string(rs) + "].uw[0] + " + std::to_string(static_cast<s32>(d.simm)) + "))");
            break;
        case R5900Op::LW:
            write_gpr64_sext32(rt, "MemoryArena::fast_read<u32>(ctx.fastmem_base, ctx.gpr[" + std::to_string(rs) + "].uw[0] + " + std::to_string(static_cast<s32>(d.simm)) + ")");
            break;
        case R5900Op::LWU:
            write_gpr64(rt, "static_cast<u64>(MemoryArena::fast_read<u32>(ctx.fastmem_base, ctx.gpr[" + std::to_string(rs) + "].uw[0] + " + std::to_string(static_cast<s32>(d.simm)) + "))");
            break;
        case R5900Op::LD:
            write_gpr64(rt, "MemoryArena::fast_read<u64>(ctx.fastmem_base, ctx.gpr[" + std::to_string(rs) + "].uw[0] + " + std::to_string(static_cast<s32>(d.simm)) + ")");
            break;
        case R5900Op::LQ:
            if (rt != 0) {
                ss << "        ctx.gpr[" << unsigned(rt) << "] = MemoryArena::fast_read128(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ");";
            }
            break;

        case R5900Op::SB:
            ss << "        MemoryArena::fast_write<u8>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ", ctx.gpr[" << unsigned(rt) << "].ub[0]);";
            break;
        case R5900Op::SH:
            ss << "        MemoryArena::fast_write<u16>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ", ctx.gpr[" << unsigned(rt) << "].uh[0]);";
            break;
        case R5900Op::SW:
            ss << "        MemoryArena::fast_write<u32>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ", ctx.gpr[" << unsigned(rt) << "].uw[0]);";
            break;
        case R5900Op::SD:
            ss << "        MemoryArena::fast_write<u64>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ", ctx.gpr[" << unsigned(rt) << "].ud[0]);";
            break;
        case R5900Op::SQ:
            ss << "        MemoryArena::fast_write128(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ", ctx.gpr[" << unsigned(rt) << "]);";
            break;

        // --- Instrucciones Multimedia de 128 bits (MMI -> Intrínsecos SSE4.1 / AVX2) ---
        case R5900Op::PADDB:
            write_gpr128_vec(rd, "_mm_add_epi8(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PADDH:
            write_gpr128_vec(rd, "_mm_add_epi16(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PADDW:
            write_gpr128_vec(rd, "_mm_add_epi32(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PSUBB:
            write_gpr128_vec(rd, "_mm_sub_epi8(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PSUBH:
            write_gpr128_vec(rd, "_mm_sub_epi16(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PSUBW:
            write_gpr128_vec(rd, "_mm_sub_epi32(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PMAXW:
            write_gpr128_vec(rd, "_mm_max_epi32(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PMINW:
            write_gpr128_vec(rd, "_mm_min_epi32(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PMAXH:
            write_gpr128_vec(rd, "_mm_max_epi16(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PMINH:
            write_gpr128_vec(rd, "_mm_min_epi16(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PCGTW:
            write_gpr128_vec(rd, "_mm_cmpgt_epi32(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PCEQW:
            write_gpr128_vec(rd, "_mm_cmpeq_epi32(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PAND:
            write_gpr128_vec(rd, "_mm_and_si128(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::POR:
            write_gpr128_vec(rd, "_mm_or_si128(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PXOR:
            write_gpr128_vec(rd, "_mm_xor_si128(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PNOR:
            write_gpr128_vec(rd, "_mm_xor_si128(_mm_or_si128(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i), _mm_set1_epi32(-1))");
            break;
        case R5900Op::PEXTLW:
            write_gpr128_vec(rd, "_mm_unpacklo_epi32(ctx.gpr[" + std::to_string(rt) + "].vec_i, ctx.gpr[" + std::to_string(rs) + "].vec_i)");
            break;
        case R5900Op::PEXTUW:
            write_gpr128_vec(rd, "_mm_unpackhi_epi32(ctx.gpr[" + std::to_string(rt) + "].vec_i, ctx.gpr[" + std::to_string(rs) + "].vec_i)");
            break;
        case R5900Op::PCPYLD:
            write_gpr128_vec(rd, "_mm_unpacklo_epi64(ctx.gpr[" + std::to_string(rt) + "].vec_i, ctx.gpr[" + std::to_string(rs) + "].vec_i)");
            break;
        case R5900Op::PCPYUD:
            write_gpr128_vec(rd, "_mm_unpackhi_epi64(ctx.gpr[" + std::to_string(rs) + "].vec_i, ctx.gpr[" + std::to_string(rt) + "].vec_i)");
            break;
        case R5900Op::PCPYH:
            if (rd != 0) {
                ss << "        { const u64 h0 = ctx.gpr[" << unsigned(rt) << "].uh[0]; const u64 h4 = ctx.gpr[" << unsigned(rt) << "].uh[4]; "
                   << "ctx.gpr[" << unsigned(rd) << "].ud[0] = h0 * 0x0001000100010001ULL; "
                   << "ctx.gpr[" << unsigned(rd) << "].ud[1] = h4 * 0x0001000100010001ULL; }";
            }
            break;
        case R5900Op::PMADDW:
            ss << "        { for (int w = 0; w < 4; ++w) { "
               << "const s64 acc = (static_cast<s64>(ctx.hi.sw[w]) << 32) | static_cast<u64>(ctx.lo.uw[w]); "
               << "const s64 res = acc + static_cast<s64>(ctx.gpr[" << unsigned(rs) << "].sw[w]) * static_cast<s64>(ctx.gpr[" << unsigned(rt) << "].sw[w]); "
               << "ctx.lo.sw[w] = static_cast<s32>(res); ctx.hi.sw[w] = static_cast<s32>(res >> 32); ";
            if (rd != 0) ss << "ctx.gpr[" << unsigned(rd) << "].sw[w] = static_cast<s32>(res); ";
            ss << "} }";
            break;

        // --- COP1 (FPU No-IEEE 754 del Emotion Engine) ---
        case R5900Op::LWC1:
            ss << "        ctx.fpr[" << unsigned(rt) << "] = MemoryArena::fast_read<f32>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ");";
            break;
        case R5900Op::SWC1:
            ss << "        MemoryArena::fast_write<f32>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ", ctx.fpr[" << unsigned(rt) << "]);";
            break;
        case R5900Op::MFC1:
            if (rt != 0) {
                ss << "        { s32 bits; std::memcpy(&bits, &ctx.fpr[" << unsigned(d.fs) << "], 4); ctx.gpr[" << unsigned(rt) << "].sd[0] = static_cast<s64>(bits); }";
            }
            break;
        case R5900Op::MTC1:
            ss << "        { u32 bits = ctx.gpr[" << unsigned(rt) << "].uw[0]; std::memcpy(&ctx.fpr[" << unsigned(d.fs) << "], &bits, 4); }";
            break;
        case R5900Op::FPU_ADD_S:
            ss << "        ctx.fpr[" << unsigned(d.fd) << "] = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) + EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "]));";
            break;
        case R5900Op::FPU_SUB_S:
            ss << "        ctx.fpr[" << unsigned(d.fd) << "] = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) - EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "]));";
            break;
        case R5900Op::FPU_MUL_S:
            ss << "        ctx.fpr[" << unsigned(d.fd) << "] = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) * EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "]));";
            break;
        case R5900Op::FPU_DIV_S:
            ss << "        ctx.fpr[" << unsigned(d.fd) << "] = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) / EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "]));";
            break;
        case R5900Op::FPU_ADDA_S:
            ss << "        ctx.fpu_acc = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) + EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "]));";
            break;
        case R5900Op::FPU_MULA_S:
            ss << "        ctx.fpu_acc = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) * EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "]));";
            break;
        case R5900Op::FPU_MADD_S:
            ss << "        ctx.fpr[" << unsigned(d.fd) << "] = EEContext::sanitize_ps2_float(ctx.fpu_acc + EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) * EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "])));";
            break;
        case R5900Op::FPU_MSUB_S:
            ss << "        ctx.fpr[" << unsigned(d.fd) << "] = EEContext::sanitize_ps2_float(ctx.fpu_acc - EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) * EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "])));";
            break;
        case R5900Op::FPU_MAX_S:
            ss << "        { const f32 a = EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]); const f32 b = EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "]); ctx.fpr[" << unsigned(d.fd) << "] = (a >= b) ? a : b; }";
            break;
        case R5900Op::FPU_MIN_S:
            ss << "        { const f32 a = EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]); const f32 b = EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "]); ctx.fpr[" << unsigned(d.fd) << "] = (a <= b) ? a : b; }";
            break;
        case R5900Op::FPU_C_EQ_S:
            ss << "        if (EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) == EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "])) ctx.fcr31 |= (1u << 23); else ctx.fcr31 &= ~(1u << 23);";
            break;
        case R5900Op::FPU_C_LT_S:
            ss << "        if (EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) < EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "])) ctx.fcr31 |= (1u << 23); else ctx.fcr31 &= ~(1u << 23);";
            break;
        case R5900Op::FPU_C_LE_S:
            ss << "        if (EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.fs) << "]) <= EEContext::sanitize_ps2_float(ctx.fpr[" << unsigned(d.ft) << "])) ctx.fcr31 |= (1u << 23); else ctx.fcr31 &= ~(1u << 23);";
            break;

        // --- Syscalls y Excepciones ---
        case R5900Op::SYSCALL:
            ss << "        ctx.pc = 0x" << std::hex << std::uppercase << d.pc << std::dec << "U; EESyscalls::dispatch(ctx); if (ctx.halted) return;";
            break;
        case R5900Op::BREAK:
            ss << "        ctx.halted = true; return;";
            break;

        // --- Memoria no alineada (LWL, LWR, SWL, SWR, LDL, LDR, SDL, SDR) ---
        case R5900Op::LWL:
            if (rt != 0) {
                ss << "        {\n"
                   << "            const u32 va = ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ";\n"
                   << "            const u32 shift = (va & 3) * 8;\n"
                   << "            const u32 mem = MemoryArena::fast_read<u32>(ctx.fastmem_base, va & ~3u);\n"
                   << "            static const u32 mask[4] = { 0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF };\n"
                   << "            ctx.gpr[" << unsigned(rt) << "].uw[0] = (ctx.gpr[" << unsigned(rt) << "].uw[0] & mask[va & 3]) | (mem << (24 - shift));\n"
                   << "            ctx.gpr[" << unsigned(rt) << "].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[" << unsigned(rt) << "].uw[0]));\n"
                   << "        }";
            }
            break;
        case R5900Op::LWR:
            if (rt != 0) {
                ss << "        {\n"
                   << "            const u32 va = ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ";\n"
                   << "            const u32 shift = (va & 3) * 8;\n"
                   << "            const u32 mem = MemoryArena::fast_read<u32>(ctx.fastmem_base, va & ~3u);\n"
                   << "            static const u32 mask[4] = { 0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000 };\n"
                   << "            ctx.gpr[" << unsigned(rt) << "].uw[0] = (ctx.gpr[" << unsigned(rt) << "].uw[0] & mask[va & 3]) | (mem >> shift);\n"
                   << "            ctx.gpr[" << unsigned(rt) << "].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[" << unsigned(rt) << "].uw[0]));\n"
                   << "        }";
            }
            break;
        case R5900Op::SWL:
            ss << "        {\n"
               << "            const u32 va = ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ";\n"
               << "            const u32 shift = (va & 3) * 8;\n"
               << "            u32 mem = MemoryArena::fast_read<u32>(ctx.fastmem_base, va & ~3u);\n"
               << "            static const u32 mask[4] = { 0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000 };\n"
               << "            mem = (mem & mask[va & 3]) | (ctx.gpr[" << unsigned(rt) << "].uw[0] >> (24 - shift));\n"
               << "            MemoryArena::fast_write<u32>(ctx.fastmem_base, va & ~3u, mem);\n"
               << "        }";
            break;
        case R5900Op::SWR:
            ss << "        {\n"
               << "            const u32 va = ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ";\n"
               << "            const u32 shift = (va & 3) * 8;\n"
               << "            u32 mem = MemoryArena::fast_read<u32>(ctx.fastmem_base, va & ~3u);\n"
               << "            static const u32 mask[4] = { 0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF };\n"
               << "            mem = (mem & mask[va & 3]) | (ctx.gpr[" << unsigned(rt) << "].uw[0] << shift);\n"
               << "            MemoryArena::fast_write<u32>(ctx.fastmem_base, va & ~3u, mem);\n"
               << "        }";
            break;
        case R5900Op::LDL:
            if (rt != 0) {
                ss << "        {\n"
                   << "            const u32 va = ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ";\n"
                   << "            const u32 shift = (va & 7) * 8;\n"
                   << "            const u64 mem = MemoryArena::fast_read<u64>(ctx.fastmem_base, va & ~7u);\n"
                   << "            const u64 mask = (shift == 0) ? 0ULL : (~0ULL >> (64 - shift));\n"
                   << "            ctx.gpr[" << unsigned(rt) << "].ud[0] = (ctx.gpr[" << unsigned(rt) << "].ud[0] & mask) | (mem << (56 - shift));\n"
                   << "        }";
            }
            break;
        case R5900Op::LDR:
            if (rt != 0) {
                ss << "        {\n"
                   << "            const u32 va = ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ";\n"
                   << "            const u32 shift = (va & 7) * 8;\n"
                   << "            const u64 mem = MemoryArena::fast_read<u64>(ctx.fastmem_base, va & ~7u);\n"
                   << "            const u64 mask = (shift == 56) ? 0ULL : (~0ULL << (shift + 8));\n"
                   << "            ctx.gpr[" << unsigned(rt) << "].ud[0] = (ctx.gpr[" << unsigned(rt) << "].ud[0] & mask) | (mem >> shift);\n"
                   << "        }";
            }
            break;
        case R5900Op::SDL:
            ss << "        {\n"
               << "            const u32 va = ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ";\n"
               << "            const u32 shift = (va & 7) * 8;\n"
               << "            u64 mem = MemoryArena::fast_read<u64>(ctx.fastmem_base, va & ~7u);\n"
               << "            const u64 mask = (shift == 56) ? 0ULL : (~0ULL << (shift + 8));\n"
               << "            mem = (mem & mask) | (ctx.gpr[" << unsigned(rt) << "].ud[0] >> (56 - shift));\n"
               << "            MemoryArena::fast_write<u64>(ctx.fastmem_base, va & ~7u, mem);\n"
               << "        }";
            break;
        case R5900Op::SDR:
            ss << "        {\n"
               << "            const u32 va = ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ";\n"
               << "            const u32 shift = (va & 7) * 8;\n"
               << "            u64 mem = MemoryArena::fast_read<u64>(ctx.fastmem_base, va & ~7u);\n"
               << "            const u64 mask = (shift == 0) ? 0ULL : (~0ULL >> (64 - shift));\n"
               << "            mem = (mem & mask) | (ctx.gpr[" << unsigned(rt) << "].ud[0] << shift);\n"
               << "            MemoryArena::fast_write<u64>(ctx.fastmem_base, va & ~7u, mem);\n"
               << "        }";
            break;

        // --- Atómicas y Prefetch / Caché ---
        case R5900Op::LL:
            write_gpr64_sext32(rt, "MemoryArena::fast_read<u32>(ctx.fastmem_base, ctx.gpr[" + std::to_string(rs) + "].uw[0] + " + std::to_string(static_cast<s32>(d.simm)) + ")");
            break;
        case R5900Op::SC:
            ss << "        MemoryArena::fast_write<u32>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ", ctx.gpr[" << unsigned(rt) << "].uw[0]);\n";
            write_gpr64(rt, "1");
            break;
        case R5900Op::PREF:
        case R5900Op::CACHE:
            ss << "        /* pref/cache */ ;";
            break;

        // --- Sistema COP0 ---
        case R5900Op::DI:
        case R5900Op::EI:
            ss << "        /* di/ei (cooperative) */ ;";
            break;
        case R5900Op::MFC0:
            write_gpr64(rt, "0");
            break;
        case R5900Op::MTC0:
            ss << "        /* mtc0 */ ;";
            break;

        // --- Movimiento Condicional FP ---
        case R5900Op::MOVF:
            if (rd != 0) {
                ss << "        if ((ctx.fcr31 & (1u << 23)) == 0) ctx.gpr[" << unsigned(rd) << "] = ctx.gpr[" << unsigned(rs) << "];";
            }
            break;
        case R5900Op::MOVT:
            if (rd != 0) {
                ss << "        if ((ctx.fcr31 & (1u << 23)) != 0) ctx.gpr[" << unsigned(rd) << "] = ctx.gpr[" << unsigned(rs) << "];";
            }
            break;

        // --- Traps ---
        case R5900Op::TEQ:
        case R5900Op::TNE:
        case R5900Op::TGE:
        case R5900Op::TGEU:
        case R5900Op::TLT:
        case R5900Op::TLTU:
            ss << "        /* trap */ ;";
            break;

        // --- VU0 / COP2 Macro Mode & Transferencias ---
        case R5900Op::LQC2:
            ss << "        ctx.vf[" << unsigned(d.ft) << "] = MemoryArena::fast_read128(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ");";
            break;
        case R5900Op::SQC2:
            ss << "        MemoryArena::fast_write128(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ", ctx.vf[" << unsigned(d.ft) << "]);";
            break;
        case R5900Op::LWC2:
            ss << "        ctx.vf[" << unsigned(d.ft) << "].uw[0] = MemoryArena::fast_read<u32>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ");";
            break;
        case R5900Op::SWC2:
            ss << "        MemoryArena::fast_write<u32>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ", ctx.vf[" << unsigned(d.ft) << "].uw[0]);";
            break;
        case R5900Op::LDC2:
            ss << "        ctx.vf[" << unsigned(d.ft) << "].ud[0] = MemoryArena::fast_read<u64>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ");";
            break;
        case R5900Op::SDC2:
            ss << "        MemoryArena::fast_write<u64>(ctx.fastmem_base, ctx.gpr[" << unsigned(rs) << "].uw[0] + " << static_cast<s32>(d.simm) << ", ctx.vf[" << unsigned(d.ft) << "].ud[0]);";
            break;
        case R5900Op::CFC2:
            write_gpr64(rt, "ctx.vi[" + std::to_string(d.rd & 0x0F) + "]");
            break;
        case R5900Op::CTC2:
            ss << "        ctx.vi[" << unsigned(d.rd & 0x0F) << "] = static_cast<u16>(ctx.gpr[" << unsigned(rt) << "].uw[0]);";
            break;
        case R5900Op::VU0_MACRO:
            ss << "        /* vu0 macro [0x" << std::hex << d.raw << std::dec << "] */ ;";
            break;

        default:
            ss << "        std::fprintf(stderr, \"[EmotionLayer FATAL] Opcode R5900 no implementado: 0x%08X en PC 0x%08X\\n\", 0x"
               << std::hex << d.raw << "U, 0x" << d.pc << "U); ctx.halted = true; return;";
            break;
    }
    return ss.str();
}

bool CppEmitter::emit_cpp_translation(
    const Ps2ElfImage& img,
    const std::string& output_cpp_path,
    std::string& out_error
) {
    std::ofstream out(output_cpp_path, std::ios::binary);
    if (!out) {
        out_error = "No se pudo crear el archivo C++ de salida: " + output_cpp_path;
        return false;
    }

    // Recopilar todas las instrucciones de todos los segmentos ejecutables
    std::vector<DecodedInsn> insns;
    std::set<u32> valid_pcs;
    for (const auto& seg : img.segments) {
        const bool is_exec = (seg.flags & ElfAnalyzer::kPF_X) != 0 ||
                             (img.entry_point >= seg.vaddr && img.entry_point < seg.vaddr + seg.memsz);
        if (!is_exec || seg.data.size() < 4) continue;

        const std::size_t count = seg.data.size() / 4;
        for (std::size_t i = 0; i < count; ++i) {
            u32 raw = 0;
            std::memcpy(&raw, seg.data.data() + i * 4, sizeof(u32));
            const u32 pc = seg.vaddr + static_cast<u32>(i * 4);
            insns.push_back(R5900Decoder::decode(pc, raw));
            valid_pcs.insert(pc);
        }
    }

    out << "// ============================================================================\n"
        << "// Generado automaticamente por EmotionLayer (traductor experimental R5900)\n"
        << "// Archivo origen: " << img.filepath << "\n"
        << "// Entry Point:    0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << img.entry_point << std::dec << "\n"
        << "// ============================================================================\n\n"
        << "#include \"common/types.hpp\"\n"
        << "#include \"runtime_wine/memory_arena.hpp\"\n"
        << "#include \"runtime_wine/ee_syscalls.hpp\"\n"
        << "#include \"runtime_wine/ee_kernel/kernel_runtime.hpp\"\n"
        << "#include \"runtime_wine/ee_kernel/dmac_vif.hpp\"\n"
        << "#include \"runtime_wine/iop_hle/sif_iop_hle.hpp\"\n"
        << "#include \"runtime_wine/smc_veh_guard.hpp\"\n"
        << "#include \"lifter/vu_compiler.hpp\"\n"
        << "#include \"gsvk/gsvk_renderer.hpp\"\n"
        << "#include \"extractor/iso_reader.hpp\"\n"
        << "#include \"extractor/elf_analyzer.hpp\"\n"
        << "#include \"common/game_profile.hpp\"\n"
        << "#include <cstdio>\n"
        << "#include <cstring>\n"
        << "#include <string>\n"
        << "#include <xmmintrin.h>\n"
        << "#include <pmmintrin.h>\n\n"
        << "using namespace emotion;\n\n";

    const bool is_large_binary = (insns.size() > 2000);

    out << "namespace {\n\n";

    if (!is_large_binary) {
        for (std::size_t s = 0; s < img.segments.size(); ++s) {
            const auto& seg = img.segments[s];
            out << "alignas(16) static const u8 kSegmentData_" << s << "[" << (seg.data.empty() ? 1 : seg.data.size()) << "] = {";
            for (std::size_t b = 0; b < seg.data.size(); ++b) {
                if (b % 16 == 0) out << "\n    ";
                out << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                    << static_cast<unsigned>(seg.data[b]) << std::dec << ", ";
            }
            out << "\n};\n\n";
        }

        out << "static void load_embedded_ps2_segments(u8* fastmem_base, const char* /*iso_path*/) {\n";
        for (std::size_t s = 0; s < img.segments.size(); ++s) {
            const auto& seg = img.segments[s];
            out << "    if (u8* p = MemoryArena::get_guest_ptr<u8>(fastmem_base, 0x" << std::hex << seg.vaddr << "U, 0x" << seg.memsz << "U)) {\n"
                << "        std::memset(p, 0, 0x" << seg.memsz << "U);\n";
            if (!seg.data.empty()) {
                out << "        std::memcpy(p, kSegmentData_" << std::dec << s
                    << ", 0x" << std::hex << seg.data.size() << std::dec << "U);\n";
            }
            out << "    }\n";
        }
        out << "}\n\n";
    } else {
        // Para juegos comerciales grandes (ej. 4 MB de código): escribir payload binario contiguo
        const std::string bin_path = output_cpp_path + ".data.bin";
        std::ofstream bin_file(bin_path, std::ios::binary);
        if (bin_file) {
            for (const auto& seg : img.segments) {
                if (!seg.data.empty()) {
                    bin_file.write(reinterpret_cast<const char*>(seg.data.data()), seg.data.size());
                }
            }
            bin_file.close();
        }

        std::string clean_bin_path = bin_path;
        for (char& c : clean_bin_path) if (c == '\\') c = '/';

        out << "static void load_embedded_ps2_segments(u8* fastmem_base, const char* iso_path) {\n"
            << "    if (iso_path) {\n"
            << "        IsoReader iso;\n"
            << "        std::string err, boot_elf, ver;\n"
            << "        if (iso.open(iso_path, err) && iso.parse_system_cnf(boot_elf, ver, err)) {\n"
            << "            std::vector<u8> elf_bytes;\n"
            << "            if (iso.extract_file(boot_elf, elf_bytes)) {\n"
            << "                GameProfileRegistry::detect_and_set_profile(elf_bytes.data(), elf_bytes.size());\n"
            << "                Ps2ElfImage g_img;\n"
            << "                if (ElfAnalyzer::parse_bytes(elf_bytes, boot_elf, g_img, err)) {\n"
            << "                    for (const auto& s : g_img.segments) {\n"
            << "                        if (u8* p = MemoryArena::get_guest_ptr<u8>(fastmem_base, s.vaddr, s.memsz)) {\n"
            << "                            std::memset(p, 0, s.memsz);\n"
            << "                            if (!s.data.empty()) std::memcpy(p, s.data.data(), s.data.size());\n"
            << "                        }\n"
            << "                    }\n"
            << "                    MemoryArena::fast_write<u32>(fastmem_base, 0x1000U + 360U, 0x00063082U);\n"
            << "                    MemoryArena::fast_write<u32>(fastmem_base, 0x1000U + 524U, 0x8C820000U);\n"
            << "                    return;\n"
            << "                }\n"
            << "            }\n"
            << "        }\n"
            << "    }\n"
            << "    FILE* f = std::fopen(\"" << clean_bin_path << "\", \"rb\");\n"
            << "    if (f) {\n";
        for (std::size_t s = 0; s < img.segments.size(); ++s) {
            const auto& seg = img.segments[s];
            out << "        if (u8* p = MemoryArena::get_guest_ptr<u8>(fastmem_base, 0x" << std::hex << seg.vaddr << "U, 0x" << seg.memsz << "U)) {\n"
                << "            std::memset(p, 0, 0x" << seg.memsz << "U);\n";
            if (!seg.data.empty()) {
                out << "            std::fread(p, 1, 0x" << seg.data.size() << "U, f);\n";
            }
            out << "        }\n";
        }
        out << "        std::fclose(f);\n"
            << "    }\n"
            << "    MemoryArena::fast_write<u32>(fastmem_base, 0x1000U + 360U, 0x00063082U);\n"
            << "    MemoryArena::fast_write<u32>(fastmem_base, 0x1000U + 524U, 0x8C820000U);\n"
            << "}\n\n";
    }

    if (is_large_binary) {
        // En juegos comerciales con cientos de miles de instrucciones, ejecutar a través del DynamicRecompCache
        // para evitar emitir un dispatcher estático enorme para imágenes grandes
        out << "static void execute_recompiled_ee(EEContext& ctx) {\n"
            << "    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);\n"
            << "    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);\n\n"
            << "    u64 count = 0;\n"
            << "    while (!ctx.halted) {\n"
            << "        if (ctx.pc == EEKernelRuntime::kThreadExitReturnSentinel) {\n"
            << "            if (auto* k = EEKernelRuntime::active_instance()) k->sys_exit_thread();\n"
            << "            return;\n"
            << "        }\n"
            << "        if (!DynamicRecompCache::execute_or_compile(ctx)) {\n"
            << "            std::fprintf(stderr, \"[EmotionLayer] Salto a direccion no recompilada: 0x%08X\\n\", ctx.pc);\n"
            << "            ctx.halted = true;\n"
            << "            return;\n"
            << "        }\n"
            << "        count++;\n"
            << "        if (count % 20000 == 0) {\n"
            << "            std::printf(\"[EmotionLayer EE] PC: 0x%08X | Bloques JIT: %u | Instrucciones ejecutadas: %llu\\n\",\n"
            << "                ctx.pc, DynamicRecompCache::compiled_blocks_count(), count);\n"
            << "            std::fflush(stdout);\n"
            << "        }\n"
            << "    }\n"
            << "}\n\n";
    } else {
        out << "static void execute_recompiled_ee(EEContext& ctx) {\n"
            << "    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);\n"
            << "    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);\n\n"
            << "dispatch_pc:\n"
            << "    if (ctx.halted) return;\n"
            << "    if (ctx.pc == EEKernelRuntime::kThreadExitReturnSentinel) {\n"
            << "        if (auto* k = EEKernelRuntime::active_instance()) k->sys_exit_thread();\n"
            << "        return;\n"
            << "    }\n"
            << "    switch (ctx.pc) {\n";
        for (u32 pc : valid_pcs) {
            out << "        case 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << pc
                << ": goto L_" << std::setw(8) << std::setfill('0') << pc << ";\n";
        }
        out << std::dec
            << "        default:\n"
            << "            if (DynamicRecompCache::execute_or_compile(ctx)) goto dispatch_pc;\n"
            << "            std::fprintf(stderr, \"[EmotionLayer] Salto a direccion no recompilada: 0x%08X\\n\", ctx.pc);\n"
            << "            ctx.halted = true;\n"
            << "            return;\n"
            << "    }\n\n";

        for (std::size_t i = 0; i < insns.size(); ++i) {
            const auto& d = insns[i];
            out << "L_" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << d.pc << std::dec
                << ": // " << d.disassemble() << "\n";

            if (d.is_branch) {
                // El delay slot debe estar estrictamente en d.pc + 4 dentro del mismo segmento de carga
                DecodedInsn delay_insn{};
                if (i + 1 < insns.size() && insns[i + 1].pc == d.pc + 4) {
                    delay_insn = insns[i + 1];
                } else {
                    delay_insn.pc = d.pc + 4;
                    delay_insn.op = R5900Op::NOP;
                }
                const std::string cond_expr  = emit_branch_condition(d);

                out << "    {\n"
                    << "        const bool branch_taken = " << cond_expr << ";\n";

                if (d.op == R5900Op::JR || d.op == R5900Op::JALR) {
                    out << "        const u32 target_pc = ctx.gpr[" << unsigned(d.rs) << "].uw[0];\n";
                }
                if (d.is_call) {
                    const u8 link_reg = (d.op == R5900Op::JALR) ? d.rd : 31;
                    if (link_reg != 0) {
                        out << "        if (branch_taken) ctx.gpr[" << unsigned(link_reg) << "].sd[0] = static_cast<s64>(static_cast<s32>(0x"
                            << std::hex << (d.pc + 8) << std::dec << "U));\n";
                    }
                }

                if (d.is_likely) {
                    out << "        if (branch_taken) {\n"
                        << "    " << emit_single_insn(delay_insn) << "\n";
                    if (valid_pcs.count(d.jump_target)) {
                        out << "            goto L_" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << d.jump_target << std::dec << ";\n";
                    } else {
                        out << "            ctx.pc = 0x" << std::hex << d.jump_target << std::dec << "U; goto dispatch_pc;\n";
                    }
                    out << "        } else {\n";
                    if (valid_pcs.count(d.pc + 8)) {
                        out << "            goto L_" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << (d.pc + 8) << std::dec << ";\n";
                    } else {
                        out << "            ctx.pc = 0x" << std::hex << (d.pc + 8) << std::dec << "U; goto dispatch_pc;\n";
                    }
                    out << "        }\n";
                } else {
                    out << emit_single_insn(delay_insn) << "\n"
                        << "        if (branch_taken) {\n";
                    if (d.op == R5900Op::JR || d.op == R5900Op::JALR) {
                        out << "            ctx.pc = target_pc; goto dispatch_pc;\n";
                    } else if (valid_pcs.count(d.jump_target)) {
                        out << "            goto L_" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << d.jump_target << std::dec << ";\n";
                    } else {
                        out << "            ctx.pc = 0x" << std::hex << d.jump_target << std::dec << "U; goto dispatch_pc;\n";
                    }
                    out << "        }\n";
                    if (valid_pcs.count(d.pc + 8)) {
                        out << "        goto L_" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << (d.pc + 8) << std::dec << ";\n";
                    } else {
                        out << "        ctx.pc = 0x" << std::hex << (d.pc + 8) << std::dec << "U; goto dispatch_pc;\n";
                    }
                }
                out << "    }\n";
            } else {
                out << emit_single_insn(d) << "\n";
            }
        }
        out << "}\n\n";
    }

    out << "} // namespace\n\n"
        << "int main(int argc, char** argv) {\n"
        << "    MemoryArena arena;\n"
        << "    if (!arena.initialize()) {\n"
        << "        std::fprintf(stderr, \"[EmotionLayer] Error inicializando la Arena de 4 GB en Windows.\\n\");\n"
        << "        return -1;\n"
        << "    }\n\n"
        << "    const char* iso_path_arg = nullptr;\n"
        << "    for (int i = 1; i < argc; ++i) {\n"
        << "        const std::string arg = argv[i];\n"
        << "        if (arg == \"--iso\" && i + 1 < argc) {\n"
        << "            iso_path_arg = argv[i + 1];\n"
        << "        }\n"
        << "    }\n"
        << "    load_embedded_ps2_segments(arena.base(), iso_path_arg);\n\n"
        << "    GsvkRenderer gsvk;\n"
        << "    gsvk.initialize();\n"
        << "    GsvkRenderer::set_active_instance(&gsvk);\n\n"
        << "    Vu1Engine vu1;\n"
        << "    DmacVifController dmac;\n"
        << "    dmac.bind(arena.base(), &vu1, &gsvk);\n"
        << "    DmacVifController::set_active_instance(&dmac);\n\n"
        << "    SifIopHle iop_hle;\n"
        << "    iop_hle.set_fastmem_base(arena.base());\n"
        << "    SifIopHle::set_active_instance(&iop_hle);\n"
        << "    iop_hle.spu2().start_live_audio();\n"
        << "    for (int i = 1; i < argc; ++i) {\n"
        << "        const std::string arg = argv[i];\n"
        << "        if (arg == \"--window\") {\n"
        << "            gsvk.enable_realtime_window(true);\n"
        << "        } else if (arg == \"--iso\" && i + 1 < argc) {\n"
        << "            std::string err;\n"
        << "            iop_hle.mount_iso(argv[++i], err);\n"
        << "        } else if (arg == \"--scale\" && i + 1 < argc) {\n"
        << "            gsvk.set_resolution_scale(std::atoi(argv[++i]));\n"
        << "        } else if (arg == \"--fps\" && i + 1 < argc) {\n"
        << "            gsvk.set_target_framerate(std::atoi(argv[++i]));\n"
        << "        }\n"
        << "    }\n\n"
        << "    EEKernelRuntime kernel;\n"
        << "    if (!kernel.initialize_main_fiber(arena.base(), 0x" << std::hex << std::uppercase << img.entry_point << std::dec << "U, &execute_recompiled_ee)) {\n"
        << "        std::fprintf(stderr, \"[EmotionLayer] Error inicializando el planificador Win32 Fibers.\\n\");\n"
        << "        return -1;\n"
        << "    }\n"
        << "    kernel.enable_hw_interrupts();\n"
        << "    return kernel.run_main_loop();\n"
        << "}\n";

    return true;
}

bool CppEmitter::build_native_windows_exe(
    const std::string& generated_cpp_path,
    const std::string& project_root_dir,
    const std::string& output_exe_path,
    std::string& out_log
) {
    // Invocamos vcvars64.bat de Visual Studio 2022 y cl.exe con optimizaciones /O2 /arch:AVX2 /std:c++20
    const std::string vcvars = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat";
    const std::string cmd = "cmd.exe /C \"\"" + vcvars + "\" >nul && cl.exe /nologo /std:c++20 /O2 /arch:AVX2 /EHsc "
        "/I\"" + project_root_dir + "\\src\" "
        "\"" + generated_cpp_path + "\" "
        "\"" + project_root_dir + "\\src\\common\\game_profile.cpp\" "
        "\"" + project_root_dir + "\\src\\extractor\\elf_analyzer.cpp\" "
        "\"" + project_root_dir + "\\src\\extractor\\iso_reader.cpp\" "
        "\"" + project_root_dir + "\\src\\lifter\\r5900_decoder.cpp\" "
        "\"" + project_root_dir + "\\src\\lifter\\vu_compiler.cpp\" "
        "\"" + project_root_dir + "\\src\\lifter\\iop_r3000_lifter.cpp\" "
        "\"" + project_root_dir + "\\src\\gsvk\\gsvk_renderer.cpp\" "
        "\"" + project_root_dir + "\\src\\gsvk\\gsvk_enhancements.cpp\" "
        "\"" + project_root_dir + "\\src\\gsvk\\gsvk_window.cpp\" "
        "\"" + project_root_dir + "\\src\\runtime_wine\\memory_arena.cpp\" "
        "\"" + project_root_dir + "\\src\\runtime_wine\\smc_veh_guard.cpp\" "
        "\"" + project_root_dir + "\\src\\runtime_wine\\game_profile_hooks.cpp\" "
        "\"" + project_root_dir + "\\src\\runtime_wine\\ee_syscalls.cpp\" "
        "\"" + project_root_dir + "\\src\\runtime_wine\\ee_kernel\\kernel_runtime.cpp\" "
        "\"" + project_root_dir + "\\src\\runtime_wine\\ee_kernel\\dmac_vif.cpp\" "
        "\"" + project_root_dir + "\\src\\runtime_wine\\iop_hle\\sif_iop_hle.cpp\" "
        "\"" + project_root_dir + "\\src\\runtime_wine\\iop_hle\\win_spu2_audio.cpp\" "
        "\"" + project_root_dir + "\\src\\runtime_wine\\iop_hle\\wasapi_streamer.cpp\" "
        "/Fe:\"" + output_exe_path + "\"\"";

    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        out_log = "Fallo la compilacion nativa con cl.exe (codigo " + std::to_string(rc) + ").";
        return false;
    }
    out_log = "Ejecutable nativo x86_64 compilado con exito: " + output_exe_path;
    return true;
}

} // namespace emotion
