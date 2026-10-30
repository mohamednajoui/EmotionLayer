#include "wasapi_streamer.hpp"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "ole32.lib")

namespace emotion {

bool WasapiAudioStreamer::start() {
    if (is_running_.load()) return true;

    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        ring_buffer_.assign(kRingBufFrames * kChannels, 0);
        read_pos_ = 0;
        write_pos_ = 0;
        buffered_frames_ = 0;
    }

    is_running_.store(true);
    worker_thread_ = std::thread(&WasapiAudioStreamer::audio_thread_func, this);
    return true;
}

void WasapiAudioStreamer::stop() {
    if (!is_running_.load()) return;
    is_running_.store(false);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void WasapiAudioStreamer::push_samples(const s16* interleaved_stereo, u32 num_frames) {
    if (!interleaved_stereo || num_frames == 0) return;

    std::lock_guard<std::mutex> lock(ring_mutex_);
    for (u32 f = 0; f < num_frames; ++f) {
        if (buffered_frames_ >= kRingBufFrames) {
            // Buffer lleno: avanzar read_pos_ para descartar la muestra más antigua
            read_pos_ = (read_pos_ + 1) % kRingBufFrames;
            buffered_frames_--;
        }

        ring_buffer_[write_pos_ * kChannels + 0] = interleaved_stereo[f * kChannels + 0];
        ring_buffer_[write_pos_ * kChannels + 1] = interleaved_stereo[f * kChannels + 1];
        write_pos_ = (write_pos_ + 1) % kRingBufFrames;
        buffered_frames_++;
    }
}

void WasapiAudioStreamer::audio_thread_func() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_initialized = SUCCEEDED(hr);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioRenderClient* pRenderClient = nullptr;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&pEnumerator)
    );

    if (SUCCEEDED(hr) && pEnumerator) {
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    }

    if (SUCCEEDED(hr) && pDevice) {
        hr = pDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(&pAudioClient)
        );
    }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = kChannels;
    wfx.nSamplesPerSec  = kSampleRate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = kChannels * sizeof(s16);
    wfx.nAvgBytesPerSec = kSampleRate * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    REFERENCE_TIME hnsDuration = 500000; // 50 ms de búfer de baja latencia
    UINT32 bufferFrameCount = 0;

    if (SUCCEEDED(hr) && pAudioClient) {
        hr = pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            hnsDuration,
            0,
            &wfx,
            nullptr
        );
    }

    if (SUCCEEDED(hr) && pAudioClient) {
        hr = pAudioClient->GetBufferSize(&bufferFrameCount);
    }

    if (SUCCEEDED(hr) && pAudioClient) {
        hr = pAudioClient->GetService(
            __uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(&pRenderClient)
        );
    }

    if (SUCCEEDED(hr) && pAudioClient) {
        pAudioClient->Start();
    }

    // Bucle continuo de streaming de audio en tiempo real
    while (is_running_.load()) {
        if (pAudioClient && pRenderClient && bufferFrameCount > 0) {
            UINT32 numPadding = 0;
            pAudioClient->GetCurrentPadding(&numPadding);

            const UINT32 availableFrames = (bufferFrameCount > numPadding) ? (bufferFrameCount - numPadding) : 0;
            if (availableFrames > 0) {
                u32 framesToWrite = 0;
                std::vector<s16> tempBuf(availableFrames * kChannels, 0);

                {
                    std::lock_guard<std::mutex> lock(ring_mutex_);
                    framesToWrite = (std::min)(availableFrames, buffered_frames_);
                    for (u32 f = 0; f < framesToWrite; ++f) {
                        tempBuf[f * kChannels + 0] = ring_buffer_[read_pos_ * kChannels + 0];
                        tempBuf[f * kChannels + 1] = ring_buffer_[read_pos_ * kChannels + 1];
                        read_pos_ = (read_pos_ + 1) % kRingBufFrames;
                    }
                    buffered_frames_ -= framesToWrite;
                }

                BYTE* pData = nullptr;
                if (pRenderClient->GetBuffer(availableFrames, &pData) == S_OK) {
                    std::memcpy(pData, tempBuf.data(), availableFrames * kChannels * sizeof(s16));
                    pRenderClient->ReleaseBuffer(availableFrames, 0);
                    frames_played_ += framesToWrite;
                }
            }
        }
        Sleep(5);
    }

    if (pAudioClient) {
        pAudioClient->Stop();
        pAudioClient->Release();
    }
    if (pRenderClient) pRenderClient->Release();
    if (pDevice) pDevice->Release();
    if (pEnumerator) pEnumerator->Release();
    if (co_initialized) CoUninitialize();
}

} // namespace emotion
