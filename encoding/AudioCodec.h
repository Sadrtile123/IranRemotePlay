#pragma once
// Phase 7 — Opus audio codec via FFmpeg (libopus).
// Input : interleaved s16 stereo 48 kHz, 10 ms granularity.
// Output: 20 ms Opus packets (960 frames each) with microsecond timestamps.

#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVFifo;

namespace rp {

struct AudioPacket {
    std::vector<uint8_t> data;
    uint64_t ptsUs = 0;
};

class AudioEncoderOpus {
public:
    AudioEncoderOpus() = default;
    ~AudioEncoderOpus();

    AudioEncoderOpus(const AudioEncoderOpus&) = delete;
    AudioEncoderOpus& operator=(const AudioEncoderOpus&) = delete;

    // bitrateKbps: 64..192 (VBR). DTX off (we always send for simplicity).
    bool init(int bitrateKbps = 128, std::string* err = nullptr);

    // Feed arbitrary-length s16 stereo samples (interleaved). Buffered
    // internally; emits one AudioPacket per 960 frames (20 ms).
    bool encode(const int16_t* samples, size_t frameCount, uint64_t timestampUs,
                std::vector<AudioPacket>& out, std::string* err = nullptr);

    // Number of buffered frames waiting for a full Opus frame.
    size_t bufferedFrames() const;

    double averageEncodeMs() const { return avgEncodeMs_; }
    uint64_t packetsEncoded() const { return packets_; }

private:
    void close();

    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFifo* fifo_ = nullptr;          // s16 interleaved sample FIFO
    int64_t nextPts_ = 0;             // in samples
    uint64_t packets_ = 0;
    double avgEncodeMs_ = 0.0;
};

// ---------------------------------------------------------------

class AudioDecoderOpus {
public:
    AudioDecoderOpus() = default;
    ~AudioDecoderOpus();

    AudioDecoderOpus(const AudioDecoderOpus&) = delete;
    AudioDecoderOpus& operator=(const AudioDecoderOpus&) = delete;

    bool init(std::string* err = nullptr);

    // Decode one Opus packet -> interleaved s16 stereo frames.
    bool decode(const uint8_t* data, size_t size,
                std::vector<int16_t>& outSamples, std::string* err = nullptr);

    double averageDecodeMs() const { return avgDecodeMs_; }
    uint64_t packetsDecoded() const { return packets_; }

    void reset();

private:
    void close();
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    uint64_t packets_ = 0;
    double avgDecodeMs_ = 0.0;
};

} // namespace rp
