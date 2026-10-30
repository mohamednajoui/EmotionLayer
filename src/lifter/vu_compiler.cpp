#include "vu_compiler.hpp"
#include "../gsvk/gsvk_renderer.hpp"
#include <sstream>
#include <cstring>
#include <cmath>

namespace emotion {

Vu1Engine::Vu1Engine() noexcept {
    reset();
}

void Vu1Engine::reset() noexcept {
    std::memset(micro_mem_, 0, sizeof(micro_mem_));
    std::memset(data_mem_,  0, sizeof(data_mem_));
    std::memset(vf_,        0, sizeof(vf_));
    std::memset(vi_,        0, sizeof(vi_));
    acc_ = r128::zero();
    q_   = 1.0f;

    // En hardware VU0/VU1, vf0 siempre vale (0.0f, 0.0f, 0.0f, 1.0f) y vi0 siempre vale 0
    vf_[0].f[0] = 0.0f;
    vf_[0].f[1] = 0.0f;
    vf_[0].f[2] = 0.0f;
    vf_[0].f[3] = 1.0f;
}

void Vu1Engine::upload_microcode(u32 dst_byte_addr, const u8* src_bytes, u32 byte_count) noexcept {
    if (!src_bytes || dst_byte_addr >= kMicroMemSize) return;
    const u32 max_copy = std::min(byte_count, kMicroMemSize - dst_byte_addr);
    std::memcpy(micro_mem_ + dst_byte_addr, src_bytes, max_copy);
}

void Vu1Engine::write_data_qword(u32 qword_index, const r128& data) noexcept {
    data_mem_[qword_index & 0x3FFU] = data;
}

r128 Vu1Engine::read_data_qword(u32 qword_index) const noexcept {
    return data_mem_[qword_index & 0x3FFU];
}

__m128 Vu1Engine::blend_by_mask(__m128 old_v, __m128 new_v, u8 mask) noexcept {
    // En VU1: bit 3 = X (lane 0), bit 2 = Y (lane 1), bit 1 = Z (lane 2), bit 0 = W (lane 3)
    alignas(16) f32 o[4], n[4];
    _mm_store_ps(o, old_v);
    _mm_store_ps(n, new_v);
    if (mask & 0x8) o[0] = n[0];
    if (mask & 0x4) o[1] = n[1];
    if (mask & 0x2) o[2] = n[2];
    if (mask & 0x1) o[3] = n[3];
    return _mm_load_ps(o);
}

std::string Vu1Engine::decompile_microprogram_to_cpp(u32 start_byte_addr) const {
    std::ostringstream ss;
    ss << "// Microprograma VU1 decompilado a C++20/SSE (Start: 0x" << std::hex << start_byte_addr << std::dec << ")\n";
    for (u32 pc = start_byte_addr & (kMicroMemSize - 8U); pc < kMicroMemSize; pc += 8) {
        u64 vliw = 0;
        std::memcpy(&vliw, micro_mem_ + pc, 8);
        const u32 upper = static_cast<u32>(vliw >> 32);
        const u32 lower = static_cast<u32>(vliw & 0xFFFFFFFFULL);
        const bool e_bit = (upper & 0x80000000U) != 0;
        const auto uop = static_cast<VuUpperOp>((upper >> 24) & 0x7FU);
        const auto lop = static_cast<VuLowerOp>((lower >> 25) & 0x7FU);

        ss << "  /* 0x" << std::hex << pc << std::dec << " */ uop=" << unsigned(uop) << ", lop=" << unsigned(lop) << ";\n";
        if (e_bit) break;
    }
    return ss.str();
}

void Vu1Engine::execute_mscal(u32 start_byte_addr) {
    // 1. Calcular hash FNV-1a de 64 bits del microprograma y registrarlo en la caché de compilación
    u64 hash = 14695981039346656037ULL;
    for (u32 addr = start_byte_addr & (kMicroMemSize - 8U); addr < kMicroMemSize; addr += 8) {
        u64 word = 0;
        std::memcpy(&word, micro_mem_ + addr, 8);
        hash = (hash ^ word) * 1099511628211ULL;
        if ((word >> 63) & 1ULL) break;
    }
    if (compiled_cache_.find(hash) == compiled_cache_.end()) {
        compiled_cache_[hash] = decompile_microprogram_to_cpp(start_byte_addr);
    }

    // 2. Ejecutar instrucciones VLIW de 64 bits con intrínsecos SIMD (__m128)
    u32 pc = start_byte_addr & (kMicroMemSize - 8U);
    bool finish_after_delay = false;

    for (u32 steps = 0; steps < 4096; ++steps) {
        u64 vliw = 0;
        std::memcpy(&vliw, micro_mem_ + (pc & (kMicroMemSize - 8U)), 8);
        const u32 upper  = static_cast<u32>(vliw >> 32);
        const u32 lower  = static_cast<u32>(vliw & 0xFFFFFFFFULL);
        const bool e_bit = (upper & 0x80000000U) != 0;

        // Decodificar Upper Slot (Vector Float SIMD)
        const auto uop  = static_cast<VuUpperOp>((upper >> 24) & 0x7FU);
        const u8   mask = static_cast<u8>((upper >> 20) & 0x0FU);
        const u8   ft   = static_cast<u8>((upper >> 15) & 0x1FU);
        const u8   fs   = static_cast<u8>((upper >> 10) & 0x1FU);
        const u8   fd   = static_cast<u8>((upper >> 5)  & 0x1FU);

        switch (uop) {
            case VuUpperOp::NOP:
                break;
            case VuUpperOp::VADD:
                if (fd != 0) vf_[fd].vec_f = blend_by_mask(vf_[fd].vec_f, _mm_add_ps(vf_[fs].vec_f, vf_[ft].vec_f), mask);
                break;
            case VuUpperOp::VSUB:
                if (fd != 0) vf_[fd].vec_f = blend_by_mask(vf_[fd].vec_f, _mm_sub_ps(vf_[fs].vec_f, vf_[ft].vec_f), mask);
                break;
            case VuUpperOp::VMUL:
                if (fd != 0) vf_[fd].vec_f = blend_by_mask(vf_[fd].vec_f, _mm_mul_ps(vf_[fs].vec_f, vf_[ft].vec_f), mask);
                break;
            case VuUpperOp::VMULAx: {
                const __m128 bc_x = _mm_shuffle_ps(vf_[ft].vec_f, vf_[ft].vec_f, _MM_SHUFFLE(0, 0, 0, 0));
                acc_.vec_f = blend_by_mask(acc_.vec_f, _mm_mul_ps(vf_[fs].vec_f, bc_x), mask);
                break;
            }
            case VuUpperOp::VMADDAy: {
                const __m128 bc_y = _mm_shuffle_ps(vf_[ft].vec_f, vf_[ft].vec_f, _MM_SHUFFLE(1, 1, 1, 1));
                acc_.vec_f = blend_by_mask(acc_.vec_f, _mm_add_ps(acc_.vec_f, _mm_mul_ps(vf_[fs].vec_f, bc_y)), mask);
                break;
            }
            case VuUpperOp::VMADDAz: {
                const __m128 bc_z = _mm_shuffle_ps(vf_[ft].vec_f, vf_[ft].vec_f, _MM_SHUFFLE(2, 2, 2, 2));
                acc_.vec_f = blend_by_mask(acc_.vec_f, _mm_add_ps(acc_.vec_f, _mm_mul_ps(vf_[fs].vec_f, bc_z)), mask);
                break;
            }
            case VuUpperOp::VMADDw: {
                const __m128 bc_w = _mm_shuffle_ps(vf_[ft].vec_f, vf_[ft].vec_f, _MM_SHUFFLE(3, 3, 3, 3));
                if (fd != 0) {
                    vf_[fd].vec_f = blend_by_mask(vf_[fd].vec_f, _mm_add_ps(acc_.vec_f, _mm_mul_ps(vf_[fs].vec_f, bc_w)), mask);
                }
                break;
            }
            case VuUpperOp::VFTOI0: {
                if (ft != 0) {
                    for (int c = 0; c < 4; ++c) {
                        if (mask & (1 << (3 - c))) {
                            vf_[ft].sw[c] = static_cast<s32>(vf_[fs].f[c]);
                        }
                    }
                }
                break;
            }
            case VuUpperOp::VFTOI4: {
                if (ft != 0) {
                    for (int c = 0; c < 4; ++c) {
                        if (mask & (1 << (3 - c))) {
                            vf_[ft].sw[c] = static_cast<s32>(std::round(vf_[fs].f[c] * 16.0f));
                        }
                    }
                }
                break;
            }
            case VuUpperOp::VITOF0: {
                if (ft != 0) {
                    for (int c = 0; c < 4; ++c) {
                        if (mask & (1 << (3 - c))) {
                            vf_[ft].f[c] = static_cast<f32>(vf_[fs].sw[c]);
                        }
                    }
                }
                break;
            }
        }

        // Decodificar Lower Slot (Memory / Integer / Branch / XGKICK)
        const auto lop = static_cast<VuLowerOp>((lower >> 25) & 0x7FU);
        const u8   lit = static_cast<u8>((lower >> 16) & 0x1FU);
        const u8   lis = static_cast<u8>((lower >> 11) & 0x1FU);
        s16        imm = static_cast<s16>(lower & 0x7FFU);
        if (imm & 0x400) imm |= static_cast<s16>(0xF800); // Sign-extend 11-bit immediate

        s32 branch_target_pc = -1;

        switch (lop) {
            case VuLowerOp::NOP:
                break;
            case VuLowerOp::LQ: {
                const u32 qw_addr = static_cast<u32>(static_cast<s32>(vi_[lis & 0xF]) + imm) & 0x3FFU;
                if (lit != 0) vf_[lit] = data_mem_[qw_addr];
                break;
            }
            case VuLowerOp::SQ: {
                const u32 qw_addr = static_cast<u32>(static_cast<s32>(vi_[lit & 0xF]) + imm) & 0x3FFU;
                data_mem_[qw_addr] = vf_[lis];
                break;
            }
            case VuLowerOp::LQI: {
                const u8 ireg = lis & 0xF;
                const u32 qw_addr = vi_[ireg] & 0x3FFU;
                if (lit != 0) vf_[lit] = data_mem_[qw_addr];
                if (ireg != 0) vi_[ireg]++;
                break;
            }
            case VuLowerOp::SQI: {
                const u8 ireg = lit & 0xF;
                const u32 qw_addr = vi_[ireg] & 0x3FFU;
                data_mem_[qw_addr] = vf_[lis];
                if (ireg != 0) vi_[ireg]++;
                break;
            }
            case VuLowerOp::IADDIU: {
                const u8 dst_i = lit & 0xF;
                if (dst_i != 0) {
                    vi_[dst_i] = static_cast<u16>(vi_[lis & 0xF] + static_cast<u16>(imm));
                }
                break;
            }
            case VuLowerOp::IADD: {
                const u8 dst_i = static_cast<u8>((lower >> 6) & 0xFU);
                if (dst_i != 0) {
                    vi_[dst_i] = static_cast<u16>(vi_[lis & 0xF] + vi_[lit & 0xF]);
                }
                break;
            }
            case VuLowerOp::ISUB: {
                const u8 dst_i = static_cast<u8>((lower >> 6) & 0xFU);
                if (dst_i != 0) {
                    vi_[dst_i] = static_cast<u16>(vi_[lis & 0xF] - vi_[lit & 0xF]);
                }
                break;
            }
            case VuLowerOp::IBNE: {
                if (vi_[lis & 0xF] != vi_[lit & 0xF]) {
                    branch_target_pc = static_cast<s32>(pc) + 8 + (static_cast<s32>(imm) * 8);
                }
                break;
            }
            case VuLowerOp::XGKICK: {
                // XGKICK dispara la transferencia del paquete GIF en VU1_MEM[vi[is]] hacia el GS por PATH1
                const u32 gif_qword_offset = vi_[lis & 0xF] & 0x3FFU;
                const u32 max_bytes = (1024U - gif_qword_offset) * 16U;
                const u8* pkt_ptr = reinterpret_cast<const u8*>(&data_mem_[gif_qword_offset]);
                xgkick_count_++;
                if (auto* gs = GsvkRenderer::active_instance()) {
                    gs->submit_gif_packet(pkt_ptr, max_bytes, 1); // PATH1
                }
                break;
            }
        }

        // Asegurar invariantes hardware de vf0 y vi0
        vf_[0].f[0] = 0.0f; vf_[0].f[1] = 0.0f; vf_[0].f[2] = 0.0f; vf_[0].f[3] = 1.0f;
        vi_[0] = 0;

        if (finish_after_delay) break;
        if (e_bit) {
            // En VU1, la instrucción posterior al bit E (delay slot) es la última en ejecutarse
            finish_after_delay = true;
        }

        if (branch_target_pc >= 0) {
            pc = static_cast<u32>(branch_target_pc);
        } else {
            pc += 8;
        }
    }
}

} // namespace emotion
