// Phase 2 — DXGI Output Duplication capture. See ScreenCapture.h for the design.

#include <string>
#include <utility>
#include <vector>
#include "ScreenCapture.h"

#include "../common/Log.h"

#include <dwmapi.h>
#include <windows.h>

#include <psapi.h>

#include <chrono>
#include <cstring>

// rp::log macros (RP_INFO etc.) provide printf-free stream logging.

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")
#endif

namespace rp {
namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct EnumCtx { std::vector<WindowInfo>* out; };

BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    // Skip tool windows and cloaked (suspended UWP / virtual desktop) windows.
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style & WS_EX_TOOLWINDOW) return TRUE;

    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked != 0) return TRUE;

    wchar_t title[256];
    int len = GetWindowTextW(hwnd, title, 256);
    if (len <= 0) return TRUE;

    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    POINT tl{ 0, 0 };
    ClientToScreen(hwnd, &tl);
    if (rc.right - rc.left < 32 || rc.bottom - rc.top < 32) return TRUE;

    WindowInfo wi;
    wi.hwnd = hwnd;
    wi.title.assign(title, len);
    wi.x = tl.x; wi.y = tl.y;
    wi.w = rc.right - rc.left; wi.h = rc.bottom - rc.top;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            wchar_t exe[MAX_PATH] = {};
            DWORD exeLen = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, exe, &exeLen)) {
                const wchar_t* base = wcsrchr(exe, L'\\');
                base = base ? base + 1 : exe;
                wi.processExe = base;
            }
            CloseHandle(proc);
        }
    }
    ctx->out->push_back(std::move(wi));
    return TRUE;
}

} // namespace

std::vector<OutputInfo> enumerateOutputs(std::string* err) {
    std::vector<OutputInfo> outputs;
    rp::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        if (err) *err = "CreateDXGIFactory1 failed hr=0x" + std::to_string(static_cast<uint32_t>(hr));
        return outputs;
    }
    rp::ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        rp::ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc;
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) { output.reset(); continue; }
            OutputInfo info;
            info.index = static_cast<int>(outputs.size());
            info.name = desc.DeviceName;
            info.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            info.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            info.attachedToDesktop = true;
            outputs.push_back(info);
            output.reset();
        }
        adapter.reset();
    }
    return outputs;
}

std::vector<WindowInfo> listCaptureWindows() {
    std::vector<WindowInfo> windows;
    EnumCtx ctx{ &windows };
    EnumWindows(enumProc, reinterpret_cast<LPARAM>(&ctx));
    return windows;
}

// ---------------------------------------------------------------------------

DisplayCapture::~DisplayCapture() { releaseAll(); }

void DisplayCapture::releaseAll() {
    if (mappedValid_ && context_) context_->Unmap(staging_, 0);
    mappedValid_ = false;
    duplication_.reset();
    staging_.reset();
    output_.reset();
    context_.reset();
    device_.reset();
}

bool DisplayCapture::init(int outputIndex, std::string* err) {
    releaseAll();
    outputIndex_ = outputIndex;

    rp::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) { if (err) *err = "CreateDXGIFactory1 failed"; return false; }

    // Prefer the adapter that owns the chosen output for zero-copy interop later.
    if (!findOutput(factory, outputIndex, err)) return false;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    // device on the adapter that owns the output
    rp::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(static_cast<UINT>(adapterIndex_), &adapter))) {
        if (err) *err = "EnumAdapters1 failed";
        return false;
    }
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels),
                           D3D11_SDK_VERSION, &device_, &featureLevel_, &context_);
    if (FAILED(hr)) { if (err) *err = "D3D11CreateDevice failed hr=0x" + std::to_string(static_cast<uint32_t>(hr)); return false; }

    DXGI_OUTPUT_DESC od;
    if (FAILED(output_->GetDesc(&od))) { if (err) *err = "GetDesc(output) failed"; return false; }
    monWidth_ = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
    monHeight_ = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
    monLeft_ = od.DesktopCoordinates.left;
    monTop_ = od.DesktopCoordinates.top;

    if (!duplicateOutput(err)) return false;

    // Staging texture: same size as the monitor, CPU-readable.
    D3D11_TEXTURE2D_DESC sd;
    std::memset(&sd, 0, sizeof(sd));
    sd.Width = static_cast<UINT>(monWidth_);
    sd.Height = static_cast<UINT>(monHeight_);
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device_->CreateTexture2D(&sd, nullptr, &staging_);
    if (FAILED(hr)) { if (err) *err = "CreateTexture2D(staging) failed"; return false; }

    hasCrop_ = false;
    cropX_ = cropY_ = 0;
    outWidth_ = monWidth_;
    outHeight_ = monHeight_;
    frameCounter_ = 0;
    RP_INFO() << "[capture] initialized output " << outputIndex_ << " (" << monWidth_ << "x" << monHeight_
              << " at " << monLeft_ << "," << monTop_ << ") d3d-level=0x" << std::hex << featureLevel_ << std::dec;
    return true;
}

bool DisplayCapture::findOutput(IDXGIFactory1* factory, int outputIndex, std::string* err) {
    if (err) err->clear();
    int global = 0;
    rp::ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        rp::ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                if (global == outputIndex) {
                    adapterIndex_ = static_cast<int>(a);
                    output_ = output;
                    return true;
                }
                ++global;
            }
            output.reset();
        }
        adapter.reset();
    }
    if (err) *err = "output index " + std::to_string(outputIndex) + " out of range";
    return false;
}

bool DisplayCapture::duplicateOutput(std::string* err) {
    rp::ComPtr<IDXGIOutput1> out1;
    if (FAILED(output_->QueryInterface(IID_IDXGIOutput1, reinterpret_cast<void**>(&out1)))) {
        if (err) *err = "IDXGIOutput1 unavailable (pre-Win8.1?)";
        return false;
    }
    HRESULT hr = out1->DuplicateOutput(device_, &duplication_);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
            *err = "DuplicateOutput: max duplication sessions reached (another capture app running?)";
        else if (hr == E_ACCESSDENIED)
            *err = "DuplicateOutput: access denied (secure desktop / UAC prompt up)";
        else
            *err = "DuplicateOutput failed hr=0x" + std::to_string(static_cast<uint32_t>(hr));
        return false;
    }
    return true;
}

bool DisplayCapture::capture(CaptureFrame& out, int timeoutMs, bool* timedOut, std::string* err) {
    if (timedOut) *timedOut = false;
    if (!duplication_) { if (err) *err = "capture: not initialized"; return false; }

    double t0 = nowMs();
    DXGI_OUTDUPL_FRAME_INFO info;
    rp::ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(static_cast<UINT>(timeoutMs), &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) { if (timedOut) *timedOut = true; return false; }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        ++accessLostCount_;
        RP_WARN() << "[capture] DXGI_ERROR_ACCESS_LOST (mode switch / fullscreen transition) - reinitializing";
        for (int attempt = 0; attempt < 3; ++attempt) {
            Sleep(50);
            if (!init(outputIndex_, err)) continue;
            hr = duplication_->AcquireNextFrame(static_cast<UINT>(timeoutMs), &info, &resource);
            if (SUCCEEDED(hr)) break;
            if (hr == DXGI_ERROR_ACCESS_LOST) { ++accessLostCount_; continue; }
            if (err) *err = "AcquireNextFrame failed hr=0x" + std::to_string(static_cast<uint32_t>(hr));
            return false;
        }
        if (FAILED(hr)) { if (err) *err = "capture: could not re-duplicate output after access lost"; return false; }
    } else if (FAILED(hr)) {
        if (err) *err = "AcquireNextFrame failed hr=0x" + std::to_string(static_cast<uint32_t>(hr));
        return false;
    }

    bool ok = false;
    do {
        rp::ComPtr<ID3D11Texture2D> desktop;
        if (FAILED(resource->QueryInterface(IID_ID3D11Texture2D, reinterpret_cast<void**>(&desktop)))) {
            if (err) *err = "AcquireNextFrame returned a non-texture resource";
            break;
        }
        if (mappedValid_) { context_->Unmap(staging_, 0); mappedValid_ = false; }
        context_->CopyResource(staging_, desktop);
        hr = context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped_);
        if (FAILED(hr)) { if (err) *err = "Map(staging) failed"; break; }
        mappedValid_ = true;

        out.data = static_cast<const uint8_t*>(mapped_.pData) + static_cast<size_t>(cropY_) * mapped_.RowPitch
                   + static_cast<size_t>(cropX_) * 4;
        out.width = outWidth_;
        out.height = outHeight_;
        out.stride = static_cast<int>(mapped_.RowPitch);
        out.frameId = ++frameCounter_;
        using namespace std::chrono;
        out.timestampUs = static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
        ok = true;
    } while (false);

    duplication_->ReleaseFrame();
    double dt = nowMs() - t0;
    lastCaptureMs_ = dt;
    avgCaptureMs_ = avgCaptureMs_ == 0.0 ? dt : (avgCaptureMs_ * 0.9 + dt * 0.1);
    return ok;
}

bool DisplayCapture::updateWindowCrop(void* hwndRaw) {
    HWND hwnd = static_cast<HWND>(hwndRaw);
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;

    RECT rc;
    if (!GetClientRect(hwnd, &rc)) return false;
    POINT tl{ 0, 0 };
    if (!ClientToScreen(hwnd, &tl)) return false;

    int x = tl.x - monLeft_;
    int y = tl.y - monTop_;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > monWidth_)  w = monWidth_ - x;
    if (y + h > monHeight_) h = monHeight_ - y;
    if (w < 16 || h < 16) return false;

    setCropRect(x, y, w, h);
    return true;
}

void DisplayCapture::setCropRect(int x, int y, int w, int h) {
    cropX_ = x; cropY_ = y;
    outWidth_ = w; outHeight_ = h;
    hasCrop_ = true;
}

void DisplayCapture::clearCrop() {
    hasCrop_ = false;
    cropX_ = cropY_ = 0;
    outWidth_ = monWidth_;
    outHeight_ = monHeight_;
}

} // namespace rp
