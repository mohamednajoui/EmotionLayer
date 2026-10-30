#pragma once

#include "../../common/types.hpp"
#include "wasapi_streamer.hpp"
#include <vector>
#include <string>

namespace emotion {

struct Spu2Voice {
    bool active       = false;
    u32  sample_addr  = 0;
    u16  pitch        = 0x1000; // 0x1000 = 48 kHz 1.0x
    s16  vol_l        = 0x3FFF;
    s16  vol_r        = 0x3FFF;
    s32  s1           = 0; // Historial para filtro ADPCM
    s32  s2           = 0;
    u32  block_offset = 0;
    u32  sample_idx   = 0;
    bool loop_flag    = false;
};

// Sintetizador y Mezclador de Audio SPU2 de PlayStation 2
// Decodifica bloques ADPCM de 16 bytes a 48 kHz stereo y exporta a WAV / WASAPI
class Spu2AudioEngine {
public:
    static constexpr u32 kSpuRamSize      = 2 * 1024 * 1024; // 2 MB Sound RAM
    static constexpr u32 kVoiceCount      = 48;              // 24 voces en Core 0 + 24 voces en Core 1
    static constexpr u32 kSampleRate      = 48000;           // 48 kHz estándar de PS2 SPU2

    Spu2AudioEngine() noexcept;
    ~Spu2AudioEngine() noexcept = default;

    void reset() noexcept;

    // Sube datos crudos (ej. muestras ADPCM) a la Sound RAM
    void upload_sample_data(u32 sram_addr, const u8* data, u32 byte_count);

    // Configura y activa una voz de reproducción
    void voice_key_on(u32 voice_idx, u32 sram_addr, u16 pitch = 0x1000, s16 vol_l = 0x3FFF, s16 vol_r = 0x3FFF);
    void voice_key_off(u32 voice_idx);

    // Decodifica un bloque individual de 16 bytes ADPCM a 28 muestras PCM de 16 bits
    static void decode_adpcm_block(const u8* block_16b, s16* out_pcm_28, s32& inout_s1, s32& inout_s2, bool& out_loop_end);

    // Sintetiza N muestras estéreo (48 kHz 16-bit) mezclando todas las voces activas
    void render_stereo_samples(s16* out_interleaved_stereo, u32 num_frames);

    // Exporta el audio renderizado a un archivo WAV nativo de Windows (PCM 48kHz 16-bit Stereo)
    bool export_to_wav(const std::string& wav_filepath, u32 duration_frames);

    // Control de Streaming de Audio en Vivo con Windows WASAPI (48 kHz Stereo)
    bool start_live_audio() { return wasapi_.start(); }
    void stop_live_audio()  { wasapi_.stop(); }
    WasapiAudioStreamer& wasapi() noexcept { return wasapi_; }
    const WasapiAudioStreamer& wasapi() const noexcept { return wasapi_; }

    u32 active_voices_count() const noexcept;

private:
    std::vector<u8> sound_ram_;
    Spu2Voice voices_[kVoiceCount];
    WasapiAudioStreamer wasapi_;
};

} // namespace emotion
