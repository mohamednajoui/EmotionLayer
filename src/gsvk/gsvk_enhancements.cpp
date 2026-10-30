#include "gsvk_enhancements.hpp"
#include <fstream>
#include <cstring>

namespace emotion {

bool HdTextureManager::load_from_bmp(u64 key, const std::string& bmp_path) {
    std::ifstream f(bmp_path, std::ios::binary);
    if (!f) return false;

    #pragma pack(push, 1)
    struct BmpHeader {
        u16 magic;      // 0x4D42 ('BM')
        u32 file_size;
        u32 reserved;
        u32 data_offset;
        u32 info_size;
        s32 width;
        s32 height;
        u16 planes;
        u16 bpp;
        u32 compression;
        u32 image_size;
        s32 x_ppm;
        s32 y_ppm;
        u32 clr_used;
        u32 clr_important;
    };
    #pragma pack(pop)

    BmpHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (hdr.magic != 0x4D42) return false;

    const u32 w = static_cast<u32>(std::abs(hdr.width));
    const u32 h = static_cast<u32>(std::abs(hdr.height));
    if (w == 0 || h == 0 || (hdr.bpp != 24 && hdr.bpp != 32)) return false;

    f.seekg(hdr.data_offset, std::ios::beg);

    std::vector<u32> rgba(w * h, 0);
    const u32 row_pitch = (hdr.bpp == 24) ? ((w * 3 + 3) & ~3U) : (w * 4U);
    std::vector<u8> row_buf(row_pitch, 0);

    const bool bottom_up = (hdr.height > 0);

    for (u32 y = 0; y < h; ++y) {
        f.read(reinterpret_cast<char*>(row_buf.data()), row_pitch);
        const u32 dst_y = bottom_up ? (h - 1 - y) : y;

        for (u32 x = 0; x < w; ++x) {
            u8 b = 0, g = 0, r = 0, a = 0xFF;
            if (hdr.bpp == 24) {
                b = row_buf[x * 3 + 0];
                g = row_buf[x * 3 + 1];
                r = row_buf[x * 3 + 2];
            } else {
                b = row_buf[x * 4 + 0];
                g = row_buf[x * 4 + 1];
                r = row_buf[x * 4 + 2];
                a = row_buf[x * 4 + 3];
            }
            rgba[dst_y * w + x] = static_cast<u32>(r) | (static_cast<u32>(g) << 8) |
                                  (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
        }
    }

    register_texture(key, w, h, rgba.data());
    return true;
}

} // namespace emotion
