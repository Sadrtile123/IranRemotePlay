#pragma once
// Phase 6 — WASAPI loopback capture of whatever the host is playing.
//
// Captures the default render device's mix (all game audio) in shared mode,
// converts to 48 kHz stereo s16 via FFmpeg swresample, and delivers blocks on
// a capture thread. No kernel components, no driver installs.

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct SwrContext;

namespace rp::audio {

class WasapiCapture {
public:
    // Called per captured block (interleaved s16 stereo @48 kHz).
    using BlockCallback = std::function<void(const int16_t* samples, size_t frameCount, uint64_t timestampUs)>;

    WasapiCapture() = default;
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    bool start(BlockCallback onBlock, std::string* err = nullptr);
    void stop();

    [[nodiscard]] double averageBlockMs() const { return avgBlockMs_; }
    [[nodiscard]] uint64_t blocksCaptured() const { return blocks_; }
    [[nodiscard]] uint32_t sourceSampleRate() const { return srcRate_; }
    [[nodiscard]] uint32_t sourceChannels() const { return srcChannels_; }
    [[nodiscard]] bool running() const { return running_.load(); }

private:
    void captureThread();
    bool initDevices(std::string* err);

    BlockCallback onBlock_;
    std::thread thread_;
    std::atomic<bool> running_{ false };

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    WAVEFORMATEX* mixFormat_ = nullptr;
    uint32_t srcRate_ = 0;
    uint32_t srcChannels_ = 0;
    bool srcIsFloat_ = false;

    SwrContext* swr_ = nullptr;            // -> s16 48k stereo
    std::vector<int16_t> convertBuf_;

    uint64_t blocks_ = 0;
    double avgBlockMs_ = 0.0;
};

} // namespace rp::audio
