// Phase 3 test tool — verifies the FFmpeg encode pipeline.
// Usage: encode_test [codec:h264|hevc] [w] [h] [fps] [kbps]
// Synthesizes moving BGRA frames, encodes N frames, prints per-encoder probe
// results, packet stats and encode latency. Works without a GPU (falls back to
// libx264), so it also validates the software fallback path.

#include <string>
#include "../encoding/VideoEncoder.h"
#include "../common/Log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace rp;
using rp::common::VideoCodec;

int main(int argc, char** argv) {
    auto codec = (argc > 1 && std::string(argv[1]) == "hevc") ? VideoCodec::Hevc : VideoCodec::H264;
    int W = argc > 2 ? std::atoi(argv[2]) : 1280;
    int H = argc > 3 ? std::atoi(argv[3]) : 720;
    int FPS = argc > 4 ? std::atoi(argv[4]) : 60;
    int KBPS = argc > 5 ? std::atoi(argv[5]) : 4000;
    const int N = 120;

    std::printf("available encoders:");
    for (const auto& e : VideoEncoder::availableEncoders(codec)) std::printf(" %s", e.c_str());
    std::printf("\n");

    VideoEncoderParams p;
    p.codec = codec; p.width = W; p.height = H; p.fps = FPS; p.bitrateKbps = KBPS; p.gop = FPS * 2;

    VideoEncoder enc;
    std::string err;
    if (!enc.init(p, &err)) { std::printf("FAIL init: %s\n", err.c_str()); return 1; }
    std::printf("encoder: %s (%dx%d@%d)\n", enc.encoderName().c_str(), W, H, FPS);

    std::vector<uint8_t> bgra(static_cast<size_t>(W) * H * 4);
    std::vector<EncodedPacket> pkts;
    auto t0 = std::chrono::steady_clock::now();
    int totalPackets = 0, keyframes = 0;
    size_t totalBytes = 0;
    for (int i = 0; i < N; ++i) {
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                uint8_t* px = &bgra[(static_cast<size_t>(y) * W + x) * 4];
                px[0] = static_cast<uint8_t>(x + i);         // B
                px[1] = static_cast<uint8_t>(y);             // G
                px[2] = static_cast<uint8_t>((x ^ y) + i);   // R
                px[3] = 255;                                 // A
            }
        }
        if (!enc.encode(bgra.data(), W, H, W * 4, pkts, &err)) {
            std::printf("FAIL encode frame %d: %s\n", i, err.c_str());
            return 2;
        }
        totalPackets += static_cast<int>(pkts.size());
        for (const auto& pk : pkts) { totalBytes += pk.data.size(); if (pk.keyframe) ++keyframes; }
    }
    auto dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("RESULT frames=%d packets=%d keyframes=%d bytes=%zu avg_ms=%.2f avg_kbps=%.0f encoder=%s\n",
                N, totalPackets, keyframes, totalBytes, enc.averageEncodeMs(),
                totalBytes * 8.0 / dur / 1000.0, enc.encoderName().c_str());

    bool ok = totalPackets >= N - 5 && keyframes >= 1 && enc.averageEncodeMs() < 100.0;
    std::printf(ok ? "PHASE3 PASS\n" : "PHASE3 FAIL\n");
    return ok ? 0 : 3;
}
