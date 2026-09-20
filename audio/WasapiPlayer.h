#pragma once
// Phase 7 — client-side audio playback via WASAPI shared-mode render.
// Accepts interleaved s16 stereo 48 kHz, buffers ~120 ms, and feeds the
// default output device on a render thread. Underruns are reported for stats.

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rp::audio {

class WasapiPlayer {
public:
    WasapiPlayer() = default;
    ~WasapiPlayer();

    WasapiPlayer(const WasapiPlayer&) = delete;
    WasapiPlayer& operator=(const WasapiPlayer&) = delete;

    bool start(std::string* err = nullptr);
    void stop();

    // Queue decoded samples (interleaved s16 stereo). Non-blocking; drops the
    // oldest buffered audio beyond 400 ms to keep latency bounded.
    void push(const int16_t* samples, size_t frameCount);

    [[nodiscard]] double bufferedMs() const;
    [[nodiscard]] uint64_t underruns() const { return underruns_.load(); }
    [[nodiscard]] bool running() const { return running_.load(); }

private:
    void renderThread();
    bool initDevices(std::string* err);

    std::thread thread_;
    std::atomic<bool> running_{ false };

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioRenderClient* renderClient_ = nullptr;
    WAVEFORMATEX* waveFormat_ = nullptr;
    uint32_t bufferFrames_ = 0;         // total endpoint buffer (frames)
    bool isS16_ = false;                // negotiated s16 (else float32 mix format)

    mutable std::mutex queueMutex_;             // (mutable for bufferedMs())
    std::deque<int16_t> queue_;         // interleaved s16 stereo
    std::atomic<uint64_t> underruns_{ 0 };
    mutable std::chrono::steady_clock::time_point lastFeed_;
};

} // namespace rp::audio
