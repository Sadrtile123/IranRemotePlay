// Phase 5 — FFmpeg video decoder implementation. See VideoDecoder.h.

#include <string>
#include <utility>
#include <vector>
#include "VideoDecoder.h"

#include "../common/Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <chrono>
#include <cstring>

namespace rp {
namespace {

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

AVCodecID codecId(VideoCodec c) {
    switch (c) {
        case VideoCodec::Hevc: return AV_CODEC_ID_HEVC;
        case VideoCodec::Av1:  return AV_CODEC_ID_AV1;
        case VideoCodec::H264:
        default:               return AV_CODEC_ID_H264;
    }
}

} // namespace

std::vector<VideoCodec> VideoDecoder::supportedCodecs() {
    std::vector<VideoCodec> out;
    const VideoCodec all[] = { VideoCodec::H264, VideoCodec::Hevc, VideoCodec::Av1 };
    for (VideoCodec c : all) {
        if (avcodec_find_decoder(codecId(c))) out.push_back(c);
    }
    return out;
}

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (packet_) { av_packet_free(&packet_); packet_ = nullptr; }
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    if (ctx_) { avcodec_free_context(&ctx_); ctx_ = nullptr; }
    width_ = height_ = 0;
}

bool VideoDecoder::init(VideoCodec codec, std::string* err) {
    close();
    codec_ = codec;

    const AVCodec* dec = avcodec_find_decoder(codecId(codec));
    if (!dec) { if (err) *err = "decoder not found in this FFmpeg build"; return false; }

    ctx_ = avcodec_alloc_context3(dec);
    if (!ctx_) { if (err) *err = "avcodec_alloc_context3 failed"; return false; }
    ctx_->thread_count = 0;      // auto: parallel decode of frame slices
    ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    // Low-latency: do not wait for future frames to confirm current one.
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;

    if (avcodec_open2(ctx_, dec, nullptr) < 0) {
        if (err) *err = "avcodec_open2(decoder) failed";
        close();
        return false;
    }

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) { if (err) *err = "alloc failed"; close(); return false; }

    decoderName_ = dec->name;
    RP_INFO() << "[decoder] opened " << decoderName_;
    return true;
}

bool VideoDecoder::decode(const uint8_t* data, size_t size, uint64_t ptsUs, bool keyframe,
                          std::vector<DecodedFrame>& out, std::string* err) {
    if (!ctx_) { if (err) *err = "decoder not initialized"; return false; }
    const double t0 = nowMs();

    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);
    packet_->flags = keyframe ? AV_PKT_FLAG_KEY : 0;
    packet_->pts = static_cast<int64_t>(ptsUs);   // passthrough timestamps (us)

    if (avcodec_send_packet(ctx_, packet_) < 0) {
        // Corrupt packet: flush decoder state so the next keyframe restarts cleanly.
        packet_->data = nullptr; packet_->size = 0;
        avcodec_send_packet(ctx_, packet_);
        if (err) *err = "avcodec_send_packet rejected packet";
        return false;
    }
    bytesDecoded_ += size;

    bool ok = true;
    while (true) {
        const int rc = avcodec_receive_frame(ctx_, frame_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            if (err) *err = "avcodec_receive_frame failed";
            ok = false;
            break;
        }
        if (!emitFrame(frame_, out)) ok = false;
        av_frame_unref(frame_);
    }

    packet_->data = nullptr; packet_->size = 0;
    const double dt = nowMs() - t0;
    lastDecodeMs_ = dt;
    avgDecodeMs_ = avgDecodeMs_ == 0.0 ? dt : (avgDecodeMs_ * 0.9 + dt * 0.1);
    return ok;
}

bool VideoDecoder::emitFrame(AVFrame* avf, std::vector<DecodedFrame>& out) {
    width_ = avf->width;
    height_ = avf->height;
    if (width_ <= 0 || height_ <= 0) return false;

    // Recreate the scaler when the source format or size changes (encoder
    // restarts can alter dimensions mid-stream; a stale sws_ would corrupt
    // the picture or overflow the destination).
    if (!sws_ || lastW_ != width_ || lastH_ != height_ || lastFmt_ != avf->format) {
        if (sws_) sws_freeContext(sws_);
        sws_ = sws_getContext(width_, height_, static_cast<AVPixelFormat>(avf->format),
                              width_, height_, AV_PIX_FMT_BGRA,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        lastW_ = width_;
        lastH_ = height_;
        lastFmt_ = avf->format;
        if (!sws_) return false;
    }

    DecodedFrame df;
    df.width = width_;
    df.height = height_;
    df.ptsUs = avf->pts >= 0 ? static_cast<uint64_t>(avf->pts) : 0;
    df.keyframe = (avf->flags & AV_FRAME_FLAG_KEY) != 0;
    df.bgra.resize(static_cast<size_t>(width_) * height_ * 4);
    uint8_t* planes[4]{ df.bgra.data(), nullptr, nullptr, nullptr };
    int strides[4]{ width_ * 4, 0, 0, 0 };
    sws_scale(sws_, avf->data, avf->linesize, 0, height_, planes, strides);
    out.push_back(std::move(df));
    ++framesDecoded_;
    return true;
}

bool VideoDecoder::flush(std::vector<DecodedFrame>& out) {
    if (!ctx_) return false;
    avcodec_send_packet(ctx_, nullptr);
    while (true) {
        const int rc = avcodec_receive_frame(ctx_, frame_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) break;
        emitFrame(frame_, out);
        av_frame_unref(frame_);
    }
    return true;
}

void VideoDecoder::reset() {
    std::string err;
    init(codec_, &err);
}

} // namespace rp
