#pragma once
// Phase 3 — H.264/HEVC video encoding via FFmpeg.
//
// Encoder selection: hardware first (NVENC -> AMF -> QSV, each probed by actually
// opening the encoder), falling back to libx264/libx265. The successfully opened
// encoder name is reported so the UI can show it. Low latency settings: no
// B-frames, short GOP, small VBV, zero frame delay.
//
// Input  : BGRA rows (captured frame, any size, with stride)
// Output : EncodedPacket vector (keyframe flag + microsecond PTS)
//
// Aspect handling: input is letterboxed onto the configured target size so the
// encoder never needs re-initialization while a game window moves/resizes.

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

struct EncodedPacket {
    std::vector<uint8_t> data;
    uint64_t ptsUs = 0;        // presentation timestamp, microseconds since stream start
    bool     keyframe = false;
};

struct VideoEncoderParams {
    VideoCodec codec = VideoCodec::H264;
    int width = 1920;          // encode target size
    int height = 1080;
    int fps = 60;
    int bitrateKbps = 8000;    // 2..50000
    int gop = 120;             // keyframe interval in frames
    std::string preferredEncoder;  // optional override, e.g. "libx264"
};

class VideoEncoder {
public:
    VideoEncoder() = default;
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool init(const VideoEncoderParams& p, std::string* err = nullptr);

    // Encodes one BGRA frame. `srcW/srcH/srcStride` describe the input; the
    // encoder letterboxes it into the target size. Returns false on fatal
    // encoder failure (caller offers Restart Encoder / Switch To Software).
    bool encode(const uint8_t* bgra, int srcW, int srcH, int srcStride,
                std::vector<EncodedPacket>& out, std::string* err = nullptr);

    // Flushes pending frames (drain). Call before destroying or after EOS.
    bool flush(std::vector<EncodedPacket>& out);

    // Forces the next encoded frame to be a keyframe.
    void requestKeyframe();

    // Dynamic bitrate target (applied at the next keyframe boundary via encoder
    // re-open; rate control is baked at init for most encoders).
    void setBitrate(int kbps);

    // Re-opens the encoder when setBitrate marked it dirty. Returns true when a
    // restart actually happened (the next frame will be a keyframe).
    bool applyBitrateIfDirty();
    bool bitrateDirty() const { return bitrateDirty_; }

    const std::string& encoderName() const { return encoderName_; }
    int width() const { return p_.width; }
    int height() const { return p_.height; }
    int fps() const { return p_.fps; }
    int bitrateKbps() const { return p_.bitrateKbps; }

    // Statistics.
    double lastEncodeMs() const { return lastEncodeMs_; }
    double averageEncodeMs() const { return avgEncodeMs_; }
    uint32_t framesEncoded() const { return framesEncoded_; }
    uint64_t bytesEncoded() const { return bytesEncoded_; }

    // Lists encoders that exist in this build (for capability reporting).
    static std::vector<std::string> availableEncoders(VideoCodec codec);

private:
    void close();
    bool openEncoder(const std::string& name, std::string* err);

    VideoEncoderParams p_{};
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    uint8_t* scaleBuffer_ = nullptr;     // letterbox intermediate (yuv420p)
    int scaleBufSize_ = 0;
    int lastFitW_ = 0, lastFitH_ = 0;    // current letterbox fit size
    bool bitrateDirty_ = false;
    std::string encoderName_;
    int64_t frameCounter_ = 0;
    bool keyframeRequested_ = true;

    uint32_t framesEncoded_ = 0;
    uint64_t bytesEncoded_ = 0;
    double lastEncodeMs_ = 0.0;
    double avgEncodeMs_ = 0.0;
};

} // namespace rp
