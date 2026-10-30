#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../../common/types.hpp"
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

namespace emotion {

// Streamer de Audio en Tiempo Real con Windows WASAPI (48 kHz 16-bit Stereo)
class WasapiAudioStreamer {
public:
    static constexpr u32 kSampleRate    = 48000;
    static constexpr u32 kChannels      = 2;
    static constexpr u32 kRingBufFrames = 48000; // 1 segundo de búfer circular

    WasapiAudioStreamer() noexcept = default;
    ~WasapiAudioStreamer() { stop(); }

    // Inicia el hilo de reproducción de audio continuo
    bool start();

    // Detiene la reproducción
    void stop();

    // Alimenta muestras estéreo intercaladas de 16 bits (48 kHz) al búfer de reproducción
    void push_samples(const s16* interleaved_stereo, u32 num_frames);

    bool is_running() const noexcept { return is_running_.load(); }
    u64  total_frames_played() const noexcept { return frames_played_.load(); }

private:
    void audio_thread_func();

    std::atomic<bool> is_running_{false};
    std::thread       worker_thread_;

    std::mutex        ring_mutex_;
    std::vector<s16>  ring_buffer_;
    u32               read_pos_  = 0;
    u32               write_pos_ = 0;
    u32               buffered_frames_ = 0;

    std::atomic<u64>  frames_played_{0};
};

} // namespace emotion
