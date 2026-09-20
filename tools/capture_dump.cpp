// Phase 2 test tool — verifies the DXGI capture pipeline on real hardware.
// Usage: capture_dump [seconds] [outputIndex]
// Captures frames for the given duration, prints achieved FPS / latency
// statistics, verifies frames actually change (content checksums) and dumps
// the last frame as raw BGRA that can be inspected with e.g. ffmpeg:
//   ffmpeg -f rawvideo -pix_fmt bgra -s WxH -i frame.bgra frame.png

#include "../capture/ScreenCapture.h"
#include "../common/Log.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

static uint32_t checksum(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i += 16) { h = (h ^ p[i]) * 16777619u; }
    return h;
}

int main(int argc, char** argv) {
    int seconds = argc > 1 ? std::atoi(argv[1]) : 3;
    int output = argc > 2 ? std::atoi(argv[2]) : 0;

    // DPI awareness so window rects match physical pixels in later phases.
    if (HMODULE u32 = LoadLibraryA("user32.dll")) {
        using Fn = BOOL(WINAPI*)(int);
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(u32, "SetProcessDpiAwarenessContext"))) {
            fn(-4 /*PER_MONITOR_AWARE_V2*/);
        }
    }

    rp::DisplayCapture cap;
    std::string err;
    if (!cap.init(output, &err)) {
        std::printf("FAIL init: %s\n", err.c_str());
        return 1;
    }
    std::printf("capture init OK: %dx%d (monitor %dx%d)\n", cap.width(), cap.height(), cap.monitorWidth(), cap.monitorHeight());

    auto t0 = std::chrono::steady_clock::now();
    int frames = 0, timeouts = 0;
    uint32_t lastChecksum = 0, distinct = 0;
    rp::CaptureFrame f;
    double captureMsTotal = 0.0;
    while (true) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= seconds) break;
        bool timedOut = false;
        if (cap.capture(f, 100, &timedOut, &err)) {
            ++frames;
            captureMsTotal += cap.lastCaptureMs();
            uint32_t c = checksum(f.data, static_cast<size_t>(f.stride) * f.height);
            if (c != lastChecksum) { ++distinct; lastChecksum = c; }
        } else if (timedOut) {
            ++timeouts;
        } else {
            std::printf("FAIL capture: %s\n", err.c_str());
            return 2;
        }
    }
    double dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("RESULT frames=%d distinct=%d timeouts=%d fps=%.1f avg_capture_ms=%.2f access_lost=%u\n",
                frames, distinct, timeouts, frames / dur,
                frames ? captureMsTotal / frames : 0.0, cap.accessLostCount());

    if (frames > 0) {
        std::ofstream out("frame.bgra", std::ios::binary);
        for (int y = 0; y < f.height; ++y) {
            out.write(reinterpret_cast<const char*>(f.data + static_cast<size_t>(y) * f.stride), static_cast<std::streamsize>(f.width * 4));
        }
        std::printf("wrote frame.bgra %dx%d\n", f.width, f.height);
    }
    return (frames > 10 && distinct > 3) ? 0 : 3;
}
