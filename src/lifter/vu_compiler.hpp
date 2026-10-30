#pragma once

#include "../common/types.hpp"
#include <string>
#include <vector>
#include <map>

namespace emotion {

// Opcodes de la unidad vectorial VU1 (Micro Mode VLIW de 64 bits: Upper 32-bit + Lower 32-bit)
enum class VuUpperOp : u8 {
    NOP     = 0,
    VADD    = 1,
    VSUB    = 2,
    VMUL    = 3,
    VMULAx  = 4,
    VMADDAy = 5,
    VMADDAz = 6,
    VMADDw  = 7,
    VFTOI0  = 8,
    VFTOI4  = 9,
    VITOF0  = 10
};

enum class VuLowerOp : u8 {
    NOP     = 0,
    LQ      = 1,
    SQ      = 2,
    LQI     = 3,
    SQI     = 4,
    IADDIU  = 5,
    IADD    = 6,
    ISUB    = 7,
    IBNE    = 8,
    XGKICK  = 9
};

// Motor y Transpilador SIMD (SSE4.1/AVX2) de Microcódigo para VU1 (Vector Unit 1)
class Vu1Engine {
public:
    static constexpr u32 kMicroMemSize = 16 * 1024; // 16 KB de instrucciones VLIW (2048 instrucciones de 64 bits)
    static constexpr u32 kDataMemSize  = 16 * 1024; // 16 KB de memoria de datos (1024 quadwords de 128 bits)

    Vu1Engine() noexcept;

    void reset() noexcept;

    // Sube instrucciones de 64 bits a VU1_MICRO (usado por tag VIF1 MPG)
    void upload_microcode(u32 dst_byte_addr, const u8* src_bytes, u32 byte_count) noexcept;

    // Escribe datos desempaquetados en VU1_MEM (usado por tag VIF1 UNPACK)
    void write_data_qword(u32 qword_index, const r128& data) noexcept;
    r128 read_data_qword(u32 qword_index) const noexcept;

    // Compila (si no está en caché por hash XXH/FNV) y ejecuta el microprograma con intrínsecos SSE/AVX
    void execute_mscal(u32 start_byte_addr);

    // Emite el código fuente C++/AVX equivalente de un microprograma VU1 para inspección o compilación AOT
    std::string decompile_microprogram_to_cpp(u32 start_byte_addr) const;

    u32 microprograms_compiled() const noexcept { return static_cast<u32>(compiled_cache_.size()); }
    u32 xgkick_count() const noexcept { return xgkick_count_; }

    // Helpers para ensamblar instrucciones VLIW de 64 bits de VU1
    static constexpr u64 encode_vliw(
        bool e_bit,
        VuUpperOp uop, u8 dest_mask, u8 ft, u8 fs, u8 fd,
        VuLowerOp lop, u8 l_it, u8 l_is, s16 imm11
    ) {
        const u32 upper = (static_cast<u32>(uop) << 24) |
                          (static_cast<u32>(dest_mask & 0xF) << 20) |
                          (static_cast<u32>(ft & 0x1F) << 15) |
                          (static_cast<u32>(fs & 0x1F) << 10) |
                          (static_cast<u32>(fd & 0x1F) << 5);
        const u32 lower = (static_cast<u32>(lop) << 25) |
                          (static_cast<u32>(l_it & 0x1F) << 16) |
                          (static_cast<u32>(l_is & 0x1F) << 11) |
                          (static_cast<u16>(imm11) & 0x7FFU);
        return (static_cast<u64>(upper | (e_bit ? 0x80000000U : 0U)) << 32) | static_cast<u64>(lower);
    }

private:
    static inline __m128 blend_by_mask(__m128 old_v, __m128 new_v, u8 mask) noexcept;

    alignas(16) u8   micro_mem_[kMicroMemSize];
    alignas(16) r128 data_mem_[kDataMemSize / 16];
    alignas(16) r128 vf_[32];
    u16              vi_[16];
    r128             acc_;
    f32              q_;
    u32              xgkick_count_ = 0;

    std::map<u64, std::string> compiled_cache_;
};

} // namespace emotion
