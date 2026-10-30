#pragma once

#include "../common/types.hpp"
#include <string>

namespace emotion {

enum class R5900Op : u16 {
    NOP = 0,
    // ALU 32/64 bits
    ADD, ADDU, ADDI, ADDIU, SUB, SUBU,
    DADDU, DADDIU, DSUBU,
    AND, ANDI, OR, ORI, XOR, XORI, NOR,
    SLT, SLTU, SLTI, SLTIU,
    LUI,
    SLL, SRL, SRA, SLLV, SRLV, SRAV,
    DSLL, DSRL, DSRA, DSLL32, DSRL32, DSRA32,
    DSLLV, DSRLV, DSRAV,
    MOVZ, MOVN,
    // Multiplicación / División (Pipeline 0 y Pipeline 1 MMI)
    MULT, MULTU, DIV, DIVU,
    MFHI, MFLO, MTHI, MTLO,
    MADD, MADDU,
    MFHI1, MFLO1, MTHI1, MTLO1, MULT1, MULTU1, DIV1, DIVU1,
    mfsa, mtsa,
    // Control Flow (Saltos y Ramas con Delay Slot)
    J, JAL, JR, JALR,
    BEQ, BNE, BLEZ, BGTZ, BLTZ, BGEZ, BLTZAL, BGEZAL,
    BEQL, BNEL, BLEZL, BGTZL, BLTZL, BGEZL,
    SYSCALL, BREAK, SYNC,
    // Memoria (8, 16, 32, 64 y 128 bits)
    LB, LBU, LH, LHU, LW, LWU, LD, LQ,
    SB, SH, SW, SD, SQ,
    // Memoria no alineada (32 y 64 bits)
    LWL, LWR, SWL, SWR,
    LDL, LDR, SDL, SDR,
    // Atómicas
    LL, SC,
    // Prefetch y Caché
    PREF, CACHE,
    // Sistema y Control COP0
    MFC0, MTC0, DI, EI,
    // Movimiento condicional en FP
    MOVF, MOVT,
    // Traps
    TEQ, TNE, TGE, TGEU, TLT, TLTU,
    // VU0 / COP2 (Macro mode y transferencias vectoriales 128-bit)
    CFC2, CTC2,
    LWC2, SWC2,
    LDC2, SDC2,
    LQC2, SQC2,
    VU0_MACRO,
    // Multimedia Instructions (MMI 128-bit SIMD - Opcode 0x1C)
    PADDB, PADDH, PADDW,
    PSUBB, PSUBH, PSUBW,
    PADDSW, PSUBSW, PADDUW, PSUBUW,
    PMAXW, PMAXH, PMINW, PMINH,
    PCGTW, PCEQW,
    PAND, POR, PXOR, PNOR,
    PMADDW,
    PCPYLD, PCPYUD, PCPYH,
    PEXTLW, PEXTUW,
    // COP1 (FPU de PS2 - Opcode 0x11)
    LWC1, SWC1, MFC1, MTC1, CFC1, CTC1,
    FPU_ADD_S, FPU_SUB_S, FPU_MUL_S, FPU_DIV_S,
    FPU_SQRT_S, FPU_ABS_S, FPU_MOV_S, FPU_NEG_S,
    FPU_MAX_S, FPU_MIN_S,
    FPU_ADDA_S, FPU_SUBA_S, FPU_MULA_S,
    FPU_MADD_S, FPU_MSUB_S,
    FPU_CVT_S_W, FPU_CVT_W_S,
    FPU_C_EQ_S, FPU_C_LT_S, FPU_C_LE_S,
    BC1F, BC1T, BC1FL, BC1TL,
    UNKNOWN
};

struct DecodedInsn {
    u32     pc          = 0;
    u32     raw         = 0;
    R5900Op op          = R5900Op::UNKNOWN;
    u8      rs          = 0;
    u8      rt          = 0;
    u8      rd          = 0;
    u8      sa          = 0;
    u8      fs          = 0;
    u8      ft          = 0;
    u8      fd          = 0;
    s16     simm        = 0;
    u16     uimm        = 0;
    u32     jump_target = 0;
    bool    is_branch   = false;
    bool    is_likely   = false; // Branch Likely (anula delay slot si no se toma la rama)
    bool    is_call     = false;

    std::string disassemble() const;
};

class R5900Decoder {
public:
    static DecodedInsn decode(u32 pc, u32 raw) noexcept;
    static const char* gpr_name(u8 idx) noexcept;
};

} // namespace emotion
