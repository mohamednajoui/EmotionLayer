#include "win_spu2_audio.hpp"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace emotion {

Spu2AudioEngine::Spu2AudioEngine() noexcept {
    sound_ram_.assign(kSpuRamSize, 0);
    reset();
}

void Spu2AudioEngine::reset() noexcept {
    for (auto& v : voices_) {
        v = Spu2Voice{};
    }
}

void Spu2AudioEngine::upload_sample_data(u32 sram_addr, const u8* data, u32 byte_count) {
    if (!data || sram_addr >= kSpuRamSize) return;
    const u32 copy_len = std::min(byte_count, kSpuRamSize - sram_addr);
    std::memcpy(sound_ram_.data() + sram_addr, data, copy_len);
}

void Spu2AudioEngine::voice_key_on(u32 voice_idx, u32 sram_addr, u16 pitch, s16 vol_l, s16 vol_r) {
    if (voice_idx >= kVoiceCount) return;
    auto& v = voices_[voice_idx];
    v.active       = true;
    v.sample_addr  = sram_addr;
    v.pitch        = pitch ? pitch : 0x1000;
    v.vol_l        = vol_l;
    v.vol_r        = vol_r;
    v.s1           = 0;
    v.s2           = 0;
    v.block_offset = 0;
    v.sample_idx   = 0;
    v.loop_flag    = false;
}

void Spu2AudioEngine::voice_key_off(u32 voice_idx) {
    if (voice_idx < kVoiceCount) {
        voices_[voice_idx].active = false;
    }
}

u32 Spu2AudioEngine::active_voices_count() const noexcept {
    u32 count = 0;
    for (const auto& v : voices_) {
        if (v.active) count++;
    }
    return count;
}

void Spu2AudioEngine::decode_adpcm_block(const u8* block_16b, s16* out_pcm_28, s32& inout_s1, s32& inout_s2, bool& out_loop_end) {
    // Coeficientes estándar de predicción de filtro ADPCM de PlayStation
    static constexpr double kF0[5] = { 0.0, 60.0 / 64.0,  115.0 / 64.0,  98.0 / 64.0,  122.0 / 64.0 };
    static constexpr double kF1[5] = { 0.0,  0.0,         -52.0 / 64.0, -55.0 / 64.0,  -60.0 / 64.0 };

    const u8 shift_factor = block_16b[0] & 0x0FU;
    const u8 filter_index = std::min(static_cast<u8>((block_16b[0] >> 4) & 0x0FU), static_cast<u8>(4));
    const u8 flags        = block_16b[1];
    out_loop_end          = (flags & 0x01U) != 0;

    const double f0 = kF0[filter_index];
    const double f1 = kF1[filter_index];

    double s1 = static_cast<double>(inout_s1);
    double s2 = static_cast<double>(inout_s2);

    u32 out_idx = 0;
    for (int b = 2; b < 16; ++b) {
        const u8 byte = block_16b[b];
        // Cada byte contiene dos nibbles de 4 bits con signo (low nibble primero)
        s32 n0 = static_cast<s32>(byte & 0x0FU);
        if (n0 >= 8) n0 -= 16;
        s32 n1 = static_cast<s32>((byte >> 4) & 0x0FU);
        if (n1 >= 8) n1 -= 16;

        auto decode_sample = [&](s32 nib) -> s16 {
            const double raw = static_cast<double>(nib << (12 - std::min(shift_factor, static_cast<u8>(12))));
            double samp = raw + s1 * f0 + s2 * f1;
            s2 = s1;
            s1 = samp;
            s32 isamp = static_cast<s32>(std::round(samp));
            return static_cast<s16>(std::clamp(isamp, -32768, 32767));
        };

        out_pcm_28[out_idx++] = decode_sample(n0);
        out_pcm_28[out_idx++] = decode_sample(n1);
    }

    inout_s1 = static_cast<s32>(s1);
    inout_s2 = static_cast<s32>(s2);
}

void Spu2AudioEngine::render_stereo_samples(s16* out_interleaved_stereo, u32 num_frames) {
    if (!out_interleaved_stereo || num_frames == 0) return;

    std::vector<s32> accum_l(num_frames, 0);
    std::vector<s32> accum_r(num_frames, 0);

    for (auto& v : voices_) {
        if (!v.active) continue;

        u32 cur_sram_block = (v.sample_addr + v.block_offset) & (kSpuRamSize - 16U);
        s16 block_samples[28];
        bool loop_end = false;

        decode_adpcm_block(sound_ram_.data() + cur_sram_block, block_samples, v.s1, v.s2, loop_end);

        for (u32 f = 0; f < num_frames; ++f) {
            s16 smp = block_samples[v.sample_idx++];
            if (v.sample_idx >= 28) {
                v.sample_idx = 0;
                v.block_offset += 16;
                cur_sram_block = (v.sample_addr + v.block_offset) & (kSpuRamSize - 16U);
                decode_adpcm_block(sound_ram_.data() + cur_sram_block, block_samples, v.s1, v.s2, loop_end);
                if (loop_end) {
                    v.block_offset = 0; // Bucle o terminación
                }
            }

            accum_l[f] += (static_cast<s32>(smp) * static_cast<s32>(v.vol_l)) >> 14;
            accum_r[f] += (static_cast<s32>(smp) * static_cast<s32>(v.vol_r)) >> 14;
        }
    }

    for (u32 f = 0; f < num_frames; ++f) {
        out_interleaved_stereo[f * 2 + 0] = static_cast<s16>(std::clamp(accum_l[f], -32768, 32767));
        out_interleaved_stereo[f * 2 + 1] = static_cast<s16>(std::clamp(accum_r[f], -32768, 32767));
    }

    if (wasapi_.is_running()) {
        wasapi_.push_samples(out_interleaved_stereo, num_frames);
    }
}

bool Spu2AudioEngine::export_to_wav(const std::string& wav_filepath, u32 duration_frames) {
    std::vector<s16> pcm(duration_frames * 2, 0);
    render_stereo_samples(pcm.data(), duration_frames);

    #pragma pack(push, 1)
    struct WavHeader {
        char     riff[4]        = {'R', 'I', 'F', 'F'};
        u32      riff_size      = 0;
        char     wave[4]        = {'W', 'A', 'V', 'E'};
        char     fmt[4]         = {'f', 'm', 't', ' '};
        u32      fmt_size       = 16;
        u16      audio_format   = 1; // PCM
        u16      num_channels   = 2; // Stereo
        u32      sample_rate    = kSampleRate;
        u32      byte_rate      = kSampleRate * 2 * sizeof(s16);
        u16      block_align    = 2 * sizeof(s16);
        u16      bits_per_sample= 16;
        char     data[4]        = {'d', 'a', 't', 'a'};
        u32      data_size      = 0;
    };
    #pragma pack(pop)

    const u32 data_bytes = duration_frames * 2 * sizeof(s16);
    WavHeader hdr{};
    hdr.riff_size = 36U + data_bytes;
    hdr.data_size = data_bytes;

    std::ofstream out(wav_filepath, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(WavHeader));
    out.write(reinterpret_cast<const char*>(pcm.data()), data_bytes);
    return true;
}

} // namespace emotion
