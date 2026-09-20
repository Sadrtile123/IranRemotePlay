// Phase 4 — UDP media protocol logic. See UdpProtocol.h.

#include <utility>
#include <vector>
#include "UdpProtocol.h"

#include <cstring>

namespace rp::net {

namespace {
inline void putU16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline void putU32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void putU64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }
inline uint16_t getU16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t getU32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t getU64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
} // namespace

void encodeUdpHeader(uint8_t* buf, const UdpHeader& h) {
    putU16(buf + 0, h.magic);
    buf[2] = h.version;
    buf[3] = h.type;
    buf[4] = h.flags;
    putU32(buf + 5, h.sessionId);
    putU64(buf + 9, h.sequence);
    putU16(buf + 17, h.fragmentIndex);
    putU16(buf + 19, h.fragmentCount);
    putU64(buf + 21, h.timestampNs);
    putU16(buf + 29, h.payloadSize);
}

bool decodeUdpHeader(const uint8_t* buf, size_t datagramSize, UdpHeader& out) {
    if (!buf || datagramSize < kUdpHeaderSize) return false;
    out.magic = getU16(buf + 0);
    if (out.magic != kUdpMagic) return false;
    out.version = buf[2];
    if (out.version != kUdpVersion) return false;
    out.type = buf[3];
    if (out.type > 8) return false;
    out.flags = buf[4];
    out.sessionId = getU32(buf + 5);
    out.sequence = getU64(buf + 9);
    out.fragmentIndex = getU16(buf + 17);
    out.fragmentCount = getU16(buf + 19);
    out.timestampNs = getU64(buf + 21);
    out.payloadSize = getU16(buf + 29);
    if (datagramSize < kUdpHeaderSize + out.payloadSize) return false;
    if (out.fragmentCount == 0 || out.fragmentCount > kMaxFragments) return false;
    if (out.fragmentIndex >= out.fragmentCount) return false;
    // Single-fragment datagrams must be marked consistent (index 0, count 1) —
    // receivers treat any count>1 as fragmented regardless of flags bit.
    return true;
}

uint16_t fragmentCountFor(size_t size) {
    if (size == 0) return 1;
    const uint16_t n = static_cast<uint16_t>((size + kMaxUdpPayload - 1) / kMaxUdpPayload);
    return n < 1 ? 1 : n;
}

// ---------------------------------------------------------------

int64_t SequenceTracker::track(uint64_t seq) {
    ++received_;
    const uint64_t slot = seq % 64;
    if (!initialized_) {
        initialized_ = true;
        highest_ = seq;
        seenWindow_[slot] = seq;
        seenMask_ |= (1ull << slot);
        return 0;
    }
    if (seq > highest_) {
        const uint64_t gap = seq - highest_;          // >= 1
        if (gap > 1) lost_ += gap - 1;
        highest_ = seq;
        seenWindow_[slot] = seq;
        seenMask_ |= (1ull << slot);
        return static_cast<int64_t>(gap);
    }
    // At or below the newest sequence.
    if (seq + kLateWindow < highest_) { ++late_; return -1; }
    if (((seenMask_ >> slot) & 1ull) && seenWindow_[slot] == seq) {
        ++duplicates_;                                // exact duplicate seen before
        return 1;
    }
    seenWindow_[slot] = seq;                          // reordered first arrival
    seenMask_ |= (1ull << slot);
    ++late_;
    return 0;
}

void SequenceTracker::reset() { *this = SequenceTracker{}; }

// ---------------------------------------------------------------

bool FragmentReassembler::feed(const UdpHeader& h, const uint8_t* payload, AssembledFrame& out) {
    if (h.fragmentCount == 1) {
        // Unfragmented frame completes immediately.
        out.type = static_cast<UdpType>(h.type);
        out.sessionId = h.sessionId;
        out.sequence = h.sequence;
        out.timestampNs = h.timestampNs;
        out.keyframe = (h.flags & static_cast<uint8_t>(UdpFlag::Keyframe)) != 0;
        out.data.assign(payload, payload + h.payloadSize);
        ++completed_;
        return true;
    }

    // Track started sequences for GC.
    if (!initialized_ || h.sequence > highestStarted_) highestStarted_ = h.sequence;
    initialized_ = true;

    FrameBuf& fb = frames_[h.sequence];
    if (fb.fragmentCount == 0) {                  // new buffer
        if (frames_.size() > kMaxInFlight) {      // too many in flight: drop oldest
            frames_.erase(frames_.begin());
            ++droppedIncomplete_;
        }
        fb.fragmentCount = h.fragmentCount;
        fb.have.assign(h.fragmentCount, false);
        fb.data.assign(static_cast<size_t>(h.fragmentCount) * kMaxUdpPayload, 0);
        fb.timestampNs = h.timestampNs;
        fb.keyframe = (h.flags & static_cast<uint8_t>(UdpFlag::Keyframe)) != 0;
    } else if (fb.fragmentCount != h.fragmentCount) {
        return false;                             // corrupt / spoofed
    }
    if (h.fragmentIndex >= fb.fragmentCount || fb.have[h.fragmentIndex]) {
        return false;                             // duplicate fragment
    }

    const size_t off = static_cast<size_t>(h.fragmentIndex) * kMaxUdpPayload;
    std::memcpy(fb.data.data() + off, payload, h.payloadSize);
    fb.have[h.fragmentIndex] = true;
    ++fb.received;
    if (h.fragmentIndex + 1 == h.fragmentCount) {
        fb.totalSize = static_cast<size_t>(h.fragmentCount - 1) * kMaxUdpPayload + h.payloadSize;
    }

    if (fb.received == fb.fragmentCount) {
        if (fb.totalSize == 0 || fb.totalSize > fb.data.size()) {
            frames_.erase(h.sequence);
            ++droppedIncomplete_;
            return false;
        }
        out.type = static_cast<UdpType>(h.type);
        out.sessionId = h.sessionId;
        out.sequence = h.sequence;
        out.timestampNs = fb.timestampNs;
        out.keyframe = fb.keyframe;
        out.data.assign(fb.data.begin(), fb.data.begin() + static_cast<std::ptrdiff_t>(fb.totalSize));
        frames_.erase(h.sequence);
        ++completed_;
        return true;
    }
    return false;
}

void FragmentReassembler::gc() {
    if (frames_.empty()) return;
    // Drop buffers that are 64+ sequences behind the newest started frame.
    while (!frames_.empty()) {
        const uint64_t lowest = frames_.begin()->first;
        if (highestStarted_ > lowest + 64) {
            frames_.erase(frames_.begin());
            ++droppedIncomplete_;
        } else {
            break;
        }
    }
}

void FragmentReassembler::reset() { frames_.clear(); highestStarted_ = 0; initialized_ = false; }

// ---------------------------------------------------------------

JitterBuffer::JitterBuffer(unsigned initialTargetMs) : targetMs_(initialTargetMs) {}

void JitterBuffer::push(AssembledFrame&& frame, uint64_t nowNs) {
    if (initialized_ && frame.sequence <= lastPoppedSequence_ && lastPoppedSequence_ > 0) {
        ++reordered_;                              // too late: frame after we already moved past its slot
        return;
    }
    while (queue_.size() >= kHighWatermark * 2) {
        ++overflowDropped_;
        queue_.pop_front();                        // extreme backlog: drop oldest
    }
    queue_.push_back(Entry{ std::move(frame), nowNs });
    // Keep sequence order (push is nearly always in order; insert for the rare reorder).
    for (size_t i = queue_.size() - 1; i > 0; --i) {
        if (queue_[i].frame.sequence < queue_[i - 1].frame.sequence) std::swap(queue_[i], queue_[i - 1]);
        else break;
    }
}

void JitterBuffer::pop(std::vector<AssembledFrame>& out, uint64_t nowNs) {
    while (!queue_.empty()) {
        Entry& front = queue_.front();
        const uint64_t waited = nowNs > front.arriveNs ? nowNs - front.arriveNs : 0;
        const bool waitedEnough = waited >= static_cast<uint64_t>(targetMs_) * 1'000'000ull;
        const bool overflow = queue_.size() > kHighWatermark;
        const uint64_t headSequence = front.frame.sequence;
        const bool gapAhead = initialized_ && headSequence > lastPoppedSequence_ + 1;
        if (waitedEnough || overflow || (gapAhead && waited >= static_cast<uint64_t>(targetMs_) / 2)) {
            if (gapAhead) { /* skipping lost frames: counted by SequenceTracker */ }
            out.push_back(std::move(front.frame));
            lastPoppedSequence_ = headSequence;
            initialized_ = true;
            queue_.pop_front();
        } else {
            break;
        }
    }
}

void JitterBuffer::reset() { queue_.clear(); reordered_ = 0; overflowDropped_ = 0; lastPoppedSequence_ = 0; initialized_ = false; }

} // namespace rp::net
