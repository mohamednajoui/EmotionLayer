#include "game_profile.hpp"
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace emotion {

GameProfile GameProfileRegistry::s_active_profile{};

namespace {

// Implementación estándar y portable de SHA-256 (FIPS 180-2) sin dependencias externas
inline u32 rotr(u32 x, u32 n) noexcept {
    return (x >> n) | (x << (32 - n));
}

inline u32 ch(u32 x, u32 y, u32 z) noexcept {
    return (x & y) ^ (~x & z);
}

inline u32 maj(u32 x, u32 y, u32 z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline u32 ep0(u32 x) noexcept {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline u32 ep1(u32 x) noexcept {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline u32 sig0(u32 x) noexcept {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline u32 sig1(u32 x) noexcept {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

const u32 kSha256K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

// Hash exacto conocido de Marvel vs. Capcom 2 (Europe) SLES-511.74
const u8 kMvC2Sha256[32] = {
    0x23, 0xCF, 0x3C, 0xD9, 0x14, 0xCB, 0x84, 0x39,
    0x81, 0x01, 0x30, 0x47, 0x95, 0xBA, 0x63, 0x6C,
    0x9F, 0xA2, 0x5F, 0x9A, 0xEC, 0xBB, 0xE9, 0xE5,
    0xAF, 0xC1, 0xB8, 0x13, 0xA3, 0xC7, 0xA0, 0x7E
};

} // namespace

std::array<u8, 32> GameProfileRegistry::compute_sha256(const u8* data, std::size_t size) {
    u32 state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    u64 bitlen = static_cast<u64>(size) * 8ULL;
    u8 buffer[64];
    std::size_t offset = 0;

    auto process_block = [&](const u8* chunk) {
        u32 m[64];
        for (int i = 0; i < 16; ++i) {
            m[i] = (static_cast<u32>(chunk[i * 4 + 0]) << 24) |
                   (static_cast<u32>(chunk[i * 4 + 1]) << 16) |
                   (static_cast<u32>(chunk[i * 4 + 2]) << 8)  |
                   (static_cast<u32>(chunk[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            m[i] = sig1(m[i - 2]) + m[i - 7] + sig0(m[i - 15]) + m[i - 16];
        }

        u32 a = state[0], b = state[1], c = state[2], d = state[3];
        u32 e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; ++i) {
            u32 t1 = h + ep1(e) + ch(e, f, g) + kSha256K[i] + m[i];
            u32 t2 = ep0(a) + maj(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    };

    while (offset + 64 <= size) {
        process_block(data + offset);
        offset += 64;
    }

    const std::size_t rem = size - offset;
    std::memcpy(buffer, data + offset, rem);
    buffer[rem] = 0x80;
    if (rem + 1 <= 56) {
        std::memset(buffer + rem + 1, 0, 56 - (rem + 1));
    } else {
        std::memset(buffer + rem + 1, 0, 64 - (rem + 1));
        process_block(buffer);
        std::memset(buffer, 0, 56);
    }

    for (int i = 0; i < 8; ++i) {
        buffer[63 - i] = static_cast<u8>((bitlen >> (i * 8)) & 0xFFU);
    }
    process_block(buffer);

    std::array<u8, 32> out_hash;
    for (int i = 0; i < 8; ++i) {
        out_hash[i * 4 + 0] = static_cast<u8>((state[i] >> 24) & 0xFFU);
        out_hash[i * 4 + 1] = static_cast<u8>((state[i] >> 16) & 0xFFU);
        out_hash[i * 4 + 2] = static_cast<u8>((state[i] >> 8)  & 0xFFU);
        out_hash[i * 4 + 3] = static_cast<u8>(state[i] & 0xFFU);
    }
    return out_hash;
}

std::string GameProfileRegistry::sha256_to_hex(const std::array<u8, 32>& hash) {
    std::ostringstream ss;
    for (u8 b : hash) {
        ss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return ss.str();
}

const GameProfile& GameProfileRegistry::detect_and_set_profile(const u8* elf_data, std::size_t elf_size) {
    if (!elf_data || elf_size == 0) {
        reset_to_generic();
        return s_active_profile;
    }

    const auto hash = compute_sha256(elf_data, elf_size);
    const std::string hex_str = sha256_to_hex(hash);

    // Activación única y exclusivamente por coincidencia exacta de los 32 bytes de SHA-256
    if (std::memcmp(hash.data(), kMvC2Sha256, 32) == 0) {
        s_active_profile.id                          = ProfileId::MarvelVsCapcom2_SLES_51174;
        s_active_profile.name                        = "Marvel vs. Capcom 2: New Age of Heroes (Europe)";
        s_active_profile.serial                      = "SLES-511.74";
        s_active_profile.elf_sha256                  = hash;
        s_active_profile.requires_legacy_cdvd_pokes  = true;
        s_active_profile.requires_smc_pc_traps       = true;
        s_active_profile.requires_deci2_fallback     = true;
        s_active_profile.requires_findaddress_rodata = true;

        std::printf("[GameProfile] Detectado binario conocido por SHA-256 (%s): %s [%s]\n",
            hex_str.c_str(), s_active_profile.name.c_str(), s_active_profile.serial.c_str());
        std::printf("[GameProfile] Advertencia: Quirks de compatibilidad especifica activos para este titulo.\n");
    } else {
        reset_to_generic();
        s_active_profile.elf_sha256 = hash;
        std::printf("[GameProfile] Binario no catalogado en base de datos de perfiles (SHA-256: %s)\n", hex_str.c_str());
        std::printf("[GameProfile] Perfil GENERIC activo: Todos los hacks de juegos especificos DESACTIVADOS.\n");
    }
    std::fflush(stdout);
    return s_active_profile;
}

const GameProfile& GameProfileRegistry::active_profile() noexcept {
    return s_active_profile;
}

void GameProfileRegistry::set_active_profile(const GameProfile& profile) noexcept {
    s_active_profile = profile;
}

void GameProfileRegistry::reset_to_generic() noexcept {
    s_active_profile.id                          = ProfileId::Generic;
    s_active_profile.name                        = "Generic PS2 Title";
    s_active_profile.serial                      = "";
    s_active_profile.elf_sha256.fill(0);
    s_active_profile.requires_legacy_cdvd_pokes  = false;
    s_active_profile.requires_smc_pc_traps       = false;
    s_active_profile.requires_deci2_fallback     = false;
    s_active_profile.requires_findaddress_rodata = false;
}

} // namespace emotion
