// Phase 7 — WASAPI playback implementation. See WasapiPlayer.h.

#include "WasapiPlayer.h"

#include "../common/Log.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <future>

namespace rp::audio {
namespace {
constexpr REFERENCE_TIME kBufferDuration100ns = 1'200'000;   // 120 ms endpoint buffer
constexpr size_t kMaxQueueFrames = 48000 / 1000 * 400;       // 400 ms hard cap
} // namespace

WasapiPlayer::~WasapiPlayer() { stop(); }

bool WasapiPlayer::initDevices(std::string* err) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) { if (err) *err = "CoCreateInstance(MMDeviceEnumerator) failed"; return false; }
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) { if (err) *err = "no default render device"; return false; }
    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient_));
    if (FAILED(hr)) { if (err) *err = "device Activate(IAudioClient) failed"; return false; }

    // Prefer plain s16 stereo 48k (shared mode converts as needed).
    WAVEFORMATEX desired{};
    desired.wFormatTag = WAVE_FORMAT_PCM;
    desired.nChannels = 2;
    desired.nSamplesPerSec = 48000;
    desired.wBitsPerSample = 16;
    desired.nBlockAlign = 4;
    desired.nAvgBytesPerSec = 48000 * 4;
    desired.cbSize = 0;

    WAVEFORMATEX* closest = nullptr;
    hr = audioClient_->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &desired, &closest);
    if (hr == S_OK) {
        waveFormat_ = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(desired)));
        if (waveFormat_) *waveFormat_ = desired;
        isS16_ = true;
    } else if (hr == S_FALSE && closest) {
        // Use the closest supported (engine will convert); we adapt in the render loop.
        waveFormat_ = closest;
        isS16_ = (waveFormat_->wFormatTag == WAVE_FORMAT_PCM && waveFormat_->wBitsPerSample == 16);
    } else {
        // Fall back to the device mix format.
        hr = audioClient_->GetMixFormat(&waveFormat_);
        isS16_ = false;
    }
    if (!waveFormat_) { if (err) *err = "no usable render format"; return false; }

    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration100ns, 0, waveFormat_, nullptr);
    if (FAILED(hr)) {
        if (err) *err = "IAudioClient::Initialize(render) failed";
        return false;
    }
    bufferFrames_ = 0;
    audioClient_->GetBufferSize(&bufferFrames_);
    hr = audioClient_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient_));
    if (FAILED(hr)) { if (err) *err = "GetService(IAudioRenderClient) failed"; return false; }

    // Pre-fill with silence and start.
    if (BYTE* p = nullptr; SUCCEEDED(renderClient_->GetBuffer(bufferFrames_, &p))) {
        std::memset(p, 0, static_cast<size_t>(bufferFrames_) * waveFormat_->nBlockAlign);
        renderClient_->ReleaseBuffer(bufferFrames_, 0);
    }
    hr = audioClient_->Start();
    if (FAILED(hr)) { if (err) *err = "IAudioClient::Start failed"; return false; }
    return true;
}

bool WasapiPlayer::start(std::string* err) {
    if (running_.exchange(true)) { running_.store(false); return false; }

    std::promise<std::string> initResult;
    std::future<std::string> initFuture = initResult.get_future();
    thread_ = std::thread([this, &initResult] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool coInit = SUCCEEDED(hr);
        std::string ierr;
        if (!initDevices(&ierr)) initResult.set_value(std::move(ierr));
        else initResult.set_value({});
        renderThread();
        if (audioClient_) audioClient_->Stop();
        if (renderClient_) { renderClient_->Release(); renderClient_ = nullptr; }
        if (audioClient_) { audioClient_->Release(); audioClient_ = nullptr; }
        if (waveFormat_) { CoTaskMemFree(waveFormat_); waveFormat_ = nullptr; }
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
    RP_INFO() << "[audio-player] started: " << waveFormat_->nSamplesPerSec << "Hz "
              << waveFormat_->nChannels << "ch s16=" << isS16_
              << " buffer=" << bufferFrames_ << " frames";
    return true;
}

void WasapiPlayer::renderThread() {
    while (running_.load()) {
        UINT32 padding = 0;
        if (FAILED(audioClient_->GetCurrentPadding(&padding))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const UINT32 framesToWrite = bufferFrames_ - padding;
        if (framesToWrite == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        BYTE* p = nullptr;
        if (FAILED(renderClient_->GetBuffer(framesToWrite, &p))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const size_t nCh = waveFormat_->nChannels;
        const size_t need = static_cast<size_t>(framesToWrite) * nCh;   // s16 samples

        std::lock_guard<std::mutex> lk(queueMutex_);
        const size_t avail = queue_.size();
        const size_t take = std::min(need, avail);
        if (take < need) ++underruns_;

        if (isS16_) {
            for (size_t i = 0; i < take; ++i) {
                reinterpret_cast<int16_t*>(p)[i] = queue_[i];
            }
            for (size_t i = take; i < need; ++i) {
                reinterpret_cast<int16_t*>(p)[i] = 0;
            }
        } else {
            // Convert s16 -> float32.
            auto* f = reinterpret_cast<float*>(p);
            for (size_t i = 0; i < need; ++i) {
                f[i] = i < take ? static_cast<float>(queue_[i]) / 32768.0f : 0.0f;
            }
        }
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(take));
        renderClient_->ReleaseBuffer(framesToWrite, 0);

        // Pace: refill roughly every 10 ms of buffer space.
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

void WasapiPlayer::push(const int16_t* samples, size_t frameCount) {
    std::lock_guard<std::mutex> lk(queueMutex_);
    queue_.insert(queue_.end(), samples, samples + frameCount * 2);
    // Hard latency cap: drop oldest beyond 400 ms.
    if (queue_.size() > kMaxQueueFrames * 2) {
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(queue_.size() - kMaxQueueFrames * 2));
    }
}

double WasapiPlayer::bufferedMs() const {
    std::lock_guard<std::mutex> lk(queueMutex_);
    return static_cast<double>(queue_.size() / 2) / 48.0;   // frames / 48kHz * 1000
}

void WasapiPlayer::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(queueMutex_);
    queue_.clear();
}

} // namespace rp::audio
