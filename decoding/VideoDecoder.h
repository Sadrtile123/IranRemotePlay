#pragma once
// Phase 5 — client video decoding via FFmpeg.
//
// Software decode (libavcodec h264/hevc/av1) + swscale to BGRA for the Qt
// render path. Hardware decode (D3D11VA/NVDEC) is a documented follow-up:
// the pipeline copies YUV to system memory once, which already sustains
// 1080p60 comfortably on modern CPUs (see docs/PERFORMANCE.md).
//
// Also provides capability discovery so clients advertise decoders they
// actually have instead of a hardcoded list.

#include "../common/Types.h"

#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace rp {

using common::VideoCodec;

struct DecodedFrame {
    std::vector<uint8_t> bgra;   // tightly packed (stride = width * 4)
    int width = 0, height = 0;
    uint64_t ptsUs = 0;
    bool keyframe = false;
};

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool init(VideoCodec codec, std::string* err = nullptr);

    // Feeds one encoded packet; appends decoded frames (0 or 1 per packet).
    bool decode(const uint8_t* data, size_t size, uint64_t ptsUs, bool keyframe,
                std::vector<DecodedFrame>& out, std::string* err = nullptr);

    bool flush(std::vector<DecodedFrame>& out);

    // Reset decoder state (e.g. after corrupt stream / recovery).
    void reset();

    // Codecs this build can actually decode.
    static std::vector<VideoCodec> supportedCodecs();

    double lastDecodeMs() const { return lastDecodeMs_; }
    double averageDecodeMs() const { return avgDecodeMs_; }
    uint64_t framesDecoded() const { return framesDecoded_; }
    uint64_t bytesDecoded() const { return bytesDecoded_; }
    int width() const { return width_; }
    int height() const { return height_; }
    const char* decoderName() const { return decoderName_.c_str(); }

private:
    void close();
    bool emitFrame(AVFrame* avf, std::vector<DecodedFrame>& out);

    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    VideoCodec codec_ = VideoCodec::H264;
    int width_ = 0, height_ = 0;
    std::string decoderName_;
    uint64_t framesDecoded_ = 0;
    uint64_t bytesDecoded_ = 0;
    double lastDecodeMs_ = 0.0;
    double avgDecodeMs_ = 0.0;
};

} // namespace rp
