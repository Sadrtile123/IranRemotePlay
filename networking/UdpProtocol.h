#pragma once
// Phase 4 — UDP media protocol: datagram header, fragmentation, reassembly,
// sequence-loss tracking and the adaptive jitter buffer.
//
// Datagram layout (docs/PROTOCOL.md section 5, committed in Phase 1):
//   u16 magic 0x5252 | u8 version 0x01 | u8 type | u8 flags | u32 sessionId
//   u64 sequence | u16 fragmentIndex | u16 fragmentCount | u64 timestampNs
//   u16 payloadSize | payload
//
// Pure logic, no sockets: unit-testable on every platform.

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace rp::net {

enum class UdpType : uint8_t {
    Video = 0,
    Audio = 1,
    Input = 2,
    Control = 3,
    Ping = 4,
    Pong = 5,
    KeyframeRequest = 6,
    StreamStart = 7,
    StreamStop = 8,
};

enum class UdpFlag : uint8_t {
    Fragment = 0x01,     // this datagram carries one fragment of a larger frame
    Keyframe = 0x02,     // frame is a keyframe (video)
    Encrypted = 0x04,    // payload encrypted (Phase 12)
};

constexpr uint16_t kUdpMagic = 0x5252;
constexpr uint8_t  kUdpVersion = 0x01;
constexpr size_t   kUdpHeaderSize = 2 + 1 + 1 + 1 + 4 + 8 + 2 + 2 + 8 + 2;  // = 30
constexpr size_t   kMaxUdpPayload = 1100;      // fragment payload bytes (datagram stays <= ~1130, no IP fragmentation)
constexpr uint16_t kMaxFragments = 1024;       // hard cap: ~1.1 MB frame

struct UdpHeader {
    uint16_t magic = kUdpMagic;
    uint8_t  version = kUdpVersion;
    uint8_t  type = 0;
    uint8_t  flags = 0;
    uint32_t sessionId = 0;
    uint64_t sequence = 0;       // per-type monotonic frame/packet number
    uint16_t fragmentIndex = 0;
    uint16_t fragmentCount = 1;
    uint64_t timestampNs = 0;    // capture timestamp (A/V sync)
    uint16_t payloadSize = 0;
};

// Encodes the fixed header into `buf` (must have kUdpHeaderSize bytes).
void encodeUdpHeader(uint8_t* buf, const UdpHeader& h);

// Decodes and validates (magic, version, payload size fits datagram).
// Returns false for malformed input.
[[nodiscard]] bool decodeUdpHeader(const uint8_t* buf, size_t datagramSize, UdpHeader& out);

// How many fragments a payload of `size` bytes needs.
[[nodiscard]] uint16_t fragmentCountFor(size_t size);

// A fully received frame (reassembled).
struct AssembledFrame {
    UdpType  type = UdpType::Video;
    uint32_t sessionId = 0;
    uint64_t sequence = 0;
    uint64_t timestampNs = 0;
    bool     keyframe = false;
    std::vector<uint8_t> data;
};

// ---------------------------------------------------------------
// SequenceTracker — per-type loss/reorder accounting.
// Detects gaps, duplicates and late packets by sequence number.
class SequenceTracker {
public:
    // Records an incoming sequence number. Returns:
    //   0 = in-order or first;  1 = duplicate;  N(>1) = gap of N-1 lost frames
    //   -1 = late (below the replay window bottom, counted as lost-then-late)
    [[nodiscard]] int64_t track(uint64_t sequence);

    uint64_t highest() const { return highest_; }
    uint64_t duplicates() const { return duplicates_; }
    uint64_t lost() const { return lost_; }
    uint64_t late() const { return late_; }
    uint64_t received() const { return received_; }
    bool     initialized() const { return initialized_; }
    void     reset();

private:
    uint64_t highest_ = 0;
    bool     initialized_ = false;
    uint64_t duplicates_ = 0;
    uint64_t lost_ = 0;
    uint64_t late_ = 0;
    uint64_t received_ = 0;
    // Recently seen sequences below `highest_` (distinguish reorder vs duplicate).
    uint64_t seenWindow_[64] = {};
    uint64_t seenMask_ = 0;        // bitmask of filled slots
    static constexpr uint64_t kLateWindow = 64;
};

// ---------------------------------------------------------------
// FragmentReassembler — turns fragments into AssembledFrames.
// Tracks one stream (sessionId+type). Discards stale/duplicate fragments.
class FragmentReassembler {
public:
    explicit FragmentReassembler(UdpType type) : type_(type) {}

    // Feed one fragment. Returns true when this call completed a frame.
    [[nodiscard]] bool feed(const UdpHeader& h, const uint8_t* payload, AssembledFrame& out);

    // Completes without further fragments? No: incomplete frames are dropped
    // by age. Removes buffers older than `ageFrames` below highestStarted.
    void gc();

    size_t inFlight() const { return frames_.size(); }
    uint64_t droppedIncomplete() const { return droppedIncomplete_; }
    uint64_t completedFrames() const { return completed_; }

    void reset();

private:
    struct FrameBuf {
        std::vector<uint8_t> data;
        std::vector<bool>    have;
        uint16_t             received = 0;
        uint16_t             fragmentCount = 0;
        size_t               totalSize = 0;   // exact frame size (set when the last fragment arrives)
        uint64_t             timestampNs = 0;
        bool                 keyframe = false;
    };
    UdpType type_;
    std::map<uint64_t, FrameBuf> frames_;    // sequence -> buffer
    uint64_t highestStarted_ = 0;
    bool     initialized_ = false;
    uint64_t droppedIncomplete_ = 0;
    uint64_t completed_ = 0;
    static constexpr size_t kMaxInFlight = 96;
};

// ---------------------------------------------------------------
// JitterBuffer — holds completed frames briefly to absorb reordering
// and network jitter before decoding. Target delay adapts upward on
// observed reordering and decays slowly when stable (bounded).
class JitterBuffer {
public:
    explicit JitterBuffer(unsigned initialTargetMs = 30);

    void push(AssembledFrame&& frame, uint64_t nowNs);

    // Pops frames ready for display: waited >= targetMs OR buffer over high
    // watermark OR stop-to-drain mode. Frames pop in sequence order.
    void pop(std::vector<AssembledFrame>& out, uint64_t nowNs);

    void setTargetMs(unsigned ms) { targetMs_ = ms; }
    unsigned targetMs() const { return targetMs_; }
    size_t size() const { return queue_.size(); }
    uint64_t reordered() const { return reordered_; }
    uint64_t overflowDropped() const { return overflowDropped_; }

    void reset();

private:
    struct Entry { AssembledFrame frame; uint64_t arriveNs = 0; };
    std::deque<Entry> queue_;
    unsigned targetMs_;
    uint64_t reordered_ = 0;
    uint64_t overflowDropped_ = 0;
    uint64_t lastPoppedSequence_ = 0;
    bool     initialized_ = false;
    static constexpr size_t kHighWatermark = 8;    // frames
    static constexpr unsigned kMaxTargetMs = 120;
};

} // namespace rp::net
