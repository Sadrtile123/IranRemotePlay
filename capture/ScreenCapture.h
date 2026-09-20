#pragma once
// Phase 2 — desktop/window capture via DXGI Output Duplication (Windows 8.1+, best on 10/11).
//
// Pipeline: GPU desktop texture -> ID3D11 staging texture -> Map -> BGRA buffer.
// One CPU copy is performed in v1 (documented limitation in docs/PERFORMANCE.md;
// GPU-direct encoding path is a later optimization). Window capture is implemented
// as monitor duplication + per-frame crop to the target window's client rect, which
// avoids WinRT (Windows.Graphics.Capture) dependencies and works with any renderer.

#include "ComPtr.h"

// Exclude winsock.h from windows.h: standalone Asio must include winsock2.h
// itself, and duplicated/old winsock headers break the build.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <string>
#include <vector>

struct HWND__;
typedef struct HWND__* HWND;

namespace rp {

// A single captured frame. `data` points into a staging texture that stays valid
// until the next call to capture(); consumers must copy it out before that.
struct CaptureFrame {
    const uint8_t* data = nullptr;
    int      width = 0;            // cropped width (pixels)
    int      height = 0;           // cropped height (pixels)
    int      stride = 0;           // row pitch in BYTES (not pixels)
    uint64_t timestampUs = 0;      // steady_clock microseconds
    uint32_t frameId = 0;          // consecutive frame counter
};

struct OutputInfo {
    int         index = 0;
    std::wstring name;             // DXGI output device name
    int         width = 0;         // desktop pixels
    int         height = 0;
    bool        attachedToDesktop = true;
};

struct WindowInfo {
    void*       hwnd = nullptr;    // HWND
    std::wstring title;
    std::wstring processExe;
    int         x = 0, y = 0, w = 0, h = 0;   // screen coords of client area
};

// Enumerates outputs on the primary adapter path (adapter 0 first, then others).
std::vector<OutputInfo> enumerateOutputs(std::string* err = nullptr);

// Lists visible, titled top-level windows suitable for capture targets.
std::vector<WindowInfo> listCaptureWindows();

class DisplayCapture {
public:
    DisplayCapture() = default;
    ~DisplayCapture();

    DisplayCapture(const DisplayCapture&) = delete;
    DisplayCapture& operator=(const DisplayCapture&) = delete;

    // Full-monitor capture mode. outputIndex refers to the global enumeration
    // order used by enumerateOutputs().
    bool init(int outputIndex, std::string* err = nullptr);

    // Grabs the next desktop frame. Returns false with timedOut=true when no new
    // frame arrived within timeoutMs (caller re-polls). Returns false with an
    // error string on fatal failure (re-init or stop streaming).
    bool capture(CaptureFrame& out, int timeoutMs, bool* timedOut = nullptr, std::string* err = nullptr);

    // Window-crop mode: recompute the crop rectangle from the window's current
    // client rect (call every frame — windows move). Returns false when the
    // window vanished; caller decides to fall back or stop.
    bool updateWindowCrop(void* hwnd);

    // Precomputed explicit crop (monitor-relative pixels).
    void setCropRect(int x, int y, int w, int h);
    void clearCrop();

    int width()  const { return outWidth_; }   // cropped output size
    int height() const { return outHeight_; }
    int monitorWidth()  const { return monWidth_; }
    int monitorHeight() const { return monHeight_; }
    uint32_t framesCaptured() const { return frameCounter_; }
    uint32_t accessLostCount() const { return accessLostCount_; }

    // Statistics for the diagnostics panel.
    double lastCaptureMs() const { return lastCaptureMs_; }
    double averageCaptureMs() const { return avgCaptureMs_; }

private:
    bool findOutput(IDXGIFactory1* factory, int outputIndex, std::string* err);
    bool duplicateOutput(std::string* err);
    void releaseAll();

    rp::ComPtr<ID3D11Device>        device_;
    rp::ComPtr<ID3D11DeviceContext> context_;
    rp::ComPtr<IDXGIOutput>         output_;     // the duplicated output
    rp::ComPtr<IDXGIOutputDuplication> duplication_;
    rp::ComPtr<ID3D11Texture2D>     staging_;
    D3D11_MAPPED_SUBRESOURCE        mapped_ = {};
    bool                            mappedValid_ = false;

    int            outputIndex_ = 0;
    int            adapterIndex_ = 0;
    int            monWidth_ = 0, monHeight_ = 0;
    int            monLeft_ = 0, monTop_ = 0;   // desktop coordinates of monitor origin
    int            outWidth_ = 0, outHeight_ = 0;  // cropped output size
    int            cropX_ = 0, cropY_ = 0;
    bool           hasCrop_ = false;
    D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_11_0;

    uint32_t       frameCounter_ = 0;
    uint32_t       accessLostCount_ = 0;
    double         lastCaptureMs_ = 0.0;
    double         avgCaptureMs_ = 0.0;
};

} // namespace rp
