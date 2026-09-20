// Phase 6 — WASAPI loopback capture implementation. See WasapiCapture.h.

#include "WasapiCapture.h"

#include "../common/Log.h"

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <audioclient.h>
#include <windows.h>

#include <chrono>
#include <future>
#include <cmath>
#include <cstring>

namespace rp::audio {
namespace {
constexpr REFERENCE_TIME kBufferDuration100ns = 200'000;   // 20 ms
constexpr REFERENCE_TIME kPollInterval100ns   = 50'000;    // 5 ms
double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

WasapiCapture::~WasapiCapture() { stop(); }

bool WasapiCapture::initDevices(std::string* err) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) { if (err) *err = "CoCreateInstance(MMDeviceEnumerator) failed"; return false; }
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) { if (err) *err = "no default render device"; return false; }
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient_));
    if (FAILED(hr)) { if (err) *err = "device Activate(IAudioClient) failed"; return false; }

    hr = audioClient_->GetMixFormat(&mixFormat_);
    if (FAILED(hr) || !mixFormat_) { if (err) *err = "GetMixFormat failed"; return false; }

    srcRate_ = mixFormat_->nSamplesPerSec;
    srcChannels_ = mixFormat_->nChannels;
    srcIsFloat_ = (mixFormat_->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                      ? (reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat_)->SubFormat.Data1 == 3)
                      : (mixFormat_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);

    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  kBufferDuration100ns, 0, mixFormat_, nullptr);
    if (FAILED(hr)) { if (err) *err = "IAudioClient::Initialize(loopback) failed"; return false; }

    hr = audioClient_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&captureClient_));
    if (FAILED(hr)) { if (err) *err = "GetService(IAudioCaptureClient) failed"; return false; }

    hr = audioClient_->Start();
    if (FAILED(hr)) { if (err) *err = "IAudioClient::Start failed"; return false; }
    return true;
}

bool WasapiCapture::start(BlockCallback onBlock, std::string* err) {
    if (running_.exchange(true)) { running_.store(false); return false; }
    onBlock_ = std::move(onBlock);

    // Init runs on the capture thread (its own COM MTA); errors come back via promise.
    std::promise<std::string> initResult;
    std::future<std::string> initFuture = initResult.get_future();
    std::string* threadErr = err;    // pointer for the thread's init path (diagnostics)
    (void)threadErr;

    thread_ = std::thread([this, &initResult, onBlock = std::move(onBlock_)]() mutable {
        onBlock_ = std::move(onBlock);
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool coInit = SUCCEEDED(hr);
        std::string ierr;
        if (!initDevices(&ierr)) {
            initResult.set_value(std::move(ierr));
            running_.store(false);
        } else {
            initResult.set_value({});
        }
        captureThread();
        if (audioClient_) audioClient_->Stop();
        if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
        if (audioClient_) { audioClient_->Release(); audioClient_ = nullptr; }
        if (mixFormat_) { CoTaskMemFree(mixFormat_); mixFormat_ = nullptr; }
        if (device_) { device_->Release(); device_ = nullptr; }
        if (enumerator_) { enumerator_->Release(); enumerator_ = nullptr; }
        if (coInit) CoUninitialize();
    });

    initFuture.wait();
    const std::string result = initFuture.get();
    if (!result.empty()) {
        if (err) *err = result;
        if (thread_.joinable()) thread_.join();
        running_.store(false);
        return false;
    }
    RP_INFO() << "[audio-capture] loopback started: " << srcRate_ << "Hz " << srcChannels_ << "ch float=" << srcIsFloat_;
    return true;
}

void WasapiCapture::captureThread() {
    const int srcFmt = srcIsFloat_ ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_S16;
    while (running_.load()) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        const double t0 = nowMs();

        HRESULT hr = captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            Sleep(5);
            continue;
        }
        if (FAILED(hr)) {
            RP_WARN() << "[audio-capture] GetBuffer failed hr=0x" << std::hex << hr << std::dec;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (frames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            // Convert to 48 kHz stereo s16 via swresample.
            if (!swr_) {
                AVChannelLayout inLayout;
                av_channel_layout_default(&inLayout, static_cast<int>(srcChannels_));
                AVChannelLayout outLayout;
                av_channel_layout_default(&outLayout, 2);
                if (swr_alloc_set_opts2(&swr_, &outLayout, AV_SAMPLE_FMT_S16, 48000,
                                        &inLayout, static_cast<AVSampleFormat>(srcFmt), static_cast<int>(srcRate_),
                                        0, nullptr) < 0) {
                    swr_ = nullptr;
                }
            }
            if (swr_) {
                // Max output frames for this input (rate change + rounding).
                const int maxOut = static_cast<int>(static_cast<int64_t>(frames) * 48000 / srcRate_ + 16);
                convertBuf_.resize(static_cast<size_t>(maxOut) * 2);
                const uint8_t* in[1]{ data };
                uint8_t* out[1]{ reinterpret_cast<uint8_t*>(convertBuf_.data()) };
                const int outFrames = swr_convert(swr_, out, maxOut, in, static_cast<int>(frames));
                if (outFrames > 0 && onBlock_) {
                    const uint64_t tsUs = static_cast<uint64_t>(nowMs() * 1000.0);
                    onBlock_(convertBuf_.data(), static_cast<size_t>(outFrames), tsUs);
                    ++blocks_;
                    const double dt = nowMs() - t0;
                    avgBlockMs_ = avgBlockMs_ == 0.0 ? dt : (avgBlockMs_ * 0.95 + dt * 0.05);
                }
            }
        }
        captureClient_->ReleaseBuffer(frames);
        // Loopback capture has no events by default; poll quickly.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (swr_) { swr_free(&swr_); swr_ = nullptr; }
}

void WasapiCapture::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

} // namespace rp::audio
