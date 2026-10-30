#include "memory_arena.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <memoryapi.h>
#include <cstdio>

// Cargamos dinámicamente VirtualAlloc2 y MapViewOfFile3 desde kernelbase.dll
// para máxima compatibilidad con cualquier configuración del SDK de Windows.
namespace {

using PFN_VirtualAlloc2 = PVOID(WINAPI*)(
    HANDLE Process,
    PVOID BaseAddress,
    SIZE_T Size,
    ULONG AllocationType,
    ULONG PageProtection,
    MEM_EXTENDED_PARAMETER* ExtendedParameters,
    ULONG ParameterCount
);

using PFN_MapViewOfFile3 = PVOID(WINAPI*)(
    HANDLE FileMapping,
    HANDLE Process,
    PVOID BaseAddress,
    ULONG64 Offset,
    SIZE_T ViewSize,
    ULONG AllocationType,
    ULONG PageProtection,
    MEM_EXTENDED_PARAMETER* ExtendedParameters,
    ULONG ParameterCount
);

} // namespace

namespace emotion {

MemoryArena::~MemoryArena() noexcept {
    shutdown();
}

bool MemoryArena::initialize(bool force_fallback) {
    shutdown();

    if (!force_fallback) {
        HMODULE hKernelBase = GetModuleHandleW(L"kernelbase.dll");
        auto pVirtualAlloc2 = hKernelBase
            ? reinterpret_cast<PFN_VirtualAlloc2>(GetProcAddress(hKernelBase, "VirtualAlloc2"))
            : nullptr;
        auto pMapViewOfFile3 = hKernelBase
            ? reinterpret_cast<PFN_MapViewOfFile3>(GetProcAddress(hKernelBase, "MapViewOfFile3"))
            : nullptr;

        if (pVirtualAlloc2 && pMapViewOfFile3) {
            // 1. Reservamos 4 GB de espacio plano + 64 KB de guarda con placeholders (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER)
            void* base = pVirtualAlloc2(
                GetCurrentProcess(),
                nullptr,
                static_cast<SIZE_T>(kTotalReserved),
                MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                PAGE_NOACCESS,
                nullptr,
                0
            );

            if (base) {
                arena_base_ = static_cast<u8*>(base);

                // 2. Creamos objetos de sección (Pagefile-backed) con validación estricta de cada descriptor
                section_ram_ = CreateFileMappingW(
                    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kMainRamSize, nullptr);
                section_spr_ = CreateFileMappingW(
                    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kScratchpadMapSz, nullptr);
                section_io_ = CreateFileMappingW(
                    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kIoRegMapSz, nullptr);
                section_vu_ = CreateFileMappingW(
                    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kVuRegMapSz, nullptr);
                section_gs_ = CreateFileMappingW(
                    INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kGsRegMapSz, nullptr);

                const bool sections_valid = (section_ram_ && section_ram_ != INVALID_HANDLE_VALUE) &&
                                            (section_spr_ && section_spr_ != INVALID_HANDLE_VALUE) &&
                                            (section_io_  && section_io_  != INVALID_HANDLE_VALUE) &&
                                            (section_vu_  && section_vu_  != INVALID_HANDLE_VALUE) &&
                                            (section_gs_  && section_gs_  != INVALID_HANDLE_VALUE);

                if (sections_valid) {
                    auto map_region = [&](u32 guest_offset, u32 size, HANDLE section) -> bool {
                        if (!section || section == INVALID_HANDLE_VALUE) return false;

                        // Dividimos el placeholder exactamente en el rango solicitado
                        const BOOL free_ok = VirtualFree(
                            arena_base_ + guest_offset,
                            size,
                            MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER
                        );
                        if (!free_ok) {
                            return false;
                        }

                        void* view = pMapViewOfFile3(
                            section,
                            GetCurrentProcess(),
                            arena_base_ + guest_offset,
                            0,
                            size,
                            MEM_REPLACE_PLACEHOLDER,
                            PAGE_READWRITE,
                            nullptr,
                            0
                        );
                        if (!view || view != (arena_base_ + guest_offset)) {
                            return false;
                        }
                        if (view_count_ < sizeof(views_) / sizeof(views_[0])) {
                            views_[view_count_++] = view;
                        }
                        return true;
                    };

                    // Mapeamos los 5 espejos físicos de los 32 MB de RAM principal del Emotion Engine:
                    // KUSEG (0x00000000), Uncached Accel (0x20000000), Uncached (0x30000000),
                    // KSEG0 (0x80000000) y KSEG1 (0xA0000000) apuntan a las mismas páginas físicas.
                    bool ok = true;
                    ok = ok && map_region(0x00000000U, kMainRamSize, section_ram_);
                    ok = ok && map_region(0x20000000U, kMainRamSize, section_ram_);
                    ok = ok && map_region(0x30000000U, kMainRamSize, section_ram_);
                    ok = ok && map_region(0x80000000U, kMainRamSize, section_ram_);
                    ok = ok && map_region(0xA0000000U, kMainRamSize, section_ram_);

                    // Scratchpad RAM en 0x70000000, 0xD0000000 y 0xF0000000 (16 KB útiles dentro de página de 64 KB)
                    ok = ok && map_region(0x70000000U, kScratchpadMapSz, section_spr_);
                    ok = ok && map_region(0xD0000000U, kScratchpadMapSz, section_spr_);
                    ok = ok && map_region(0xF0000000U, kScratchpadMapSz, section_spr_);

                    // Registros I/O del Emotion Engine en 0x10000000
                    ok = ok && map_region(0x10000000U, kIoRegMapSz, section_io_);

                    // Registros VU0 / VU1 en 0x11000000
                    ok = ok && map_region(0x11000000U, kVuRegMapSz, section_vu_);

                    // Registros Privilegiados del Graphics Synthesizer en 0x12000000 (GS_CSR, GS_PMODE, etc.)
                    ok = ok && map_region(0x12000000U, kGsRegMapSz, section_gs_);

                    if (ok) {
                        hw_mirrored_ = true;
                        std::memset(arena_base_ + 0x00000000U, 0, kMainRamSize);
                        std::memset(arena_base_ + 0x70000000U, 0, kScratchpadMapSz);
                        std::memset(arena_base_ + 0x10000000U, 0, kIoRegMapSz);
                        std::memset(arena_base_ + 0x11000000U, 0, kVuRegMapSz);
                        std::memset(arena_base_ + 0x12000000U, 0, kGsRegMapSz);

                        // GS_CSR (0x12001000): Bits 16..23 = GS revision ID (0x20 para CXD2947GS), Bits 24..31 = ID (0x50)
                        *reinterpret_cast<u64*>(arena_base_ + 0x12001000U) = 0x0000000050200000ULL;
                        return true;
                    }
                }
                shutdown();
            }
        }
    }

    // Ruta de Fallback coherente (sin VirtualAlloc2 o forzada):
    // Reserva los 4 GB de espacio virtual + 64 KB de guarda mediante VirtualAlloc estándar.
    void* base = VirtualAlloc(nullptr, static_cast<SIZE_T>(kTotalReserved), MEM_RESERVE, PAGE_NOACCESS);
    if (!base) {
        return false;
    }
    arena_base_ = static_cast<u8*>(base);

    // En el modo fallback, para garantizar coherencia absoluta entre los 5 alias de RAM y evitar
    // que 0x00000000, 0x20000000, etc. diverjan en copias separadas, se compromete ÚNICAMENTE
    // la memoria canónica física (32 MB de RAM en 0x00000000, Scratchpad en 0x70000000, y MMIOs).
    // Toda lectura y escritura en la arena se traduce de forma centralizada por translate_guest_addr(),
    // convergiendo todos los alias virtuales en la misma memoria física canónica.
    auto commit_check = [&](u32 offset, u32 size) -> bool {
        void* ptr = VirtualAlloc(arena_base_ + offset, size, MEM_COMMIT, PAGE_READWRITE);
        return ptr != nullptr;
    };

    bool commit_ok = true;
    commit_ok = commit_ok && commit_check(0x00000000U, kMainRamSize);
    commit_ok = commit_ok && commit_check(0x70000000U, kScratchpadMapSz);
    commit_ok = commit_ok && commit_check(0x10000000U, kIoRegMapSz);
    commit_ok = commit_ok && commit_check(0x11000000U, kVuRegMapSz);
    commit_ok = commit_ok && commit_check(0x12000000U, kGsRegMapSz);

    if (!commit_ok) {
        shutdown();
        return false;
    }

    std::memset(arena_base_ + 0x00000000U, 0, kMainRamSize);
    std::memset(arena_base_ + 0x70000000U, 0, kScratchpadMapSz);
    std::memset(arena_base_ + 0x10000000U, 0, kIoRegMapSz);
    std::memset(arena_base_ + 0x11000000U, 0, kVuRegMapSz);
    std::memset(arena_base_ + 0x12000000U, 0, kGsRegMapSz);

    *reinterpret_cast<u64*>(arena_base_ + 0x12001000U) = 0x0000000050200000ULL;
    hw_mirrored_ = false;
    return true;
}

void MemoryArena::shutdown() noexcept {
    for (u32 i = 0; i < view_count_; ++i) {
        if (views_[i]) {
            UnmapViewOfFile(views_[i]);
            views_[i] = nullptr;
        }
    }
    view_count_ = 0;

    auto close_safe = [](void*& handle) {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(handle));
            handle = nullptr;
        }
    };

    close_safe(section_ram_);
    close_safe(section_spr_);
    close_safe(section_io_);
    close_safe(section_vu_);
    close_safe(section_gs_);

    unregister_mmio_callbacks();

    if (arena_base_) {
        VirtualFree(arena_base_, 0, MEM_RELEASE);
        arena_base_ = nullptr;
    }
    hw_mirrored_ = false;
}

} // namespace emotion
