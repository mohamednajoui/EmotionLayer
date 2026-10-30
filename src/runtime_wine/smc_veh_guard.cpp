#include "smc_veh_guard.hpp"
#include "memory_arena.hpp"
#include "ee_syscalls.hpp"
#include "ee_kernel/kernel_runtime.hpp"
#include "diag.hpp"
#include "game_profile_hooks.hpp"
#include "../common/game_profile.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <emmintrin.h>
#include <smmintrin.h>

namespace emotion {

namespace {

// Registros MMIO con semántica de hardware (DMAC 0x10008..0x1000E, INTC 0x1000F, GS_CSR 0x12001).
// Devuelve true si el kernel consumió la escritura; false si debe escribirse como memoria normal.
inline bool try_mmio_store(u32 vaddr, u32 size_bytes, u64 value) {
    const u32 pa = vaddr & 0x1FFFFFFFU;
    const u32 page = pa >> 12;
    if (!((page >= 0x10008U && page <= 0x1000FU) || page == 0x12001U)) return false;
    if (auto* k = EEKernelRuntime::active_instance()) {
        return k->mmio_write(vaddr, size_bytes, value);
    }
    return false;
}

} // namespace

std::unordered_map<u32, RecompiledBlock> DynamicRecompCache::blocks_;
u32 DynamicRecompCache::executions_count_ = 0;

RecompiledBlock DynamicRecompCache::analyze_block(const EEContext& ctx, u32 pc) {
    RecompiledBlock blk;
    blk.start_pc = pc;

    u32 cur_pc = pc;
    u64 hash = 14695981039346656037ULL;

    for (u32 i = 0; i < 256; ++i) { // Máximo 256 instrucciones por bloque
        const u32 raw = MemoryArena::fast_read<u32>(ctx.fastmem_base, cur_pc);
        hash = (hash ^ raw) * 1099511628211ULL;

        DecodedInsn insn = R5900Decoder::decode(cur_pc, raw);
        blk.insns.push_back(insn);
        cur_pc += 4;

        if (insn.is_branch) {
            // Incluir la instrucción del delay slot
            const u32 delay_raw = MemoryArena::fast_read<u32>(ctx.fastmem_base, cur_pc);
            hash = (hash ^ delay_raw) * 1099511628211ULL;
            blk.insns.push_back(R5900Decoder::decode(cur_pc, delay_raw));
            cur_pc += 4;
            break;
        }
        if (insn.op == R5900Op::SYSCALL) {
            break;
        }
    }

    blk.end_pc = cur_pc;
    blk.insn_count = static_cast<u32>(blk.insns.size());
    blk.code_hash = hash;
    return blk;
}

void DynamicRecompCache::execute_block_interp(const RecompiledBlock& blk, EEContext& ctx) {
    auto execute_single = [&](const DecodedInsn& ins) {
        const u8 rs = ins.rs;
        const u8 rt = ins.rt;
        const u8 rd = ins.rd;
        const s32 simm = static_cast<s32>(ins.simm);

        switch (ins.op) {
            case R5900Op::NOP:
            case R5900Op::SYNC:
            case R5900Op::PREF:
            case R5900Op::CACHE:
            case R5900Op::VU0_MACRO:
                break;
            case R5900Op::ADDU:
            case R5900Op::ADD:
                if (rd) ctx.gpr[rd].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[rs].uw[0] + ctx.gpr[rt].uw[0]));
                break;
            case R5900Op::ADDIU:
            case R5900Op::ADDI:
                if (rt) ctx.gpr[rt].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[rs].uw[0] + static_cast<u32>(simm)));
                break;
            case R5900Op::SUBU:
            case R5900Op::SUB:
                if (rd) ctx.gpr[rd].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[rs].uw[0] - ctx.gpr[rt].uw[0]));
                break;
            case R5900Op::DADDU:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rs].ud[0] + ctx.gpr[rt].ud[0];
                break;
            case R5900Op::DADDIU:
                if (rt) ctx.gpr[rt].ud[0] = ctx.gpr[rs].ud[0] + static_cast<u64>(static_cast<s64>(simm));
                break;
            case R5900Op::AND:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rs].ud[0] & ctx.gpr[rt].ud[0];
                break;
            case R5900Op::OR:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rs].ud[0] | ctx.gpr[rt].ud[0];
                break;
            case R5900Op::XOR:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rs].ud[0] ^ ctx.gpr[rt].ud[0];
                break;
            case R5900Op::ANDI:
                if (rt) ctx.gpr[rt].ud[0] = ctx.gpr[rs].ud[0] & static_cast<u64>(ins.uimm);
                break;
            case R5900Op::ORI:
                if (rt) ctx.gpr[rt].ud[0] = ctx.gpr[rs].ud[0] | static_cast<u64>(ins.uimm);
                break;
            case R5900Op::XORI:
                if (rt) ctx.gpr[rt].ud[0] = ctx.gpr[rs].ud[0] ^ static_cast<u64>(ins.uimm);
                break;
            case R5900Op::LUI:
                if (rt) ctx.gpr[rt].sd[0] = static_cast<s64>(static_cast<s32>(static_cast<u32>(ins.uimm) << 16));
                break;
            case R5900Op::SLL:
                if (rd) ctx.gpr[rd].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[rt].uw[0] << ins.sa));
                break;
            case R5900Op::SRL:
                if (rd) ctx.gpr[rd].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[rt].uw[0] >> ins.sa));
                break;
            case R5900Op::SRA:
                if (rd) ctx.gpr[rd].sd[0] = static_cast<s64>(ctx.gpr[rt].sw[0] >> ins.sa);
                break;
            case R5900Op::SLLV:
                if (rd) ctx.gpr[rd].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[rt].uw[0] << (ctx.gpr[rs].uw[0] & 0x1FU)));
                break;
            case R5900Op::SRLV:
                if (rd) ctx.gpr[rd].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[rt].uw[0] >> (ctx.gpr[rs].uw[0] & 0x1FU)));
                break;
            case R5900Op::SRAV:
                if (rd) ctx.gpr[rd].sd[0] = static_cast<s64>(ctx.gpr[rt].sw[0] >> (ctx.gpr[rs].uw[0] & 0x1FU));
                break;
            case R5900Op::DSLL:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rt].ud[0] << ins.sa;
                break;
            case R5900Op::DSRL:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rt].ud[0] >> ins.sa;
                break;
            case R5900Op::DSRA:
                if (rd) ctx.gpr[rd].sd[0] = ctx.gpr[rt].sd[0] >> ins.sa;
                break;
            case R5900Op::DSLL32:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rt].ud[0] << (ins.sa + 32);
                break;
            case R5900Op::DSRL32:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rt].ud[0] >> (ins.sa + 32);
                break;
            case R5900Op::DSRA32:
                if (rd) ctx.gpr[rd].sd[0] = ctx.gpr[rt].sd[0] >> (ins.sa + 32);
                break;
            case R5900Op::DSLLV:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rt].ud[0] << (ctx.gpr[rs].uw[0] & 0x3FU);
                break;
            case R5900Op::DSRLV:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rt].ud[0] >> (ctx.gpr[rs].uw[0] & 0x3FU);
                break;
            case R5900Op::DSRAV:
                if (rd) ctx.gpr[rd].sd[0] = ctx.gpr[rt].sd[0] >> (ctx.gpr[rs].uw[0] & 0x3FU);
                break;
            case R5900Op::DSUBU:
                if (rd) ctx.gpr[rd].ud[0] = ctx.gpr[rs].ud[0] - ctx.gpr[rt].ud[0];
                break;
            case R5900Op::NOR:
                if (rd) ctx.gpr[rd].ud[0] = ~(ctx.gpr[rs].ud[0] | ctx.gpr[rt].ud[0]);
                break;
            case R5900Op::SLT:
                if (rd) ctx.gpr[rd].ud[0] = (ctx.gpr[rs].sd[0] < ctx.gpr[rt].sd[0]) ? 1 : 0;
                break;
            case R5900Op::SLTU:
                if (rd) ctx.gpr[rd].ud[0] = (ctx.gpr[rs].ud[0] < ctx.gpr[rt].ud[0]) ? 1 : 0;
                break;
            case R5900Op::SLTI:
                if (rt) ctx.gpr[rt].ud[0] = (ctx.gpr[rs].sd[0] < static_cast<s64>(simm)) ? 1 : 0;
                break;
            case R5900Op::SLTIU:
                if (rt) ctx.gpr[rt].ud[0] = (ctx.gpr[rs].ud[0] < static_cast<u64>(static_cast<s64>(simm))) ? 1 : 0;
                break;
            case R5900Op::MOVZ:
                if (rd && ctx.gpr[rt].ud[0] == 0) ctx.gpr[rd] = ctx.gpr[rs];
                break;
            case R5900Op::MOVN:
                if (rd && ctx.gpr[rt].ud[0] != 0) ctx.gpr[rd] = ctx.gpr[rs];
                break;
            case R5900Op::MOVF:
                if (rd && (ctx.fcr31 & (1u << 23)) == 0) ctx.gpr[rd] = ctx.gpr[rs];
                break;
            case R5900Op::MOVT:
                if (rd && (ctx.fcr31 & (1u << 23)) != 0) ctx.gpr[rd] = ctx.gpr[rs];
                break;

            // Multiplicacion y Division Pipeline 0
            case R5900Op::MULT: {
                const s64 prod = static_cast<s64>(ctx.gpr[rs].sw[0]) * static_cast<s64>(ctx.gpr[rt].sw[0]);
                ctx.lo.sd[0] = static_cast<s64>(static_cast<s32>(prod));
                ctx.hi.sd[0] = static_cast<s64>(static_cast<s32>(prod >> 32));
                if (rd) ctx.gpr[rd].sd[0] = ctx.lo.sd[0];
                break;
            }
            case R5900Op::MULTU: {
                const u64 prod = static_cast<u64>(ctx.gpr[rs].uw[0]) * static_cast<u64>(ctx.gpr[rt].uw[0]);
                ctx.lo.sd[0] = static_cast<s64>(static_cast<s32>(prod));
                ctx.hi.sd[0] = static_cast<s64>(static_cast<s32>(prod >> 32));
                if (rd) ctx.gpr[rd].sd[0] = ctx.lo.sd[0];
                break;
            }
            case R5900Op::DIV: {
                if (ctx.gpr[rt].sw[0] != 0) {
                    const s32 num = ctx.gpr[rs].sw[0];
                    const s32 den = ctx.gpr[rt].sw[0];
                    if (num == -2147483648 && den == -1) {
                        ctx.lo.sd[0] = static_cast<s64>(num);
                        ctx.hi.sd[0] = 0;
                    } else {
                        ctx.lo.sd[0] = static_cast<s64>(num / den);
                        ctx.hi.sd[0] = static_cast<s64>(num % den);
                    }
                } else {
                    ctx.lo.sd[0] = (ctx.gpr[rs].sw[0] < 0) ? 1 : -1;
                    ctx.hi.sd[0] = static_cast<s64>(ctx.gpr[rs].sw[0]);
                }
                break;
            }
            case R5900Op::DIVU: {
                if (ctx.gpr[rt].uw[0] != 0) {
                    const u32 num = ctx.gpr[rs].uw[0];
                    const u32 den = ctx.gpr[rt].uw[0];
                    ctx.lo.sd[0] = static_cast<s64>(static_cast<s32>(num / den));
                    ctx.hi.sd[0] = static_cast<s64>(static_cast<s32>(num % den));
                } else {
                    ctx.lo.sd[0] = -1;
                    ctx.hi.sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[rs].uw[0]));
                }
                break;
            }
            case R5900Op::MFHI:
                if (rd) ctx.gpr[rd].ud[0] = ctx.hi.ud[0];
                break;
            case R5900Op::MFLO:
                if (rd) ctx.gpr[rd].ud[0] = ctx.lo.ud[0];
                break;
            case R5900Op::MTHI:
                ctx.hi.ud[0] = ctx.gpr[rs].ud[0];
                break;
            case R5900Op::MTLO:
                ctx.lo.ud[0] = ctx.gpr[rs].ud[0];
                break;

            // Multiplicacion y Division Pipeline 1
            case R5900Op::MULT1: {
                const s64 prod = static_cast<s64>(ctx.gpr[rs].sw[0]) * static_cast<s64>(ctx.gpr[rt].sw[0]);
                ctx.lo.sd[1] = static_cast<s64>(static_cast<s32>(prod));
                ctx.hi.sd[1] = static_cast<s64>(static_cast<s32>(prod >> 32));
                if (rd) ctx.gpr[rd].sd[0] = ctx.lo.sd[1];
                break;
            }
            case R5900Op::MULTU1: {
                const u64 prod = static_cast<u64>(ctx.gpr[rs].uw[0]) * static_cast<u64>(ctx.gpr[rt].uw[0]);
                ctx.lo.sd[1] = static_cast<s64>(static_cast<s32>(prod));
                ctx.hi.sd[1] = static_cast<s64>(static_cast<s32>(prod >> 32));
                if (rd) ctx.gpr[rd].sd[0] = ctx.lo.sd[1];
                break;
            }
            case R5900Op::DIV1: {
                if (ctx.gpr[rt].sw[0] != 0) {
                    const s32 num = ctx.gpr[rs].sw[0];
                    const s32 den = ctx.gpr[rt].sw[0];
                    if (num == -2147483648 && den == -1) {
                        ctx.lo.sd[1] = static_cast<s64>(num);
                        ctx.hi.sd[1] = 0;
                    } else {
                        ctx.lo.sd[1] = static_cast<s64>(num / den);
                        ctx.hi.sd[1] = static_cast<s64>(num % den);
                    }
                } else {
                    ctx.lo.sd[1] = (ctx.gpr[rs].sw[0] < 0) ? 1 : -1;
                    ctx.hi.sd[1] = static_cast<s64>(ctx.gpr[rs].sw[0]);
                }
                break;
            }
            case R5900Op::DIVU1: {
                if (ctx.gpr[rt].uw[0] != 0) {
                    const u32 num = ctx.gpr[rs].uw[0];
                    const u32 den = ctx.gpr[rt].uw[0];
                    ctx.lo.sd[1] = static_cast<s64>(static_cast<s32>(num / den));
                    ctx.hi.sd[1] = static_cast<s64>(static_cast<s32>(num % den));
                } else {
                    ctx.lo.sd[1] = -1;
                    ctx.hi.sd[1] = static_cast<s64>(static_cast<s32>(ctx.gpr[rs].uw[0]));
                }
                break;
            }
            case R5900Op::MFHI1:
                if (rd) ctx.gpr[rd].ud[0] = ctx.hi.ud[1];
                break;
            case R5900Op::MFLO1:
                if (rd) ctx.gpr[rd].ud[0] = ctx.lo.ud[1];
                break;
            case R5900Op::MTHI1:
                ctx.hi.ud[1] = ctx.gpr[rs].ud[0];
                break;
            case R5900Op::MTLO1:
                ctx.lo.ud[1] = ctx.gpr[rs].ud[0];
                break;
            case R5900Op::MADD: {
                const s64 prod = static_cast<s64>(ctx.gpr[rs].sw[0]) * static_cast<s64>(ctx.gpr[rt].sw[0]);
                const s64 acc = (static_cast<s64>(ctx.hi.sw[0]) << 32) | static_cast<u64>(ctx.lo.uw[0]);
                const s64 res = acc + prod;
                ctx.lo.sd[0] = static_cast<s64>(static_cast<s32>(res));
                ctx.hi.sd[0] = static_cast<s64>(static_cast<s32>(res >> 32));
                if (rd) ctx.gpr[rd].sd[0] = ctx.lo.sd[0];
                break;
            }
            case R5900Op::MADDU: {
                const u64 prod = static_cast<u64>(ctx.gpr[rs].uw[0]) * static_cast<u64>(ctx.gpr[rt].uw[0]);
                const u64 acc = (static_cast<u64>(ctx.hi.uw[0]) << 32) | static_cast<u64>(ctx.lo.uw[0]);
                const u64 res = acc + prod;
                ctx.lo.sd[0] = static_cast<s64>(static_cast<s32>(res));
                ctx.hi.sd[0] = static_cast<s64>(static_cast<s32>(res >> 32));
                if (rd) ctx.gpr[rd].sd[0] = ctx.lo.sd[0];
                break;
            }
            case R5900Op::mfsa:
                if (rd) ctx.gpr[rd].ud[0] = ctx.sa;
                break;
            case R5900Op::mtsa:
                ctx.sa = ctx.gpr[rs].uw[0];
                break;

            // Multimedia Instructions 128-bit (MMI)
            case R5900Op::PADDB:
                if (rd) ctx.gpr[rd].vec_i = _mm_add_epi8(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PADDH:
                if (rd) ctx.gpr[rd].vec_i = _mm_add_epi16(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PADDW:
                if (rd) ctx.gpr[rd].vec_i = _mm_add_epi32(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PSUBB:
                if (rd) ctx.gpr[rd].vec_i = _mm_sub_epi8(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PSUBH:
                if (rd) ctx.gpr[rd].vec_i = _mm_sub_epi16(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PSUBW:
                if (rd) ctx.gpr[rd].vec_i = _mm_sub_epi32(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PADDSW:
                if (rd) {
                    for (int w = 0; w < 4; ++w) {
                        const s64 val = static_cast<s64>(ctx.gpr[rs].sw[w]) + static_cast<s64>(ctx.gpr[rt].sw[w]);
                        ctx.gpr[rd].sw[w] = (val > 0x7FFFFFFFLL) ? 0x7FFFFFFF : ((val < -0x80000000LL) ? -0x80000000 : static_cast<s32>(val));
                    }
                }
                break;
            case R5900Op::PSUBSW:
                if (rd) {
                    for (int w = 0; w < 4; ++w) {
                        const s64 val = static_cast<s64>(ctx.gpr[rs].sw[w]) - static_cast<s64>(ctx.gpr[rt].sw[w]);
                        ctx.gpr[rd].sw[w] = (val > 0x7FFFFFFFLL) ? 0x7FFFFFFF : ((val < -0x80000000LL) ? -0x80000000 : static_cast<s32>(val));
                    }
                }
                break;
            case R5900Op::PADDUW:
                if (rd) {
                    for (int w = 0; w < 4; ++w) {
                        const u64 val = static_cast<u64>(ctx.gpr[rs].uw[w]) + static_cast<u64>(ctx.gpr[rt].uw[w]);
                        ctx.gpr[rd].uw[w] = (val > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<u32>(val);
                    }
                }
                break;
            case R5900Op::PSUBUW:
                if (rd) {
                    for (int w = 0; w < 4; ++w) {
                        ctx.gpr[rd].uw[w] = (ctx.gpr[rs].uw[w] >= ctx.gpr[rt].uw[w]) ? (ctx.gpr[rs].uw[w] - ctx.gpr[rt].uw[w]) : 0;
                    }
                }
                break;
            case R5900Op::PMAXW:
                if (rd) ctx.gpr[rd].vec_i = _mm_max_epi32(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PMINW:
                if (rd) ctx.gpr[rd].vec_i = _mm_min_epi32(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PMAXH:
                if (rd) ctx.gpr[rd].vec_i = _mm_max_epi16(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PMINH:
                if (rd) ctx.gpr[rd].vec_i = _mm_min_epi16(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PCGTW:
                if (rd) ctx.gpr[rd].vec_i = _mm_cmpgt_epi32(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PCEQW:
                if (rd) ctx.gpr[rd].vec_i = _mm_cmpeq_epi32(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PAND:
                if (rd) ctx.gpr[rd].vec_i = _mm_and_si128(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::POR:
                if (rd) ctx.gpr[rd].vec_i = _mm_or_si128(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PXOR:
                if (rd) ctx.gpr[rd].vec_i = _mm_xor_si128(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i);
                break;
            case R5900Op::PNOR:
                if (rd) ctx.gpr[rd].vec_i = _mm_xor_si128(_mm_or_si128(ctx.gpr[rs].vec_i, ctx.gpr[rt].vec_i), _mm_set1_epi32(-1));
                break;
            case R5900Op::PEXTLW:
                if (rd) ctx.gpr[rd].vec_i = _mm_unpacklo_epi32(ctx.gpr[rt].vec_i, ctx.gpr[rs].vec_i);
                break;
            case R5900Op::PEXTUW:
                if (rd) ctx.gpr[rd].vec_i = _mm_unpackhi_epi32(ctx.gpr[rt].vec_i, ctx.gpr[rs].vec_i);
                break;
            case R5900Op::PCPYLD:
                if (rd) ctx.gpr[rd].vec_i = _mm_unpacklo_epi64(ctx.gpr[rt].vec_i, ctx.gpr[rs].vec_i);
                break;
            case R5900Op::PCPYUD:
                if (rd) ctx.gpr[rd].vec_i = _mm_unpackhi_epi64(ctx.gpr[rt].vec_i, ctx.gpr[rs].vec_i);
                break;
            case R5900Op::PCPYH:
                if (rd) {
                    const u64 h0 = ctx.gpr[rt].uh[0];
                    const u64 h4 = ctx.gpr[rt].uh[4];
                    ctx.gpr[rd].ud[0] = h0 * 0x0001000100010001ULL;
                    ctx.gpr[rd].ud[1] = h4 * 0x0001000100010001ULL;
                }
                break;
            case R5900Op::PMADDW: {
                for (int w = 0; w < 4; ++w) {
                    const s64 acc = (static_cast<s64>(ctx.hi.sw[w]) << 32) | static_cast<u64>(ctx.lo.uw[w]);
                    const s64 res = acc + static_cast<s64>(ctx.gpr[rs].sw[w]) * static_cast<s64>(ctx.gpr[rt].sw[w]);
                    ctx.lo.sw[w] = static_cast<s32>(res);
                    ctx.hi.sw[w] = static_cast<s32>(res >> 32);
                }
                if (rd) {
                    ctx.gpr[rd].sw[0] = ctx.lo.sw[0];
                    ctx.gpr[rd].sw[1] = ctx.lo.sw[1];
                    ctx.gpr[rd].sw[2] = ctx.lo.sw[2];
                    ctx.gpr[rd].sw[3] = ctx.lo.sw[3];
                }
                break;
            }

            // COP1 (FPU)
            case R5900Op::LWC1:
                ctx.fpr[rt] = MemoryArena::fast_read<f32>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm);
                break;
            case R5900Op::SWC1:
                MemoryArena::fast_write<f32>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm, ctx.fpr[rt]);
                break;
            case R5900Op::MFC1:
                if (rt) {
                    s32 bits;
                    std::memcpy(&bits, &ctx.fpr[ins.fs], 4);
                    ctx.gpr[rt].sd[0] = static_cast<s64>(bits);
                }
                break;
            case R5900Op::MTC1: {
                u32 bits = ctx.gpr[rt].uw[0];
                std::memcpy(&ctx.fpr[ins.fs], &bits, 4);
                break;
            }
            case R5900Op::CFC1:
                if (rt) ctx.gpr[rt].sd[0] = static_cast<s64>(static_cast<s32>(ctx.fcr31));
                break;
            case R5900Op::CTC1:
                ctx.fcr31 = ctx.gpr[rt].uw[0];
                break;
            case R5900Op::FPU_ADD_S:
                ctx.fpr[ins.fd] = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) + EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]));
                break;
            case R5900Op::FPU_SUB_S:
                ctx.fpr[ins.fd] = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) - EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]));
                break;
            case R5900Op::FPU_MUL_S:
                ctx.fpr[ins.fd] = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) * EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]));
                break;
            case R5900Op::FPU_DIV_S:
                ctx.fpr[ins.fd] = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) / EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]));
                break;
            case R5900Op::FPU_SQRT_S:
                ctx.fpr[ins.fd] = EEContext::sanitize_ps2_float(std::sqrt(EEContext::sanitize_ps2_float(ctx.fpr[ins.fs])));
                break;
            case R5900Op::FPU_ABS_S:
                ctx.fpr[ins.fd] = std::fabs(ctx.fpr[ins.fs]);
                break;
            case R5900Op::FPU_MOV_S:
                ctx.fpr[ins.fd] = ctx.fpr[ins.fs];
                break;
            case R5900Op::FPU_NEG_S:
                ctx.fpr[ins.fd] = -ctx.fpr[ins.fs];
                break;
            case R5900Op::FPU_ADDA_S:
                ctx.fpu_acc = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) + EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]));
                break;
            case R5900Op::FPU_SUBA_S:
                ctx.fpu_acc = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) - EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]));
                break;
            case R5900Op::FPU_MULA_S:
                ctx.fpu_acc = EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) * EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]));
                break;
            case R5900Op::FPU_MADD_S:
                ctx.fpr[ins.fd] = EEContext::sanitize_ps2_float(ctx.fpu_acc + EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) * EEContext::sanitize_ps2_float(ctx.fpr[ins.ft])));
                break;
            case R5900Op::FPU_MSUB_S:
                ctx.fpr[ins.fd] = EEContext::sanitize_ps2_float(ctx.fpu_acc - EEContext::sanitize_ps2_float(EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) * EEContext::sanitize_ps2_float(ctx.fpr[ins.ft])));
                break;
            case R5900Op::FPU_MAX_S: {
                const f32 a = EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]);
                const f32 b = EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]);
                ctx.fpr[ins.fd] = (a >= b) ? a : b;
                break;
            }
            case R5900Op::FPU_MIN_S: {
                const f32 a = EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]);
                const f32 b = EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]);
                ctx.fpr[ins.fd] = (a <= b) ? a : b;
                break;
            }
            case R5900Op::FPU_CVT_S_W: {
                s32 w;
                std::memcpy(&w, &ctx.fpr[ins.fs], 4);
                ctx.fpr[ins.fd] = static_cast<f32>(w);
                break;
            }
            case R5900Op::FPU_CVT_W_S: {
                s32 w = static_cast<s32>(ctx.fpr[ins.fs]);
                std::memcpy(&ctx.fpr[ins.fd], &w, 4);
                break;
            }
            case R5900Op::FPU_C_EQ_S:
                if (EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) == EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]))
                    ctx.fcr31 |= (1u << 23);
                else
                    ctx.fcr31 &= ~(1u << 23);
                break;
            case R5900Op::FPU_C_LT_S:
                if (EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) < EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]))
                    ctx.fcr31 |= (1u << 23);
                else
                    ctx.fcr31 &= ~(1u << 23);
                break;
            case R5900Op::FPU_C_LE_S:
                if (EEContext::sanitize_ps2_float(ctx.fpr[ins.fs]) <= EEContext::sanitize_ps2_float(ctx.fpr[ins.ft]))
                    ctx.fcr31 |= (1u << 23);
                else
                    ctx.fcr31 &= ~(1u << 23);
                break;

            // Traps
            case R5900Op::TEQ:
            case R5900Op::TNE:
            case R5900Op::TGE:
            case R5900Op::TGEU:
            case R5900Op::TLT:
            case R5900Op::TLTU:
                break;

            // Memoria
            case R5900Op::LB:
                if (rt) ctx.gpr[rt].sd[0] = static_cast<s64>(MemoryArena::fast_read<s8>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm));
                break;
            case R5900Op::LBU:
                if (rt) ctx.gpr[rt].ud[0] = static_cast<u64>(MemoryArena::fast_read<u8>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm));
                break;
            case R5900Op::LH:
                if (rt) ctx.gpr[rt].sd[0] = static_cast<s64>(MemoryArena::fast_read<s16>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm));
                break;
            case R5900Op::LHU:
                if (rt) ctx.gpr[rt].ud[0] = static_cast<u64>(MemoryArena::fast_read<u16>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm));
                break;
            case R5900Op::LW:
            case R5900Op::LL:
                if (rt) ctx.gpr[rt].sd[0] = static_cast<s64>(static_cast<s32>(MemoryArena::fast_read<u32>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm)));
                if (Diag::enabled()) Diag::get().on_load(ctx.gpr[rs].uw[0] + simm, ctx.gpr[rt].uw[0], ins.pc);
                break;
            case R5900Op::LWU:
                if (rt) ctx.gpr[rt].ud[0] = static_cast<u64>(MemoryArena::fast_read<u32>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm));
                break;
            case R5900Op::LD:
                if (rt) ctx.gpr[rt].ud[0] = MemoryArena::fast_read<u64>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm);
                break;
            case R5900Op::LQ:
                if (rt) ctx.gpr[rt] = MemoryArena::fast_read128(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm);
                break;
            case R5900Op::LQC2:
                ctx.vf[ins.ft] = MemoryArena::fast_read128(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm);
                break;
            case R5900Op::LWC2:
                ctx.vf[ins.ft].uw[0] = MemoryArena::fast_read<u32>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm);
                break;
            case R5900Op::LDC2:
                ctx.vf[ins.ft].ud[0] = MemoryArena::fast_read<u64>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm);
                break;

            // Unaligned loads
            case R5900Op::LWL:
                if (rt) {
                    const u32 va = ctx.gpr[rs].uw[0] + simm;
                    const u32 shift = (va & 3) * 8;
                    const u32 mem = MemoryArena::fast_read<u32>(ctx.fastmem_base, va & ~3u);
                    static const u32 mask[4] = { 0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF };
                    ctx.gpr[rt].uw[0] = (ctx.gpr[rt].uw[0] & mask[va & 3]) | (mem << (24 - shift));
                    ctx.gpr[rt].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[rt].uw[0]));
                }
                break;
            case R5900Op::LWR:
                if (rt) {
                    const u32 va = ctx.gpr[rs].uw[0] + simm;
                    const u32 shift = (va & 3) * 8;
                    const u32 mem = MemoryArena::fast_read<u32>(ctx.fastmem_base, va & ~3u);
                    static const u32 mask[4] = { 0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000 };
                    ctx.gpr[rt].uw[0] = (ctx.gpr[rt].uw[0] & mask[va & 3]) | (mem >> shift);
                    ctx.gpr[rt].sd[0] = static_cast<s64>(static_cast<s32>(ctx.gpr[rt].uw[0]));
                }
                break;
            case R5900Op::LDL:
                if (rt) {
                    const u32 va = ctx.gpr[rs].uw[0] + simm;
                    const u32 shift = (va & 7) * 8;
                    const u64 mem = MemoryArena::fast_read<u64>(ctx.fastmem_base, va & ~7u);
                    const u64 mask = (shift == 0) ? 0ULL : (~0ULL >> (64 - shift));
                    ctx.gpr[rt].ud[0] = (ctx.gpr[rt].ud[0] & mask) | (mem << (56 - shift));
                }
                break;
            case R5900Op::LDR:
                if (rt) {
                    const u32 va = ctx.gpr[rs].uw[0] + simm;
                    const u32 shift = (va & 7) * 8;
                    const u64 mem = MemoryArena::fast_read<u64>(ctx.fastmem_base, va & ~7u);
                    const u64 mask = (shift == 56) ? 0ULL : (~0ULL << (shift + 8));
                    ctx.gpr[rt].ud[0] = (ctx.gpr[rt].ud[0] & mask) | (mem >> shift);
                }
                break;

            // Stores
            case R5900Op::SB:
                MemoryArena::fast_write<u8>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm, ctx.gpr[rt].ub[0]);
                break;
            case R5900Op::SH: {
                const u32 va = ctx.gpr[rs].uw[0] + simm;
                if (!try_mmio_store(va, 2, ctx.gpr[rt].uh[0])) {
                    MemoryArena::fast_write<u16>(ctx.fastmem_base, va, ctx.gpr[rt].uh[0]);
                }
                break;
            }
            case R5900Op::SW: {
                const u32 va = ctx.gpr[rs].uw[0] + simm;
                if (!try_mmio_store(va, 4, ctx.gpr[rt].uw[0])) {
                    MemoryArena::fast_write<u32>(ctx.fastmem_base, va, ctx.gpr[rt].uw[0]);
                }
                break;
            }
            case R5900Op::SC:
                MemoryArena::fast_write<u32>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm, ctx.gpr[rt].uw[0]);
                if (rt) ctx.gpr[rt].ud[0] = 1;
                break;
            case R5900Op::SD: {
                const u32 va = ctx.gpr[rs].uw[0] + simm;
                if (!try_mmio_store(va, 8, ctx.gpr[rt].ud[0])) {
                    MemoryArena::fast_write<u64>(ctx.fastmem_base, va, ctx.gpr[rt].ud[0]);
                }
                break;
            }
            case R5900Op::SQ:
                MemoryArena::fast_write128(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm, ctx.gpr[rt]);
                break;
            case R5900Op::SQC2:
                MemoryArena::fast_write128(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm, ctx.vf[ins.ft]);
                break;
            case R5900Op::SWC2:
                MemoryArena::fast_write<u32>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm, ctx.vf[ins.ft].uw[0]);
                break;
            case R5900Op::SDC2:
                MemoryArena::fast_write<u64>(ctx.fastmem_base, ctx.gpr[rs].uw[0] + simm, ctx.vf[ins.ft].ud[0]);
                break;
            case R5900Op::SWL: {
                const u32 va = ctx.gpr[rs].uw[0] + simm;
                const u32 shift = (va & 3) * 8;
                u32 mem = MemoryArena::fast_read<u32>(ctx.fastmem_base, va & ~3u);
                static const u32 mask[4] = { 0xFFFFFF00, 0xFFFF0000, 0xFF000000, 0x00000000 };
                mem = (mem & mask[va & 3]) | (ctx.gpr[rt].uw[0] >> (24 - shift));
                MemoryArena::fast_write<u32>(ctx.fastmem_base, va & ~3u, mem);
                break;
            }
            case R5900Op::SWR: {
                const u32 va = ctx.gpr[rs].uw[0] + simm;
                const u32 shift = (va & 3) * 8;
                u32 mem = MemoryArena::fast_read<u32>(ctx.fastmem_base, va & ~3u);
                static const u32 mask[4] = { 0x00000000, 0x000000FF, 0x0000FFFF, 0x00FFFFFF };
                mem = (mem & mask[va & 3]) | (ctx.gpr[rt].uw[0] << shift);
                MemoryArena::fast_write<u32>(ctx.fastmem_base, va & ~3u, mem);
                break;
            }
            case R5900Op::SDL: {
                const u32 va = ctx.gpr[rs].uw[0] + simm;
                const u32 shift = (va & 7) * 8;
                u64 mem = MemoryArena::fast_read<u64>(ctx.fastmem_base, va & ~7u);
                const u64 mask = (shift == 56) ? 0ULL : (~0ULL << (shift + 8));
                mem = (mem & mask) | (ctx.gpr[rt].ud[0] >> (56 - shift));
                MemoryArena::fast_write<u64>(ctx.fastmem_base, va & ~7u, mem);
                break;
            }
            case R5900Op::SDR: {
                const u32 va = ctx.gpr[rs].uw[0] + simm;
                const u32 shift = (va & 7) * 8;
                u64 mem = MemoryArena::fast_read<u64>(ctx.fastmem_base, va & ~7u);
                const u64 mask = (shift == 0) ? 0ULL : (~0ULL >> (64 - shift));
                mem = (mem & mask) | (ctx.gpr[rt].ud[0] << shift);
                MemoryArena::fast_write<u64>(ctx.fastmem_base, va & ~7u, mem);
                break;
            }

            // COP0 y COP2
            case R5900Op::MFC0:
                if (rt) {
                    const auto* k = EEKernelRuntime::active_instance();
                    const u32 v = k ? k->cop0_read(rd) : 0U;
                    ctx.gpr[rt].sd[0] = static_cast<s64>(static_cast<s32>(v));
                }
                break;
            case R5900Op::MTC0:
                if (auto* k = EEKernelRuntime::active_instance()) k->cop0_write(rd, ctx.gpr[rt].uw[0]);
                break;
            case R5900Op::DI:
                if (auto* k = EEKernelRuntime::active_instance()) k->cpu_set_int_enable(false);
                break;
            case R5900Op::EI:
                if (auto* k = EEKernelRuntime::active_instance()) k->cpu_set_int_enable(true);
                break;
            case R5900Op::CFC2:
                if (rt) ctx.gpr[rt].ud[0] = ctx.vi[rd & 0x0F];
                break;
            case R5900Op::CTC2:
                ctx.vi[rd & 0x0F] = static_cast<u16>(ctx.gpr[rt].uw[0]);
                break;

            // Syscall
            case R5900Op::SYSCALL:
                ctx.pc = ins.pc;
                EESyscalls::dispatch(ctx);
                break;

            default:
                if (Diag::enabled()) Diag::get().on_unimpl(ins.pc, static_cast<u32>(ins.op));
                break;
        }
        ctx.gpr[0] = r128::zero();
    };

    const std::size_t n = blk.insns.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (ctx.halted) return;
        const auto& d = blk.insns[i];

        if (d.is_branch) {
            DecodedInsn delay_insn{};
            if (i + 1 < n && blk.insns[i + 1].pc == d.pc + 4) {
                delay_insn = blk.insns[i + 1];
            } else {
                delay_insn.pc = d.pc + 4;
                delay_insn.op = R5900Op::NOP;
            }
            bool taken = false;

            switch (d.op) {
                case R5900Op::J:
                case R5900Op::JAL:
                case R5900Op::JR:
                case R5900Op::JALR:
                    taken = true;
                    break;
                case R5900Op::BEQ:
                case R5900Op::BEQL:
                    taken = (ctx.gpr[d.rs].ud[0] == ctx.gpr[d.rt].ud[0]);
                    break;
                case R5900Op::BNE:
                case R5900Op::BNEL:
                    taken = (ctx.gpr[d.rs].ud[0] != ctx.gpr[d.rt].ud[0]);
                    break;
                case R5900Op::BLEZ:
                case R5900Op::BLEZL:
                    taken = (ctx.gpr[d.rs].sd[0] <= 0);
                    break;
                case R5900Op::BGTZ:
                case R5900Op::BGTZL:
                    taken = (ctx.gpr[d.rs].sd[0] > 0);
                    break;
                case R5900Op::BLTZ:
                case R5900Op::BLTZL:
                case R5900Op::BLTZAL:
                    taken = (ctx.gpr[d.rs].sd[0] < 0);
                    break;
                case R5900Op::BGEZ:
                case R5900Op::BGEZL:
                case R5900Op::BGEZAL:
                    taken = (ctx.gpr[d.rs].sd[0] >= 0);
                    break;
                case R5900Op::BC1F:
                case R5900Op::BC1FL:
                    taken = ((ctx.fcr31 & (1u << 23)) == 0);
                    break;
                case R5900Op::BC1T:
                case R5900Op::BC1TL:
                    taken = ((ctx.fcr31 & (1u << 23)) != 0);
                    break;
                default:
                    break;
            }

            u32 target = d.jump_target;
            if (d.op == R5900Op::JR || d.op == R5900Op::JALR) {
                target = ctx.gpr[d.rs].uw[0];
            }
            if (d.is_call) {
                const u8 link = (d.op == R5900Op::JALR) ? d.rd : 31;
                if (link) ctx.gpr[link].sd[0] = static_cast<s64>(static_cast<s32>(d.pc + 8));
            }

            if (d.is_likely) {
                if (taken) {
                    execute_single(delay_insn);
                    ctx.pc = target;
                } else {
                    ctx.pc = d.pc + 8;
                }
            } else {
                execute_single(delay_insn);
                if (taken) {
                    ctx.pc = target;
                } else {
                    ctx.pc = d.pc + 8;
                }
            }
            return;
        }

        execute_single(d);
        ctx.pc = d.pc + 4;
    }
}

static thread_local u32 s_last_pc = 0xFFFFFFFFU;
static thread_local const RecompiledBlock* s_last_block = nullptr;

bool DynamicRecompCache::execute_or_compile(EEContext& ctx) {
    // Punto seguro para interrupciones: inicio de bloque básico, sin delay slots pendientes.
    if (auto* k = EEKernelRuntime::active_instance()) {
        k->on_block_boundary(ctx);
    }

    // Aislamiento de traps PC de compatibilidad histórica (P3)
    GameProfileHooks::handle_smc_pc_trap(ctx, EEKernelRuntime::active_instance());

    const RecompiledBlock* blk_ptr = nullptr;
    if (ctx.pc == s_last_pc && s_last_block) {
        blk_ptr = s_last_block;
    } else {
        auto it = blocks_.find(ctx.pc);
        if (it == blocks_.end()) {
            RecompiledBlock blk = analyze_block(ctx, ctx.pc);
            blocks_[ctx.pc] = blk;
            it = blocks_.find(ctx.pc);
        }
        blk_ptr = &it->second;
        s_last_pc = ctx.pc;
        s_last_block = blk_ptr;
    }

    executions_count_++;
    if (Diag::enabled()) {
        auto* k = EEKernelRuntime::active_instance();
        Diag::get().on_block(k ? k->current_thread_id() : 1, ctx.pc);
    }
    execute_block_interp(*blk_ptr, ctx);
    return true;
}

void DynamicRecompCache::invalidate_range(u32 start_vaddr, u32 size_bytes) {
    s_last_pc = 0xFFFFFFFFU;
    s_last_block = nullptr;
    const u32 end_vaddr = start_vaddr + size_bytes;
    for (auto it = blocks_.begin(); it != blocks_.end(); ) {
        if (it->first >= start_vaddr && it->first < end_vaddr) {
            it = blocks_.erase(it);
        } else {
            ++it;
        }
    }
}

void DynamicRecompCache::reset() {
    s_last_pc = 0xFFFFFFFFU;
    s_last_block = nullptr;
    blocks_.clear();
    executions_count_ = 0;
}

} // namespace emotion
