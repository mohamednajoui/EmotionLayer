#pragma once

#include "../common/types.hpp"
#include <string>
#include <cstring>
#include <emmintrin.h>

namespace emotion {

// Regiones canónicas del mapa de memoria de la PlayStation 2
enum class GuestMemoryRegion : u8 {
    MainRam,      // 32 MB Main RAM física y sus 5 espejos virtuales (0x00..., 0x20..., 0x30..., 0x80..., 0xA0...)
    Scratchpad,   // 16 KB Scratchpad SPRAM y sus espejos (0x70..., 0xD0..., 0xF0...)
    IoRegisters,  // 64 KB Registros Hardware MMIO del EE (0x10000000 - 0x1000FFFF: DMAC, INTC, Timers, SIF)
    VuRegisters,  // 64 KB Registros VU0 / VU1 (0x11000000 - 0x1100FFFF)
    GsRegisters,  // 64 KB Registros GS Privilegiados (0x12000000 - 0x1200FFFF: PMODE, DISPFB, DISPLAY, CSR)
    Invalid       // Región desmapeada / no válida (bus error o hueco sin mapear)
};

// Resultado del análisis y traducción unificada de una dirección invitada
struct GuestAddressInfo {
    GuestMemoryRegion region           = GuestMemoryRegion::Invalid;
    u32               canonical_offset = 0;     // Desplazamiento canónico físico en arena_base_
    bool              is_valid         = false; // true si pertenece a una región válida y acotada
};

// Mapa de memoria física y espejos virtuales de la PlayStation 2:
// - 32 MB Main RAM:      0x00000000 - 0x01FFFFFF (KUSEG Cached)
//                        0x20000000 - 0x21FFFFFF (KUSEG Uncached Accelerated)
//                        0x30000000 - 0x31FFFFFF (KUSEG Uncached)
//                        0x80000000 - 0x81FFFFFF (KSEG0 Kernel Cached)
//                        0xA0000000 - 0xA1FFFFFF (KSEG1 Kernel Uncached)
// - 16 KB Scratchpad:    0x70000000 - 0x70003FFF (SPRAM)
// - 64 KB I/O Registers: 0x10000000 - 0x1000FFFF (EE Hardware Registers)
// - 64 KB VU Registers:  0x11000000 - 0x1100FFFF (VU0/VU1)
// - 64 KB GS Registers:  0x12000000 - 0x1200FFFF (GS Privileged MMIO)
class MemoryArena {
public:
    static constexpr u64 kArenaSize       = 0x100000000ULL; // 4 GB espacio plano invitado
    static constexpr u64 kGuardSize       = 64 * 1024;      // 64 KB guarda al final de los 4 GB para prevenir buffer overrun host
    static constexpr u64 kTotalReserved   = kArenaSize + kGuardSize;
    static constexpr u32 kMainRamSize     = 32 * 1024 * 1024; // 32 MB
    static constexpr u32 kScratchpadMapSz = 64 * 1024;        // 64 KB (granularidad de mapeo Win32; 16 KB útiles)
    static constexpr u32 kIoRegMapSz      = 64 * 1024;        // 64 KB para registros MMIO (0x10000000)
    static constexpr u32 kVuRegMapSz      = 64 * 1024;        // 64 KB para registros VU0/VU1 (0x11000000)
    static constexpr u32 kGsRegMapSz      = 64 * 1024;        // 64 KB para registros GS Privilegiados (0x12000000)

    using MmioWriteCallback = bool (*)(u32 vaddr, u32 size_bytes, u64 value);
    using MmioReadCallback  = bool (*)(u32 vaddr, u32 size_bytes, u64& out_value);

    MemoryArena() noexcept = default;
    ~MemoryArena() noexcept;

    MemoryArena(const MemoryArena&) = delete;
    MemoryArena& operator=(const MemoryArena&) = delete;

    // Inicializa la arena de 4 GB.
    // Si force_fallback == false, intenta mapeo compartido vía VirtualAlloc2 y MapViewOfFile3 (Fastmem NT).
    // Si force_fallback == true o fallan las APIs de placeholders, activa la ruta de fallback con memoria
    // unificada donde todos los alias físicos convergen en la memoria canónica.
    bool initialize(bool force_fallback = false);

    // Libera todos los mapeos, secciones y memoria reservada en el kernel NT
    void shutdown() noexcept;

    // Puntero base de 64 bits de la arena de 4 GB (se asigna a ctx.fastmem_base)
    u8* base() const noexcept { return arena_base_; }

    // Indica si initialize() consiguió mapear vistas físicas compartidas por hardware
    bool is_hardware_mirrored() const noexcept { return hw_mirrored_; }

    // Registra callbacks para intercepción de lecturas y escrituras en registros MMIO
    static void register_mmio_callbacks(MmioWriteCallback w_cb, MmioReadCallback r_cb) noexcept {
        s_mmio_write_cb_ = w_cb;
        s_mmio_read_cb_  = r_cb;
    }

    // Desvincula callbacks MMIO limpiamente
    static void unregister_mmio_callbacks() noexcept {
        s_mmio_write_cb_ = nullptr;
        s_mmio_read_cb_  = nullptr;
    }

    // Consulta si hay callbacks MMIO registrados actualmente
    static bool has_mmio_callbacks() noexcept {
        return (s_mmio_write_cb_ != nullptr) || (s_mmio_read_cb_ != nullptr);
    }

    // Autoridad única para clasificación, límites y traducción de direcciones virtuales a desplazamiento físico
    static inline GuestAddressInfo translate_guest_addr(u32 vaddr, u32 size) noexcept {
        GuestAddressInfo info;
        if (size == 0) {
            info.is_valid = true;
            return info;
        }
        const u64 end = static_cast<u64>(vaddr) + size;
        if (end > 0x100000000ULL) {
            return info;
        }

        const u32 top = vaddr >> 28;

        // 1. 32 MB Main RAM y sus 5 espejos virtuales (KUSEG, Uncached Accel, Uncached, KSEG0, KSEG1):
        u32 seg_base = 0xFFFFFFFFU;
        if (top == 0x0)      seg_base = 0x00000000U;
        else if (top == 0x2) seg_base = 0x20000000U;
        else if (top == 0x3) seg_base = 0x30000000U;
        else if (top == 0x8) seg_base = 0x80000000U;
        else if (top == 0xA) seg_base = 0xA0000000U;

        if (seg_base != 0xFFFFFFFFU) {
            const u32 ram_offset = vaddr - seg_base;
            if (static_cast<u64>(ram_offset) + size <= kMainRamSize) {
                info.region           = GuestMemoryRegion::MainRam;
                info.canonical_offset = ram_offset; // Rango canónico 0x00000000 .. 0x01FFFFFF
                info.is_valid         = true;
                return info;
            }
        }

        // 2. 16 KB Scratchpad RAM (SPRAM) y sus espejos (0x70000000, 0xD0000000, 0xF0000000):
        u32 spr_base = 0xFFFFFFFFU;
        if (top == 0x7)      spr_base = 0x70000000U;
        else if (top == 0xD) spr_base = 0xD0000000U;
        else if (top == 0xF) spr_base = 0xF0000000U;

        if (spr_base != 0xFFFFFFFFU && ((vaddr & 0x0FFF0000U) == 0x00000000U)) {
            const u32 spr_offset = vaddr - spr_base;
            if (static_cast<u64>(spr_offset) + size <= 0x4000U) { // 16 KB efectivos
                info.region           = GuestMemoryRegion::Scratchpad;
                info.canonical_offset = 0x70000000U | spr_offset;
                info.is_valid         = true;
                return info;
            }
        }

        // 3. Registros de Hardware MMIO del Emotion Engine (0x10000000 - 0x1000FFFF):
        if (vaddr >= 0x10000000U && (end <= 0x10000000ULL + kIoRegMapSz)) {
            info.region           = GuestMemoryRegion::IoRegisters;
            info.canonical_offset = 0x10000000U | (vaddr & 0xFFFFU);
            info.is_valid         = true;
            return info;
        }

        // 4. Registros VU0 / VU1 (0x11000000 - 0x1100FFFF):
        if (vaddr >= 0x11000000U && (end <= 0x11000000ULL + kVuRegMapSz)) {
            info.region           = GuestMemoryRegion::VuRegisters;
            info.canonical_offset = 0x11000000U | (vaddr & 0xFFFFU);
            info.is_valid         = true;
            return info;
        }

        // 5. Registros Privilegiados GS (0x12000000 - 0x1200FFFF: CSR, PMODE, DISPFB, etc.):
        if (vaddr >= 0x12000000U && (end <= 0x12000000ULL + kGsRegMapSz)) {
            info.region           = GuestMemoryRegion::GsRegisters;
            info.canonical_offset = 0x12000000U | (vaddr & 0xFFFFU);
            info.is_valid         = true;
            return info;
        }

        // Cualquier otra región es desmapeada / inválida
        info.region           = GuestMemoryRegion::Invalid;
        info.canonical_offset = 0;
        info.is_valid         = false;
        return info;
    }

    // Traducción de dirección virtual de PS2 a offset físico canónico
    static inline u32 normalize_guest_addr(u32 vaddr) noexcept {
        const GuestAddressInfo info = translate_guest_addr(vaddr, 1);
        if (info.is_valid) {
            return info.canonical_offset;
        }
        return vaddr;
    }

    // Valida si un rango [guest_vaddr, guest_vaddr + size) cae dentro de regiones permitidas y acotadas
    static inline bool is_valid_guest_range(u32 guest_vaddr, u32 size) noexcept {
        if (size == 0) return true;
        const GuestAddressInfo info = translate_guest_addr(guest_vaddr, size);
        return info.is_valid;
    }

    // Lee de forma segura una cadena terminada en NUL desde la memoria del guest sin desbordar rangos válidos
    static inline bool safe_read_string(const u8* fastmem_base, u32 guest_vaddr, std::string& out_str, std::size_t max_len = 1024) noexcept {
        out_str.clear();
        if (!fastmem_base || guest_vaddr == 0) return false;

        for (std::size_t i = 0; i < max_len; ++i) {
            const u32 cur_addr = guest_vaddr + static_cast<u32>(i);
            const GuestAddressInfo info = translate_guest_addr(cur_addr, 1);
            if (!info.is_valid) {
                out_str.clear();
                return false;
            }
            const char c = static_cast<char>(fastmem_base[info.canonical_offset]);
            if (c == '\0') {
                return true;
            }
            out_str.push_back(c);
        }
        out_str.clear();
        return false;
    }

    // Lectura segura de búfer desde la memoria del guest
    static inline bool safe_read_bytes(const u8* fastmem_base, u32 guest_vaddr, void* dst, std::size_t size) noexcept {
        if (!fastmem_base || !dst) return false;
        if (size == 0) return true;
        const GuestAddressInfo info = translate_guest_addr(guest_vaddr, static_cast<u32>(size));
        if (!info.is_valid) return false;
        std::memcpy(dst, fastmem_base + info.canonical_offset, size);
        return true;
    }

    // Escritura segura de búfer hacia la memoria del guest
    static inline bool safe_write_bytes(u8* fastmem_base, u32 guest_vaddr, const void* src, std::size_t size) noexcept {
        if (!fastmem_base || !src) return false;
        if (size == 0) return true;
        const GuestAddressInfo info = translate_guest_addr(guest_vaddr, static_cast<u32>(size));
        if (!info.is_valid) return false;
        std::memcpy(fastmem_base + info.canonical_offset, src, size);
        return true;
    }

    // Accesos acotados y seguros a la arena (protegen el camino de ejecución y despachan MMIO / alias)
    template <typename T>
    static inline T fast_read(const u8* fastmem_base, u32 guest_vaddr) noexcept {
        T val{};
        if (!fastmem_base) return val;
        const GuestAddressInfo info = translate_guest_addr(guest_vaddr, sizeof(T));
        if (!info.is_valid) return val;

        if (info.region == GuestMemoryRegion::IoRegisters || info.region == GuestMemoryRegion::GsRegisters) {
            u64 mmio_val = 0;
            if (s_mmio_read_cb_ && s_mmio_read_cb_(guest_vaddr, sizeof(T), mmio_val)) {
                val = static_cast<T>(mmio_val);
                return val;
            }
        }

        std::memcpy(&val, fastmem_base + info.canonical_offset, sizeof(T));
        return val;
    }

    template <typename T>
    static inline void fast_write(u8* fastmem_base, u32 guest_vaddr, T val) noexcept {
        if (!fastmem_base) return;
        const GuestAddressInfo info = translate_guest_addr(guest_vaddr, sizeof(T));
        if (!info.is_valid) return;

        if (info.region == GuestMemoryRegion::IoRegisters || info.region == GuestMemoryRegion::GsRegisters) {
            u64 uval = 0;
            if constexpr (sizeof(T) <= 8) {
                uval = static_cast<u64>(val);
            }
            if (s_mmio_write_cb_ && s_mmio_write_cb_(guest_vaddr, sizeof(T), uval)) {
                return;
            }
        }

        std::memcpy(fastmem_base + info.canonical_offset, &val, sizeof(T));
    }

    // Obtiene un puntero tipado hacia la memoria física canónica del guest con verificación de límites, alineación y overflow
    template <typename T>
    static inline T* get_guest_ptr(u8* fastmem_base, u32 guest_vaddr, std::size_t count = 1) noexcept {
        if (!fastmem_base || count == 0) return nullptr;
        if constexpr (alignof(T) > 1) {
            if ((guest_vaddr % alignof(T)) != 0) {
                return nullptr;
            }
        }
        constexpr u64 kMaxU32 = 0xFFFFFFFFULL;
        if (count > (kMaxU32 / sizeof(T))) {
            return nullptr;
        }
        const u64 total_bytes = static_cast<u64>(sizeof(T)) * count;
        if (total_bytes > kMaxU32 || total_bytes == 0) {
            return nullptr;
        }
        const GuestAddressInfo info = translate_guest_addr(guest_vaddr, static_cast<u32>(total_bytes));
        if (!info.is_valid) return nullptr;
        return reinterpret_cast<T*>(fastmem_base + info.canonical_offset);
    }

    template <typename T>
    static inline const T* get_guest_ptr(const u8* fastmem_base, u32 guest_vaddr, std::size_t count = 1) noexcept {
        if (!fastmem_base || count == 0) return nullptr;
        if constexpr (alignof(T) > 1) {
            if ((guest_vaddr % alignof(T)) != 0) {
                return nullptr;
            }
        }
        constexpr u64 kMaxU32 = 0xFFFFFFFFULL;
        if (count > (kMaxU32 / sizeof(T))) {
            return nullptr;
        }
        const u64 total_bytes = static_cast<u64>(sizeof(T)) * count;
        if (total_bytes > kMaxU32 || total_bytes == 0) {
            return nullptr;
        }
        const GuestAddressInfo info = translate_guest_addr(guest_vaddr, static_cast<u32>(total_bytes));
        if (!info.is_valid) return nullptr;
        return reinterpret_cast<const T*>(fastmem_base + info.canonical_offset);
    }

    static inline r128 fast_read128(const u8* fastmem_base, u32 guest_vaddr) noexcept {
        r128 val{};
        if (!fastmem_base) return val;
        const u32 aligned_addr = guest_vaddr & ~0xFU;
        const GuestAddressInfo info = translate_guest_addr(aligned_addr, 16);
        if (!info.is_valid) return val;

        if (info.region == GuestMemoryRegion::IoRegisters || info.region == GuestMemoryRegion::GsRegisters) {
            u64 low_val = 0;
            u64 high_val = 0;
            bool handled_low = false;
            bool handled_high = false;
            if (s_mmio_read_cb_) {
                handled_low  = s_mmio_read_cb_(aligned_addr, 8, low_val);
                handled_high = s_mmio_read_cb_(aligned_addr + 8U, 8, high_val);
            }
            if (handled_low || handled_high) {
                if (!handled_low) {
                    std::memcpy(&low_val, fastmem_base + info.canonical_offset, 8);
                }
                if (!handled_high) {
                    std::memcpy(&high_val, fastmem_base + info.canonical_offset + 8U, 8);
                }
                val.ud[0] = low_val;
                val.ud[1] = high_val;
                return val;
            }
        }

        val.vec_i = _mm_loadu_si128(reinterpret_cast<const __m128i*>(fastmem_base + info.canonical_offset));
        return val;
    }

    static inline void fast_write128(u8* fastmem_base, u32 guest_vaddr, const r128& val) noexcept {
        if (!fastmem_base) return;
        const u32 aligned_addr = guest_vaddr & ~0xFU;
        const GuestAddressInfo info = translate_guest_addr(aligned_addr, 16);
        if (!info.is_valid) return;

        if (info.region == GuestMemoryRegion::IoRegisters || info.region == GuestMemoryRegion::GsRegisters) {
            bool handled_low = false;
            bool handled_high = false;
            if (s_mmio_write_cb_) {
                handled_low  = s_mmio_write_cb_(aligned_addr, 8, val.ud[0]);
                handled_high = s_mmio_write_cb_(aligned_addr + 8U, 8, val.ud[1]);
            }
            if (handled_low && handled_high) {
                return;
            }
            if (!handled_low) {
                std::memcpy(fastmem_base + info.canonical_offset, &val.ud[0], 8);
            }
            if (!handled_high) {
                std::memcpy(fastmem_base + info.canonical_offset + 8U, &val.ud[1], 8);
            }
            return;
        }

        _mm_storeu_si128(reinterpret_cast<__m128i*>(fastmem_base + info.canonical_offset), val.vec_i);
    }

private:
    inline static MmioWriteCallback s_mmio_write_cb_ = nullptr;
    inline static MmioReadCallback  s_mmio_read_cb_  = nullptr;

    u8*   arena_base_       = nullptr;
    void* section_ram_      = nullptr;
    void* section_spr_      = nullptr;
    void* section_io_       = nullptr;
    void* section_vu_       = nullptr;
    void* section_gs_       = nullptr;
    void* views_[32]        = {};
    u32   view_count_       = 0;
    bool  hw_mirrored_      = false;
};

} // namespace emotion
