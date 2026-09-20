// Phase 7 — Opus encode/decode via FFmpeg libopus. See AudioCodec.h.

#include "AudioCodec.h"

#include "../common/Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/fifo.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <chrono>
#include <cstring>

namespace rp {
namespace {
double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
constexpr int kOpusFrameSize = 960;      // 20 ms @ 48 kHz
constexpr AVSampleFormat kFormat = AV_SAMPLE_FMT_S16;
} // namespace

// ---------------- encoder ----------------

AudioEncoderOpus::~AudioEncoderOpus() { close(); }

void AudioEncoderOpus::close() {
    if (fifo_) { av_fifo_freep2(&fifo_); fifo_ = nullptr; }
    if (packet_) { av_packet_free(&packet_); packet_ = nullptr; }
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    if (ctx_) { avcodec_free_context(&ctx_); ctx_ = nullptr; }
}

bool AudioEncoderOpus::init(int bitrateKbps, std::string* err) {
    close();
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!enc) { if (err) *err = "libopus encoder missing from this FFmpeg build"; return false; }

    ctx_ = avcodec_alloc_context3(enc);
    if (!ctx_) { if (err) *err = "alloc failed"; return false; }
    AVChannelLayout stereo;
    av_channel_layout_default(&stereo, 2);
    av_channel_layout_copy(&ctx_->ch_layout, &stereo);
    ctx_->sample_rate = 48000;
    ctx_->sample_fmt = kFormat;               // libopus accepts s16
    ctx_->bit_rate = static_cast<int64_t>(bitrateKbps) * 1000;
    ctx_->frame_size = kOpusFrameSize;        // 20 ms
    ctx_->time_base = AVRational{ 1, 48000 };
    ctx_->flags |= AV_CODEC_FLAG_BITEXACT;    // deterministic output

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "application", "lowdelay", 0);
    av_dict_set(&opts, "frame_duration", "20", 0);
    const int rc = avcodec_open2(ctx_, enc, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        char buf[128]{};
        av_strerror(rc, buf, sizeof(buf));
        if (err) *err = std::string("avcodec_open2(opus) failed: ") + buf;
        close();
        return false;
    }

    frame_ = av_frame_alloc();
    frame_->format = kFormat;
    frame_->sample_rate = 48000;
    av_channel_layout_copy(&frame_->ch_layout, &stereo);
    frame_->nb_samples = ctx_->frame_size > 0 ? ctx_->frame_size : kOpusFrameSize;
    if (av_frame_get_buffer(frame_, 0) < 0) { if (err) *err = "frame alloc failed"; close(); return false; }

    packet_ = av_packet_alloc();
    fifo_ = av_fifo_alloc2(kOpusFrameSize * 2 * sizeof(int16_t) * 4, 1, 0);   // ~80 ms capacity, auto-grow
    nextPts_ = 0;
    packets_ = 0;
    avgEncodeMs_ = 0.0;
    return true;
}

size_t AudioEncoderOpus::bufferedFrames() const {
    return fifo_ ? av_fifo_can_read(fifo_) / (2 * sizeof(int16_t)) : 0;
}

bool AudioEncoderOpus::encode(const int16_t* samples, size_t frameCount, uint64_t timestampUs,
                              std::vector<AudioPacket>& out, std::string* err) {
    if (!ctx_) { if (err) *err = "not initialized"; return false; }
    const double t0 = nowMs();

    if (frameCount > 0 && samples) {
        if (av_fifo_write(fifo_, samples, frameCount * 2) < 0) {   // s16 pairs
            if (err) *err = "fifo write failed";
            return false;
        }
    }
    (void)timestampUs;   // packet pts derived from sample counter (drift-free)

    const size_t need = static_cast<size_t>(frame_->nb_samples) * 2;
    while (av_fifo_can_read(fifo_) >= need) {
        if (av_frame_make_writable(frame_) < 0) { if (err) *err = "frame not writable"; return false; }
        av_fifo_read(fifo_, frame_->data[0], need);
        frame_->pts = nextPts_;
        nextPts_ += frame_->nb_samples;

        if (avcodec_send_frame(ctx_, frame_) < 0) { if (err) *err = "send_frame failed"; return false; }
        while (true) {
            const int rc = avcodec_receive_packet(ctx_, packet_);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) { if (err) *err = "receive_packet failed"; return false; }
            AudioPacket ap;
            ap.data.assign(packet_->data, packet_->data + packet_->size);
            ap.ptsUs = static_cast<uint64_t>(packet_->pts) * 1'000'000ull / 48000ull;
            out.push_back(std::move(ap));
            ++packets_;
            av_packet_unref(packet_);
        }
    }

    const double dt = nowMs() - t0;
    avgEncodeMs_ = avgEncodeMs_ == 0.0 ? dt : (avgEncodeMs_ * 0.95 + dt * 0.05);
    return true;
}

// ---------------- decoder ----------------

AudioDecoderOpus::~AudioDecoderOpus() { close(); }

void AudioDecoderOpus::close() {
    if (packet_) { av_packet_free(&packet_); packet_ = nullptr; }
    if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
    if (ctx_) { avcodec_free_context(&ctx_); ctx_ = nullptr; }
}

bool AudioDecoderOpus::init(std::string* err) {
    close();
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!dec) { if (err) *err = "libopus decoder missing from this FFmpeg build"; return false; }
    ctx_ = avcodec_alloc_context3(dec);
    if (!ctx_) { if (err) *err = "alloc failed"; return false; }
    AVChannelLayout stereo;
    av_channel_layout_default(&stereo, 2);
    av_channel_layout_copy(&ctx_->ch_layout, &stereo);
    ctx_->sample_rate = 48000;
    if (avcodec_open2(ctx_, dec, nullptr) < 0) {
        if (err) *err = "avcodec_open2(opus dec) failed";
        close();
        return false;
    }
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) { if (err) *err = "alloc failed"; close(); return false; }
    return true;
}

bool AudioDecoderOpus::decode(const uint8_t* data, size_t size,
                              std::vector<int16_t>& outSamples, std::string* err) {
    if (!ctx_) { if (err) *err = "not initialized"; return false; }
    const double t0 = nowMs();

    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);
    if (avcodec_send_packet(ctx_, packet_) < 0) {
        packet_->data = nullptr; packet_->size = 0;
        if (err) *err = "send_packet rejected";
        return false;
    }
    while (true) {
        const int rc = avcodec_receive_frame(ctx_, frame_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) { if (err) *err = "receive_frame failed"; break; }
        // libopus outputs s16 (requested layout) — copy interleaved samples.
        const int ch = frame_->ch_layout.nb_channels ? frame_->ch_layout.nb_channels : 2;
        const int n = frame_->nb_samples * ch;
        const int16_t* src = reinterpret_cast<const int16_t*>(frame_->data[0]);
        outSamples.insert(outSamples.end(), src, src + n);
        av_frame_unref(frame_);
    }
    packet_->data = nullptr; packet_->size = 0;
    ++packets_;
    const double dt = nowMs() - t0;
    avgDecodeMs_ = avgDecodeMs_ == 0.0 ? dt : (avgDecodeMs_ * 0.95 + dt * 0.05);
    return true;
}

void AudioDecoderOpus::reset() {
    std::string err;
    init(&err);
}

} // namespace rp
