#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <cstring>
#include <immintrin.h>

namespace emotion {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using f64 = double;

// Registro de 128 bits del Emotion Engine (MIPS R5900 GPR / MMI / VU0)
// Mapeado directamente a registros SIMD de x86_64 (__m128i / __m128).
struct alignas(16) r128 {
    union {
        __m128i  vec_i;
        __m128   vec_f;
        u64      ud[2];
        s64      sd[2];
        u32      uw[4];
        s32      sw[4];
        u16      uh[8];
        s16      sh[8];
        u8       ub[16];
        s8       sb[16];
        f32      f[4];
    };

    static inline r128 zero() noexcept {
        r128 r;
        r.vec_i = _mm_setzero_si128();
        return r;
    }

    static inline r128 from_u64(u64 low, u64 high = 0) noexcept {
        r128 r;
        r.ud[0] = low;
        r.ud[1] = high;
        return r;
    }

    static inline r128 from_s32_sext(s32 val) noexcept {
        r128 r;
        r.sd[0] = static_cast<s64>(val);
        r.sd[1] = 0;
        return r;
    }
};

// Nombres arquitectónicos de los 32 registros GPR de 128 bits del R5900
enum class GprIndex : u8 {
    r0_zero = 0,
    r1_at   = 1,
    r2_v0   = 2,  r3_v1 = 3,
    r4_a0   = 4,  r5_a1 = 5,  r6_a2 = 6,  r7_a3 = 7,
    r8_t0   = 8,  r9_t1 = 9,  r10_t2 = 10, r11_t3 = 11,
    r12_t4  = 12, r13_t5 = 13, r14_t6 = 14, r15_t7 = 15,
    r16_s0  = 16, r17_s1 = 17, r18_s2 = 18, r19_s3 = 19,
    r20_s4  = 20, r21_s5 = 21, r22_s6 = 22, r23_s7 = 23,
    r24_t8  = 24, r25_t9 = 25,
    r26_k0  = 26, r27_k1 = 27,
    r28_gp  = 28,
    r29_sp  = 29,
    r30_fp  = 30,
    r31_ra  = 31
};

// Contexto completo del procesador Emotion Engine (R5900 + COP1 FPU + VU0 Macro)
struct alignas(64) EEContext {
    // 32 Registros de Propósito General de 128 bits (r0 siempre es 0)
    r128 gpr[32];

    // Registros HI y LO de 128 bits (usados por MULT/DIV y pipeline MMI HI1/LO1)
    r128 hi;
    r128 lo;

    // Registro SA (Shift Amount) de 32 bits para instrucciones QFSRV
    u32 sa;

    // Program Counter actual (32 bits en espacio virtual de PS2)
    u32 pc;

    // COP1 (FPU): 32 registros flotantes de 32 bits + acumulador ACC + FCR31 (Control/Status)
    f32 fpr[32];
    f32 fpu_acc;
    u32 fcr0;
    u32 fcr31;

    // COP2 (VU0 Macro Mode): 32 registros vectoriales de 128 bits (vf0..vf31) + 16 enteros (vi0..vi15)
    r128 vf[32];
    u16  vi[16];
    r128 vu0_acc;
    f32  vu0_q;
    f32  vu0_p;

    // Puntero directo a la base de la Arena de 4 GB en Windows (Fastmem R15)
    u8* fastmem_base;

    // Flag de terminación o salida por Syscall Exit
    bool halted;
    s32  exit_code;

    void reset(u8* arena_base, u32 entry_point = 0x00100000) noexcept {
        std::memset(this, 0, sizeof(EEContext));
        fastmem_base = arena_base;
        pc = entry_point;
        // Stack pointer inicial por defecto en la cima de los 32 MB (0x02000000 - 0x10)
        gpr[static_cast<u8>(GprIndex::r29_sp)].ud[0] = 0x01FFFFF0ULL;
        // vf0 de VU0 tiene constante hardware (x=0, y=0, z=0, w=1.0f)
        vf[0].f[0] = 0.0f;
        vf[0].f[1] = 0.0f;
        vf[0].f[2] = 0.0f;
        vf[0].f[3] = 1.0f;
        // Implementación COP1 rev del Emotion Engine
        fcr0 = 0x00002E00;
    }

    // Emulación exacta de coma flotante no-IEEE 754 del Emotion Engine (Clamping a +-MAX_FLOAT y Flush-to-Zero)
    static inline f32 sanitize_ps2_float(f32 value) noexcept {
        u32 bits;
        std::memcpy(&bits, &value, sizeof(u32));
        u32 exp = (bits >> 23) & 0xFF;
        if (exp == 0) {
            // Denormals -> Flush to signed zero
            bits &= 0x80000000U;
        } else if (exp == 0xFF) {
            // NaN / Inf en x86 -> En PS2 se satura a +-3.4028234e38 (0x7F7FFFFF / 0xFF7FFFFF)
            bits = (bits & 0x80000000U) | 0x7F7FFFFFU;
        }
        f32 out;
        std::memcpy(&out, &bits, sizeof(f32));
        return out;
    }
};

} // namespace emotion
