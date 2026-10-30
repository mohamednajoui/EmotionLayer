#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../common/types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <windows.h>

namespace emotion {

enum class AspectRatio : u8 {
    Standard_4_3    = 0,
    Widescreen_16_9 = 1,
    Ultrawide_21_9  = 2
};

struct HdTexture {
    u32 width  = 0;
    u32 height = 0;
    std::vector<u32> pixels; // RGBA8

    u32 sample_bilinear(f32 u, f32 v) const noexcept {
        if (width == 0 || height == 0 || pixels.empty()) return 0;
        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);

        const f32 fx = u * static_cast<f32>(width - 1);
        const f32 fy = v * static_cast<f32>(height - 1);

        const u32 x0 = static_cast<u32>(std::floor(fx));
        const u32 y0 = static_cast<u32>(std::floor(fy));
        const u32 x1 = std::min(x0 + 1, width - 1);
        const u32 y1 = std::min(y0 + 1, height - 1);

        const f32 wx = fx - static_cast<f32>(x0);
        const f32 wy = fy - static_cast<f32>(y0);

        const u32 c00 = pixels[y0 * width + x0];
        const u32 c10 = pixels[y0 * width + x1];
        const u32 c01 = pixels[y1 * width + x0];
        const u32 c11 = pixels[y1 * width + x1];

        auto lerp_channel = [&](u32 shift) -> u32 {
            const f32 v00 = static_cast<f32>((c00 >> shift) & 0xFF);
            const f32 v10 = static_cast<f32>((c10 >> shift) & 0xFF);
            const f32 v01 = static_cast<f32>((c01 >> shift) & 0xFF);
            const f32 v11 = static_cast<f32>((c11 >> shift) & 0xFF);
            const f32 top = v00 * (1.0f - wx) + v10 * wx;
            const f32 bot = v01 * (1.0f - wx) + v11 * wx;
            const f32 val = top * (1.0f - wy) + bot * wy;
            return static_cast<u32>(std::clamp(val, 0.0f, 255.0f)) << shift;
        };

        return lerp_channel(0) | lerp_channel(8) | lerp_channel(16) | lerp_channel(24);
    }
};

// Gestor de Reemplazo de Texturas HD en Caliente (HD Texture Pack & Modding)
class HdTextureManager {
public:
    HdTextureManager() = default;

    void register_texture(u64 key, u32 width, u32 height, const u32* rgba_pixels) {
        HdTexture tex;
        tex.width = width;
        tex.height = height;
        tex.pixels.assign(rgba_pixels, rgba_pixels + (width * height));
        textures_[key] = std::move(tex);
    }

    bool has_texture(u64 key) const noexcept {
        return textures_.find(key) != textures_.end();
    }

    u32 sample(u64 key, f32 u, f32 v) const noexcept {
        auto it = textures_.find(key);
        if (it == textures_.end()) return 0;
        samples_count_++;
        return it->second.sample_bilinear(u, v);
    }

    // Carga una textura en alta definición directamente desde un archivo .BMP de Windows
    bool load_from_bmp(u64 key, const std::string& bmp_path);

    u32 loaded_count() const noexcept { return static_cast<u32>(textures_.size()); }
    u32 samples_count() const noexcept { return samples_count_; }

    void clear() {
        textures_.clear();
        samples_count_ = 0;
    }

private:
    std::unordered_map<u64, HdTexture> textures_;
    mutable u32 samples_count_ = 0;
};

// Limitador de Cuadros y Temporizador de Alta Precisión de Windows (Frame Rate Unlocker)
class HighPrecisionFrameLimiter {
public:
    HighPrecisionFrameLimiter() noexcept {
        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&last_frame_time_);
    }

    void set_target_fps(u32 fps) noexcept {
        target_fps_ = fps;
    }

    u32 target_fps() const noexcept { return target_fps_; }

    void begin_frame() noexcept {
        QueryPerformanceCounter(&frame_start_time_);
    }

    void end_frame() noexcept {
        if (target_fps_ > 0 && freq_.QuadPart > 0) {
            const LONGLONG target_ticks = freq_.QuadPart / target_fps_;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            LONGLONG elapsed = now.QuadPart - last_frame_time_.QuadPart;

            while (elapsed < target_ticks) {
                const LONGLONG remaining_ticks = target_ticks - elapsed;
                const double remaining_ms = (static_cast<double>(remaining_ticks) * 1000.0) / static_cast<double>(freq_.QuadPart);
                if (remaining_ms > 2.0) {
                    Sleep(1);
                } else {
                    YieldProcessor();
                }
                QueryPerformanceCounter(&now);
                elapsed = now.QuadPart - last_frame_time_.QuadPart;
            }
        }

        LARGE_INTEGER cur_time;
        QueryPerformanceCounter(&cur_time);
        if (freq_.QuadPart > 0 && cur_time.QuadPart > last_frame_time_.QuadPart) {
            frame_time_ms_ = static_cast<f32>(
                (static_cast<double>(cur_time.QuadPart - last_frame_time_.QuadPart) * 1000.0) / static_cast<double>(freq_.QuadPart)
            );
            if (frame_time_ms_ > 0.0f) {
                current_fps_ = 1000.0f / frame_time_ms_;
            }
        }
        last_frame_time_ = cur_time;
        total_frames_++;
    }

    f32 current_fps() const noexcept { return current_fps_; }
    f32 frame_time_ms() const noexcept { return frame_time_ms_; }
    u64 total_frames() const noexcept { return total_frames_; }

private:
    LARGE_INTEGER freq_{};
    LARGE_INTEGER last_frame_time_{};
    LARGE_INTEGER frame_start_time_{};
    u32 target_fps_    = 0; // 0 = Desbloqueado
    f32 current_fps_   = 60.0f;
    f32 frame_time_ms_ = 16.666f;
    u64 total_frames_  = 0;
};

// Configuración Global de Mejoras de Calidad de Vida (Fase 5)
struct GsvkEnhancementsConfig {
    u32         resolution_scale     = 1; // 1x=640x480, 2x=1280x960, 4x=2560x1920 (1440p/2K), 6x=4K, 8x=8K
    AspectRatio aspect_ratio         = AspectRatio::Standard_4_3;
    u32         target_fps           = 0; // 0 = Desbloqueado, 60, 120, 144, 240
    bool        hd_textures_enabled  = true;
    bool        widescreen_hack      = false;
};

} // namespace emotion
