#include "r5900_decoder.hpp"
#include <cstdio>

namespace emotion {

const char* R5900Decoder::gpr_name(u8 idx) noexcept {
    static constexpr const char* kNames[32] = {
        "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
        "$t0",   "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
        "$s0",   "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
        "$t8",   "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra"
    };
    return kNames[idx & 0x1F];
}

DecodedInsn R5900Decoder::decode(u32 pc, u32 raw) noexcept {
    DecodedInsn d{};
    d.pc   = pc;
    d.raw  = raw;
    if (raw == 0) {
        d.op = R5900Op::NOP;
        return d;
    }

    const u8 opcode = static_cast<u8>((raw >> 26) & 0x3F);
    d.rs   = static_cast<u8>((raw >> 21) & 0x1F);
    d.rt   = static_cast<u8>((raw >> 16) & 0x1F);
    d.rd   = static_cast<u8>((raw >> 11) & 0x1F);
    d.sa   = static_cast<u8>((raw >> 6)  & 0x1F);
    d.fs   = d.rd;
    d.ft   = d.rt;
    d.fd   = d.sa;
    d.uimm = static_cast<u16>(raw & 0xFFFFU);
    d.simm = static_cast<s16>(d.uimm);
    const u8 funct = static_cast<u8>(raw & 0x3F);

    const auto branch_dest = [&]() -> u32 {
        return pc + 4 + (static_cast<s32>(d.simm) << 2);
    };

    switch (opcode) {
        case 0x00: { // SPECIAL
            switch (funct) {
                case 0x00: d.op = R5900Op::SLL;    break;
                case 0x01: { // MOVF / MOVT
                    if ((raw & (1 << 16)) == 0) d.op = R5900Op::MOVF;
                    else                        d.op = R5900Op::MOVT;
                    break;
                }
                case 0x02: d.op = R5900Op::SRL;    break;
                case 0x03: d.op = R5900Op::SRA;    break;
                case 0x04: d.op = R5900Op::SLLV;   break;
                case 0x06: d.op = R5900Op::SRLV;   break;
                case 0x07: d.op = R5900Op::SRAV;   break;
                case 0x08: d.op = R5900Op::JR;     d.is_branch = true; break;
                case 0x09: d.op = R5900Op::JALR;   d.is_branch = true; d.is_call = true; break;
                case 0x0A: d.op = R5900Op::MOVZ;   break;
                case 0x0B: d.op = R5900Op::MOVN;   break;
                case 0x0C: d.op = R5900Op::SYSCALL; break;
                case 0x0D: d.op = R5900Op::BREAK;  break;
                case 0x0F: d.op = R5900Op::SYNC;   break;
                case 0x10: d.op = R5900Op::MFHI;   break;
                case 0x11: d.op = R5900Op::MTHI;   break;
                case 0x12: d.op = R5900Op::MFLO;   break;
                case 0x13: d.op = R5900Op::MTLO;   break;
                case 0x14: d.op = R5900Op::DSLLV;  break;
                case 0x16: d.op = R5900Op::DSRLV;  break;
                case 0x17: d.op = R5900Op::DSRAV;  break;
                case 0x18: d.op = R5900Op::MULT;   break;
                case 0x19: d.op = R5900Op::MULTU;  break;
                case 0x1A: d.op = R5900Op::DIV;    break;
                case 0x1B: d.op = R5900Op::DIVU;   break;
                case 0x2C: d.op = R5900Op::DADDU;  break;
                case 0x2E: d.op = R5900Op::DSUBU;  break;
                case 0x20: d.op = R5900Op::ADD;    break;
                case 0x21: d.op = R5900Op::ADDU;   break;
                case 0x22: d.op = R5900Op::SUB;    break;
                case 0x23: d.op = R5900Op::SUBU;   break;
                case 0x24: d.op = R5900Op::AND;    break;
                case 0x25: d.op = R5900Op::OR;     break;
                case 0x26: d.op = R5900Op::XOR;    break;
                case 0x27: d.op = R5900Op::NOR;    break;
                case 0x28: d.op = R5900Op::mfsa;   break;
                case 0x29: d.op = R5900Op::mtsa;   break;
                case 0x2A: d.op = R5900Op::SLT;    break;
                case 0x2B: d.op = R5900Op::SLTU;   break;
                case 0x2D: d.op = R5900Op::DADDU;  break;
                case 0x2F: d.op = R5900Op::DSUBU;  break;
                case 0x30: d.op = R5900Op::TGE;    break;
                case 0x31: d.op = R5900Op::TGEU;   break;
                case 0x32: d.op = R5900Op::TLT;    break;
                case 0x33: d.op = R5900Op::TLTU;   break;
                case 0x34: d.op = R5900Op::TEQ;    break;
                case 0x36: d.op = R5900Op::TNE;    break;
                case 0x38: d.op = R5900Op::DSLL;   break;
                case 0x3A: d.op = R5900Op::DSRL;   break;
                case 0x3B: d.op = R5900Op::DSRA;   break;
                case 0x3C: d.op = R5900Op::DSLL32; break;
                case 0x3E: d.op = R5900Op::DSRL32; break;
                case 0x3F: d.op = R5900Op::DSRA32; break;
                default: break;
            }
            break;
        }
        case 0x01: { // REGIMM
            d.is_branch   = true;
            d.jump_target = branch_dest();
            switch (d.rt) {
                case 0x00: d.op = R5900Op::BLTZ;   break;
                case 0x01: d.op = R5900Op::BGEZ;   break;
                case 0x02: d.op = R5900Op::BLTZL;  d.is_likely = true; break;
                case 0x03: d.op = R5900Op::BGEZL;  d.is_likely = true; break;
                case 0x10: d.op = R5900Op::BLTZAL; d.is_call = true;   break;
                case 0x11: d.op = R5900Op::BGEZAL; d.is_call = true;   break;
                default:   d.is_branch = false; break;
            }
            break;
        }
        case 0x02: // J
            d.op          = R5900Op::J;
            d.is_branch   = true;
            d.jump_target = ((pc + 4) & 0xF0000000U) | ((raw & 0x03FFFFFFU) << 2);
            break;
        case 0x03: // JAL
            d.op          = R5900Op::JAL;
            d.is_branch   = true;
            d.is_call     = true;
            d.jump_target = ((pc + 4) & 0xF0000000U) | ((raw & 0x03FFFFFFU) << 2);
            break;
        case 0x04: d.op = R5900Op::BEQ;   d.is_branch = true; d.jump_target = branch_dest(); break;
        case 0x05: d.op = R5900Op::BNE;   d.is_branch = true; d.jump_target = branch_dest(); break;
        case 0x06: d.op = R5900Op::BLEZ;  d.is_branch = true; d.jump_target = branch_dest(); break;
        case 0x07: d.op = R5900Op::BGTZ;  d.is_branch = true; d.jump_target = branch_dest(); break;
        case 0x08: d.op = R5900Op::ADDI;  break;
        case 0x09: d.op = R5900Op::ADDIU; break;
        case 0x0A: d.op = R5900Op::SLTI;  break;
        case 0x0B: d.op = R5900Op::SLTIU; break;
        case 0x0C: d.op = R5900Op::ANDI;  break;
        case 0x0D: d.op = R5900Op::ORI;   break;
        case 0x0E: d.op = R5900Op::XORI;  break;
        case 0x0F: d.op = R5900Op::LUI;   break;

        case 0x10: { // COP0 (System Control del Emotion Engine)
            // COP0 CO (bit 25 = 1): el R5900 codifica EI = funct 0x38 y DI = funct 0x39
            // (0x42000038 / 0x42000039). Las formas MIPS32 (0x41606000 / 0x41606020) se aceptan también.
            if ((raw & 0x02000000U) && (raw & 0x3FU) == 0x39U) {
                d.op = R5900Op::DI;
            } else if ((raw & 0x02000000U) && (raw & 0x3FU) == 0x38U) {
                d.op = R5900Op::EI;
            } else if (raw == 0x41606000 || (d.rs == 0x0B && (raw & 0x20) == 0)) {
                d.op = R5900Op::DI;
            } else if (raw == 0x41606020 || (d.rs == 0x0B && (raw & 0x20) != 0)) {
                d.op = R5900Op::EI;
            } else if (d.rs == 0x00) {
                d.op = R5900Op::MFC0;
            } else if (d.rs == 0x04) {
                d.op = R5900Op::MTC0;
            }
            break;
        }

        case 0x11: { // COP1 (FPU del Emotion Engine)
            switch (d.rs) {
                case 0x00: d.op = R5900Op::MFC1; break;
                case 0x02: d.op = R5900Op::CFC1; break;
                case 0x04: d.op = R5900Op::MTC1; break;
                case 0x06: d.op = R5900Op::CTC1; break;
                case 0x08: { // BC1
                    d.is_branch   = true;
                    d.jump_target = branch_dest();
                    switch (d.rt) {
                        case 0x00: d.op = R5900Op::BC1F;  break;
                        case 0x01: d.op = R5900Op::BC1T;  break;
                        case 0x02: d.op = R5900Op::BC1FL; d.is_likely = true; break;
                        case 0x03: d.op = R5900Op::BC1TL; d.is_likely = true; break;
                        default:   d.is_branch = false; break;
                    }
                    break;
                }
                case 0x10: { // FPU.S
                    switch (funct) {
                        case 0x00: d.op = R5900Op::FPU_ADD_S;  break;
                        case 0x01: d.op = R5900Op::FPU_SUB_S;  break;
                        case 0x02: d.op = R5900Op::FPU_MUL_S;  break;
                        case 0x03: d.op = R5900Op::FPU_DIV_S;  break;
                        case 0x04: d.op = R5900Op::FPU_SQRT_S; break;
                        case 0x05: d.op = R5900Op::FPU_ABS_S;  break;
                        case 0x06: d.op = R5900Op::FPU_MOV_S;  break;
                        case 0x07: d.op = R5900Op::FPU_NEG_S;  break;
                        case 0x18: d.op = R5900Op::FPU_ADDA_S; break;
                        case 0x19: d.op = R5900Op::FPU_SUBA_S; break;
                        case 0x1A: d.op = R5900Op::FPU_MULA_S; break;
                        case 0x1C: d.op = R5900Op::FPU_MADD_S; break;
                        case 0x1D: d.op = R5900Op::FPU_MSUB_S; break;
                        case 0x24: d.op = R5900Op::FPU_CVT_W_S; break;
                        case 0x28: d.op = R5900Op::FPU_MAX_S;  break;
                        case 0x29: d.op = R5900Op::FPU_MIN_S;  break;
                        case 0x32: d.op = R5900Op::FPU_C_EQ_S; break;
                        case 0x34: d.op = R5900Op::FPU_C_LT_S; break;
                        case 0x36: d.op = R5900Op::FPU_C_LE_S; break;
                        default: break;
                    }
                    break;
                }
                case 0x14: { // FPU.W
                    if (funct == 0x20) d.op = R5900Op::FPU_CVT_S_W;
                    break;
                }
                default: break;
            }
            break;
        }

        case 0x12: { // COP2 (VU0 Macro Mode & transferencias de registros de control)
            if (raw & (1 << 25)) {
                d.op = R5900Op::VU0_MACRO;
            } else if (d.rs == 0x02) {
                d.op = R5900Op::CFC2;
            } else if (d.rs == 0x06) {
                d.op = R5900Op::CTC2;
            }
            break;
        }

        case 0x14: d.op = R5900Op::BEQL;  d.is_branch = true; d.is_likely = true; d.jump_target = branch_dest(); break;
        case 0x15: d.op = R5900Op::BNEL;  d.is_branch = true; d.is_likely = true; d.jump_target = branch_dest(); break;
        case 0x16: d.op = R5900Op::BLEZL; d.is_branch = true; d.is_likely = true; d.jump_target = branch_dest(); break;
        case 0x17: d.op = R5900Op::BGTZL; d.is_branch = true; d.is_likely = true; d.jump_target = branch_dest(); break;
        case 0x19: d.op = R5900Op::DADDIU; break;
        case 0x1A: d.op = R5900Op::LDL; break;
        case 0x1B: d.op = R5900Op::LDR; break;

        case 0x1C: { // MMI (Instrucciones Multimedia de 128 bits exclusivas del R5900)
            switch (funct) {
                case 0x00: d.op = R5900Op::MADD;   break;
                case 0x01: d.op = R5900Op::MADDU;  break;
                case 0x08: { // MMI0
                    switch (d.sa) {
                        case 0x00: d.op = R5900Op::PADDW;  break;
                        case 0x01: d.op = R5900Op::PSUBW;  break;
                        case 0x02: d.op = R5900Op::PCGTW;  break;
                        case 0x03: d.op = R5900Op::PMAXW;  break;
                        case 0x04: d.op = R5900Op::PADDH;  break;
                        case 0x05: d.op = R5900Op::PSUBH;  break;
                        case 0x07: d.op = R5900Op::PMAXH;  break;
                        case 0x08: d.op = R5900Op::PADDB;  break;
                        case 0x09: d.op = R5900Op::PSUBB;  break;
                        case 0x10: d.op = R5900Op::PADDSW; break;
                        case 0x11: d.op = R5900Op::PSUBSW; break;
                        case 0x12: d.op = R5900Op::PEXTLW; break;
                        default: break;
                    }
                    break;
                }
                case 0x09: { // MMI2
                    switch (d.sa) {
                        case 0x00: d.op = R5900Op::PMADDW; break;
                        case 0x0E: d.op = R5900Op::PCPYLD; break;
                        case 0x12: d.op = R5900Op::PAND;   break;
                        case 0x13: d.op = R5900Op::PXOR;   break;
                        default: break;
                    }
                    break;
                }
                case 0x10: d.op = R5900Op::MFHI1;  break;
                case 0x11: d.op = R5900Op::MTHI1;  break;
                case 0x12: d.op = R5900Op::MFLO1;  break;
                case 0x13: d.op = R5900Op::MTLO1;  break;
                case 0x18: d.op = R5900Op::MULT1;  break;
                case 0x19: d.op = R5900Op::MULTU1; break;
                case 0x1A: d.op = R5900Op::DIV1;   break;
                case 0x1B: d.op = R5900Op::DIVU1;  break;
                case 0x28: { // MMI1
                    switch (d.sa) {
                        case 0x02: d.op = R5900Op::PCEQW;  break;
                        case 0x03: d.op = R5900Op::PMINW;  break;
                        case 0x07: d.op = R5900Op::PMINH;  break;
                        case 0x10: d.op = R5900Op::PADDUW; break;
                        case 0x11: d.op = R5900Op::PSUBUW; break;
                        case 0x12: d.op = R5900Op::PEXTUW; break;
                        default: break;
                    }
                    break;
                }
                case 0x29: { // MMI3
                    switch (d.sa) {
                        case 0x0E: d.op = R5900Op::PCPYUD; break;
                        case 0x12: d.op = R5900Op::POR;    break;
                        case 0x13: d.op = R5900Op::PNOR;   break;
                        case 0x1B: d.op = R5900Op::PCPYH;  break;
                        default: break;
                    }
                    break;
                }
                default: break;
            }
            break;
        }

        case 0x1E: d.op = R5900Op::LQ;    break; // Load Quadword (128-bit)
        case 0x1F: d.op = R5900Op::SQ;    break; // Store Quadword (128-bit)
        case 0x20: d.op = R5900Op::LB;    break;
        case 0x21: d.op = R5900Op::LH;    break;
        case 0x22: d.op = R5900Op::LWL;   break;
        case 0x23: d.op = R5900Op::LW;    break;
        case 0x24: d.op = R5900Op::LBU;   break;
        case 0x25: d.op = R5900Op::LHU;   break;
        case 0x26: d.op = R5900Op::LWR;   break;
        case 0x27: d.op = R5900Op::LWU;   break;
        case 0x28: d.op = R5900Op::SB;    break;
        case 0x29: d.op = R5900Op::SH;    break;
        case 0x2A: d.op = R5900Op::SWL;   break;
        case 0x2B: d.op = R5900Op::SW;    break;
        case 0x2C: d.op = R5900Op::SDL;   break;
        case 0x2D: d.op = R5900Op::SDR;   break;
        case 0x2E: d.op = R5900Op::SWR;   break;
        case 0x2F: d.op = R5900Op::CACHE; break;
        case 0x30: d.op = R5900Op::LL;    break;
        case 0x31: d.op = R5900Op::LWC1;  break;
        case 0x32: d.op = R5900Op::LWC2;  break;
        case 0x33: d.op = R5900Op::PREF;  break;
        case 0x34: d.op = R5900Op::LQC2;  break;
        case 0x36: d.op = R5900Op::LDC2;  break;
        case 0x37: d.op = R5900Op::LD;    break;
        case 0x38: d.op = R5900Op::SC;    break;
        case 0x39: d.op = R5900Op::SWC1;  break;
        case 0x3A: d.op = R5900Op::SWC2;  break;
        case 0x3C: d.op = R5900Op::SQC2;  break;
        case 0x3E: d.op = R5900Op::SDC2;  break;
        case 0x3F: d.op = R5900Op::SD;    break;
        default: break;
    }

    return d;
}

std::string DecodedInsn::disassemble() const {
    char buf[128];
    const char* rs_n = R5900Decoder::gpr_name(rs);
    const char* rt_n = R5900Decoder::gpr_name(rt);
    const char* rd_n = R5900Decoder::gpr_name(rd);

    switch (op) {
        case R5900Op::NOP:     std::snprintf(buf, sizeof(buf), "nop"); break;
        case R5900Op::ADDU:    std::snprintf(buf, sizeof(buf), "addu %s, %s, %s", rd_n, rs_n, rt_n); break;
        case R5900Op::ADDIU:   std::snprintf(buf, sizeof(buf), "addiu %s, %s, %d", rt_n, rs_n, simm); break;
        case R5900Op::SUBU:    std::snprintf(buf, sizeof(buf), "subu %s, %s, %s", rd_n, rs_n, rt_n); break;
        case R5900Op::DADDU:   std::snprintf(buf, sizeof(buf), "daddu %s, %s, %s", rd_n, rs_n, rt_n); break;
        case R5900Op::DADDIU:  std::snprintf(buf, sizeof(buf), "daddiu %s, %s, %d", rt_n, rs_n, simm); break;
        case R5900Op::DSLLV:   std::snprintf(buf, sizeof(buf), "dsllv %s, %s, %s", rd_n, rt_n, rs_n); break;
        case R5900Op::DSRLV:   std::snprintf(buf, sizeof(buf), "dsrlv %s, %s, %s", rd_n, rt_n, rs_n); break;
        case R5900Op::DSRAV:   std::snprintf(buf, sizeof(buf), "dsrav %s, %s, %s", rd_n, rt_n, rs_n); break;
        case R5900Op::ANDI:    std::snprintf(buf, sizeof(buf), "andi %s, %s, 0x%04X", rt_n, rs_n, uimm); break;
        case R5900Op::ORI:     std::snprintf(buf, sizeof(buf), "ori %s, %s, 0x%04X", rt_n, rs_n, uimm); break;
        case R5900Op::LUI:     std::snprintf(buf, sizeof(buf), "lui %s, 0x%04X", rt_n, uimm); break;
        case R5900Op::LW:      std::snprintf(buf, sizeof(buf), "lw %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::SW:      std::snprintf(buf, sizeof(buf), "sw %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::LD:      std::snprintf(buf, sizeof(buf), "ld %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::SD:      std::snprintf(buf, sizeof(buf), "sd %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::LQ:      std::snprintf(buf, sizeof(buf), "lq %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::SQ:      std::snprintf(buf, sizeof(buf), "sq %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::JAL:     std::snprintf(buf, sizeof(buf), "jal 0x%08X", jump_target); break;
        case R5900Op::JR:      std::snprintf(buf, sizeof(buf), "jr %s", rs_n); break;
        case R5900Op::BEQ:     std::snprintf(buf, sizeof(buf), "beq %s, %s, 0x%08X", rs_n, rt_n, jump_target); break;
        case R5900Op::BNE:     std::snprintf(buf, sizeof(buf), "bne %s, %s, 0x%08X", rs_n, rt_n, jump_target); break;
        case R5900Op::BEQL:    std::snprintf(buf, sizeof(buf), "beql %s, %s, 0x%08X", rs_n, rt_n, jump_target); break;
        case R5900Op::BNEL:    std::snprintf(buf, sizeof(buf), "bnel %s, %s, 0x%08X", rs_n, rt_n, jump_target); break;
        case R5900Op::PADDW:   std::snprintf(buf, sizeof(buf), "paddw %s, %s, %s", rd_n, rs_n, rt_n); break;
        case R5900Op::PSUBW:   std::snprintf(buf, sizeof(buf), "psubw %s, %s, %s", rd_n, rs_n, rt_n); break;
        case R5900Op::PMAXW:   std::snprintf(buf, sizeof(buf), "pmaxw %s, %s, %s", rd_n, rs_n, rt_n); break;
        case R5900Op::PMINW:   std::snprintf(buf, sizeof(buf), "pminw %s, %s, %s", rd_n, rs_n, rt_n); break;
        case R5900Op::PMADDW:  std::snprintf(buf, sizeof(buf), "pmaddw %s, %s, %s", rd_n, rs_n, rt_n); break;
        case R5900Op::PCPYLD:     std::snprintf(buf, sizeof(buf), "pcpyld %s, %s, %s", rd_n, rs_n, rt_n); break;
        case R5900Op::PCPYUD:     std::snprintf(buf, sizeof(buf), "pcpyud %s, %s, %s", rd_n, rs_n, rt_n); break;
        case R5900Op::MTC1:       std::snprintf(buf, sizeof(buf), "mtc1 %s, $f%u", rt_n, fs); break;
        case R5900Op::MFC1:       std::snprintf(buf, sizeof(buf), "mfc1 %s, $f%u", rt_n, fs); break;
        case R5900Op::FPU_ADD_S:  std::snprintf(buf, sizeof(buf), "add.s $f%u, $f%u, $f%u", fd, fs, ft); break;
        case R5900Op::FPU_SUB_S:  std::snprintf(buf, sizeof(buf), "sub.s $f%u, $f%u, $f%u", fd, fs, ft); break;
        case R5900Op::FPU_MUL_S:  std::snprintf(buf, sizeof(buf), "mul.s $f%u, $f%u, $f%u", fd, fs, ft); break;
        case R5900Op::FPU_ADDA_S: std::snprintf(buf, sizeof(buf), "adda.s $f%u, $f%u", fs, ft); break;
        case R5900Op::FPU_MADD_S: std::snprintf(buf, sizeof(buf), "madd.s $f%u, $f%u, $f%u", fd, fs, ft); break;
        case R5900Op::SYSCALL:    std::snprintf(buf, sizeof(buf), "syscall"); break;
        case R5900Op::LWL:        std::snprintf(buf, sizeof(buf), "lwl %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::LWR:        std::snprintf(buf, sizeof(buf), "lwr %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::SWL:        std::snprintf(buf, sizeof(buf), "swl %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::SWR:        std::snprintf(buf, sizeof(buf), "swr %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::LDL:        std::snprintf(buf, sizeof(buf), "ldl %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::LDR:        std::snprintf(buf, sizeof(buf), "ldr %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::SDL:        std::snprintf(buf, sizeof(buf), "sdl %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::SDR:        std::snprintf(buf, sizeof(buf), "sdr %s, %d(%s)", rt_n, simm, rs_n); break;
        case R5900Op::DI:         std::snprintf(buf, sizeof(buf), "di"); break;
        case R5900Op::EI:         std::snprintf(buf, sizeof(buf), "ei"); break;
        case R5900Op::MFC0:       std::snprintf(buf, sizeof(buf), "mfc0 %s, %d", rt_n, rd); break;
        case R5900Op::MTC0:       std::snprintf(buf, sizeof(buf), "mtc0 %s, %d", rt_n, rd); break;
        case R5900Op::PREF:       std::snprintf(buf, sizeof(buf), "pref %d, %d(%s)", rt, simm, rs_n); break;
        case R5900Op::CACHE:      std::snprintf(buf, sizeof(buf), "cache %d, %d(%s)", rt, simm, rs_n); break;
        case R5900Op::LQC2:       std::snprintf(buf, sizeof(buf), "lqc2 vf%02d, %d(%s)", ft, simm, rs_n); break;
        case R5900Op::SQC2:       std::snprintf(buf, sizeof(buf), "sqc2 vf%02d, %d(%s)", ft, simm, rs_n); break;
        case R5900Op::VU0_MACRO:  std::snprintf(buf, sizeof(buf), "vu0_macro [0x%08X]", raw); break;
        case R5900Op::PCPYH:      std::snprintf(buf, sizeof(buf), "pcpyh %s, %s", rd_n, rt_n); break;
        default:               std::snprintf(buf, sizeof(buf), "insn_0x%08X", raw); break;
    }
    return std::string(buf);
}

} // namespace emotion
