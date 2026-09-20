// Phase 3 — FFmpeg video encoder implementation. See VideoEncoder.h.

#include "VideoEncoder.h"

#include "../common/Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <cstring>

namespace rp {
namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> encoderCandidates(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::Hevc:
            return { "hevc_nvenc", "hevc_amf", "hevc_qsv", "libx265" };
        case VideoCodec::Av1:
            return { "av1_nvenc", "av1_amf", "av1_qsv", "libsvtav1" };
        case VideoCodec::H264:
        default:
            return { "h264_nvenc", "h264_amf", "h264_qsv", "libx264" };
    }
}

} // namespace

std::vector<std::string> VideoEncoder::availableEncoders(VideoCodec codec) {
    std::vector<std::string> out;
    for (const auto& name : encoderCandidates(codec)) {
        if (avcodec_find_encoder_by_name(name.c_str())) out.push_back(name);
    }
    return out;
}

VideoEncoder::~VideoEncoder() { close(); }

void VideoEncoder::close() {
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (scaleBuffer_) { av_freep(&scaleBuffer_); scaleBuffer_ = nullptr; scaleBufSize_ = 0; }
    if (packet_) { av_packet_free(&packet_); packet_ = nullptr; }
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    if (ctx_) { avcodec_free_context(&ctx_); ctx_ = nullptr; }
}

bool VideoEncoder::init(const VideoEncoderParams& p, std::string* err) {
    close();
    p_ = p;
    frameCounter_ = 0;
    keyframeRequested_ = true;
    framesEncoded_ = 0;
    bytesEncoded_ = 0;
    avgEncodeMs_ = lastEncodeMs_ = 0.0;

    std::vector<std::string> candidates;
    if (!p.preferredEncoder.empty()) candidates.push_back(p.preferredEncoder);
    for (const auto& c : encoderCandidates(p.codec)) {
        if (c != p.preferredEncoder) candidates.push_back(c);
    }

    std::string lastErr;
    for (const auto& name : candidates) {
        std::string openErr;
        if (openEncoder(name, &openErr)) {
            encoderName_ = name;
            RP_INFO() << "[encoder] opened " << name << " " << p_.width << "x" << p_.height
                      << "@" << p_.fps << " " << p_.bitrateKbps << "kbps";
            return true;
        }
        RP_INFO() << "[encoder] " << name << " unavailable: " << openErr;
        lastErr = openErr;
    }
    if (err) *err = "no usable encoder (" + lastErr + ")";
    return false;
}

bool VideoEncoder::openEncoder(const std::string& name, std::string* err) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) { if (err) *err = "encoder not found in this FFmpeg build"; return false; }

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) { if (err) *err = "avcodec_alloc_context3 failed"; return false; }

    ctx_->width = p_.width;
    ctx_->height = p_.height;
    ctx_->time_base = AVRational{ 1, p_.fps };
    ctx_->framerate = AVRational{ p_.fps, 1 };
    ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx_->bit_rate = static_cast<int64_t>(p_.bitrateKbps) * 1000;
    ctx_->rc_max_rate = ctx_->bit_rate;
    // Small VBV (~1.5 frames) for latency; CBR-ish behavior.
    ctx_->rc_buffer_size = static_cast<int>(ctx_->bit_rate / p_.fps * 3 / 2);
    ctx_->gop_size = p_.gop;
    ctx_->max_b_frames = 0;                    // no B-frames (latency)
    ctx_->thread_count = (name.rfind("libx2", 0) == 0) ? 0 : 1;  // x264 can thread internally

    AVDictionary* opts = nullptr;
    // Hardware low-latency presets (options unknown to a given encoder are ignored).
    if (name.find("nvenc") != std::string::npos) {
        av_dict_set(&opts, "preset", "p1", 0);          // fastest preset
        av_dict_set(&opts, "tune", "ull", 0);           // ultra low latency
        av_dict_set(&opts, "rc", "cbr", 0);
        av_dict_set(&opts, "delay", "0", 0);
        av_dict_set(&opts, "zerolatency", "1", 0);
    } else if (name.find("amf") != std::string::npos) {
        av_dict_set(&opts, "usage", "ultralowlatency", 0);
        av_dict_set(&opts, "rc", "cbr", 0);
    } else if (name.find("qsv") != std::string::npos) {
        av_dict_set(&opts, "preset", "veryfast", 0);
        av_dict_set(&opts, "low_power", "1", 0);
    } else if (name == "libx264") {
        av_dict_set(&opts, "preset", "ultrafast", 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
    } else if (name == "libx265") {
        av_dict_set(&opts, "preset", "ultrafast", 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
        av_dict_set(&opts, "x265-params", "bframes=0:repeat-headers=1", 0);
    }

    int rc = avcodec_open2(ctx_, codec, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        char buf[128]{};
        av_strerror(rc, buf, sizeof(buf));
        if (err) *err = "avcodec_open2(" + name + ") failed: " + buf;
        avcodec_free_context(&ctx_);
        ctx_ = nullptr;
        return false;
    }

    frame_ = av_frame_alloc();
    if (!frame_ || av_frame_get_buffer(frame_, 32) < 0) {
        if (err) *err = "frame alloc failed";
        close();
        return false;
    }
    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width = p_.width;
    frame_->height = p_.height;

    packet_ = av_packet_alloc();
    if (!packet_) { if (err) *err = "packet alloc failed"; close(); return false; }

    // Letterbox intermediate buffer at fitted size (allocated on first frame).
    sws_ = nullptr;
    return true;
}

bool VideoEncoder::encode(const uint8_t* bgra, int srcW, int srcH, int srcStride,
                          std::vector<EncodedPacket>& out, std::string* err) {
    if (!ctx_ || !frame_) { if (err) *err = "encoder not initialized"; return false; }

    double t0 = nowMs();
    out.clear();

    // ---- letterbox BGRA -> YUV420P ----
    // Fit source inside target, preserving aspect; black bars around.
    const int tw = p_.width, th = p_.height;
    int fitW, fitH;
    if (srcW * static_cast<int64_t>(th) >= srcH * static_cast<int64_t>(tw)) {
        fitW = tw;
        fitH = static_cast<int>((static_cast<int64_t>(tw) * srcH + srcW - 1) / srcW);
        if (fitH > th) fitH = th;
        if (fitH < 1) fitH = 1;
    } else {
        fitH = th;
        fitW = static_cast<int>((static_cast<int64_t>(th) * srcW + srcH - 1) / srcH);
        if (fitW > tw) fitW = tw;
        if (fitW < 1) fitW = 1;
    }
    // Even dimensions for yuv420.
    fitW &= ~1;
    fitH &= ~1;
    const int offX = (tw - fitW) / 2 & ~1;
    const int offY = (th - fitH) / 2 & ~1;

    if (!sws_ || lastFitW_ != fitW || lastFitH_ != fitH) {
        if (sws_) sws_freeContext(sws_);
        const int align = 1;  // tight packing so memcpy row counts match exactly
        const size_t yuvSize = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, fitW, fitH, align);
        if (scaleBufSize_ < static_cast<int>(yuvSize)) {
            if (scaleBuffer_) av_freep(reinterpret_cast<void**>(&scaleBuffer_));
            scaleBuffer_ = static_cast<uint8_t*>(av_malloc(yuvSize));
            scaleBufSize_ = static_cast<int>(yuvSize);
        }
        sws_ = sws_getContext(srcW, srcH, AV_PIX_FMT_BGRA, fitW, fitH, AV_PIX_FMT_YUV420P,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) { if (err) *err = "sws_getContext failed"; return false; }
        lastFitW_ = fitW;
        lastFitH_ = fitH;
    }
    if (!scaleBuffer_) { if (err) *err = "letterbox buffer missing"; return false; }

    {
        uint8_t* scalePlanes[4]{};
        int scaleStride[4]{};
        av_image_fill_arrays(scalePlanes, scaleStride, scaleBuffer_, AV_PIX_FMT_YUV420P, fitW, fitH, 1);
        const uint8_t* srcSlice[1]{ bgra };
        const int srcStrideArr[1]{ srcStride };
        sws_scale(sws_, srcSlice, srcStrideArr, 0, srcH, scalePlanes, scaleStride);
    }

    if (av_frame_make_writable(frame_) < 0) { if (err) *err = "frame not writable"; return false; }

    // Black background + blit fitted rect.
    std::memset(frame_->data[0], 16, static_cast<size_t>(frame_->linesize[0]) * th);
    for (int y = 0; y < th / 2; ++y) {
        std::memset(frame_->data[1] + static_cast<size_t>(y) * frame_->linesize[1], 128, static_cast<size_t>(tw / 2));
        std::memset(frame_->data[2] + static_cast<size_t>(y) * frame_->linesize[2], 128, static_cast<size_t>(tw / 2));
    }
    {
        uint8_t* scalePlanes[4]{};
        int scaleStride[4]{};
        av_image_fill_arrays(scalePlanes, scaleStride, scaleBuffer_, AV_PIX_FMT_YUV420P, fitW, fitH, 1);
        for (int y = 0; y < fitH; ++y) {
            std::memcpy(frame_->data[0] + static_cast<size_t>(offY + y) * frame_->linesize[0] + offX,
                        scalePlanes[0] + static_cast<size_t>(y) * scaleStride[0], static_cast<size_t>(fitW));
        }
        for (int y = 0; y < fitH / 2; ++y) {
            std::memcpy(frame_->data[1] + static_cast<size_t>(offY / 2 + y) * frame_->linesize[1] + offX / 2,
                        scalePlanes[1] + static_cast<size_t>(y) * scaleStride[1], static_cast<size_t>(fitW / 2));
            std::memcpy(frame_->data[2] + static_cast<size_t>(offY / 2 + y) * frame_->linesize[2] + offX / 2,
                        scalePlanes[2] + static_cast<size_t>(y) * scaleStride[2], static_cast<size_t>(fitW / 2));
        }
    }

    frame_->pts = frameCounter_++;
    if (keyframeRequested_) {
        frame_->pict_type = AV_PICTURE_TYPE_I;
        keyframeRequested_ = false;
    } else {
        frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }

    if (avcodec_send_frame(ctx_, frame_) < 0) { if (err) *err = "avcodec_send_frame failed"; return false; }

    bool ok = true;
    while (true) {
        int rc = avcodec_receive_packet(ctx_, packet_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) { if (err) *err = "avcodec_receive_packet failed"; ok = false; break; }
        EncodedPacket ep;
        ep.data.assign(packet_->data, packet_->data + packet_->size);
        ep.ptsUs = packet_->dts >= 0 ? (static_cast<uint64_t>(packet_->dts) * 1'000'000ull / static_cast<uint64_t>(p_.fps))
                                     : (static_cast<uint64_t>(frameCounter_) * 1'000'000ull / static_cast<uint64_t>(p_.fps));
        ep.keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
        out.push_back(std::move(ep));
        bytesEncoded_ += packet_->size;
        ++framesEncoded_;
        av_packet_unref(packet_);
    }

    double dt = nowMs() - t0;
    lastEncodeMs_ = dt;
    avgEncodeMs_ = avgEncodeMs_ == 0.0 ? dt : (avgEncodeMs_ * 0.9 + dt * 0.1);
    return ok;
}

bool VideoEncoder::flush(std::vector<EncodedPacket>& out) {
    if (!ctx_) return false;
    avcodec_send_frame(ctx_, nullptr);
    while (true) {
        int rc = avcodec_receive_packet(ctx_, packet_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) break;
        EncodedPacket ep;
        ep.data.assign(packet_->data, packet_->data + packet_->size);
        ep.ptsUs = static_cast<uint64_t>(packet_->dts) * 1'000'000ull / static_cast<uint64_t>(p_.fps);
        ep.keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
        out.push_back(std::move(ep));
        av_packet_unref(packet_);
    }
    return true;
}

void VideoEncoder::requestKeyframe() { keyframeRequested_ = true; }

void VideoEncoder::setBitrate(int kbps) {
    kbps = std::clamp(kbps, 2000, 50000);
    if (kbps == p_.bitrateKbps) return;
    p_.bitrateKbps = kbps;
    bitrateDirty_ = true;
    RP_INFO() << "[encoder] bitrate target -> " << kbps << " kbps (applied at next restart boundary)";
}

bool VideoEncoder::applyBitrateIfDirty() {
    if (!bitrateDirty_ || !ctx_) return false;
    VideoEncoderParams np = p_;
    std::string err;
    // Re-open the encoder: FFmpeg bakes rate control at init for x264 and some
    // hardware encoders. Hysteresis in the rate controller (Phase 15) makes this rare.
    if (init(np, &err)) {
        bitrateDirty_ = false;
        return true;
    }
    RP_ERROR() << "[encoder] bitrate reinit failed: " << err;
    return false;
}

} // namespace rp
