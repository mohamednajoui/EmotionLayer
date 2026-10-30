#pragma once

#include "types.hpp"
#include <array>
#include <string>
#include <string_view>
#include <cstdint>

namespace emotion {

enum class ProfileId : u32 {
    Generic = 0,
    MarvelVsCapcom2_SLES_51174 = 1,
};

struct GameProfile {
    ProfileId id = ProfileId::Generic;
    std::string name = "Generic PS2 Title";
    std::string serial = "";
    std::array<u8, 32> elf_sha256 = {0};

    // Flags de quirks específicos (todos false por defecto en Generic)
    bool requires_legacy_cdvd_pokes = false;
    bool requires_smc_pc_traps       = false;
    bool requires_deci2_fallback     = false;
    bool requires_findaddress_rodata = false;
};

class GameProfileRegistry {
public:
    // Calcula SHA-256 de un bloque de memoria
    static std::array<u8, 32> compute_sha256(const u8* data, std::size_t size);
    static std::string sha256_to_hex(const std::array<u8, 32>& hash);

    // Identifica el perfil según el hash exacto del ELF
    static const GameProfile& detect_and_set_profile(const u8* elf_data, std::size_t elf_size);

    // Perfil activo
    static const GameProfile& active_profile() noexcept;
    static void set_active_profile(const GameProfile& profile) noexcept;
    static void reset_to_generic() noexcept;

private:
    static GameProfile s_active_profile;
};

} // namespace emotion
