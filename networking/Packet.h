// RemotePlay - networking/Packet.h
// TCP control-channel framing and little-endian byte serialization primitives.
//
// Frame layout (10 bytes header, all little-endian):
//   offset 0  : uint16 magic       0x5250 ("RP")
//   offset 2  : uint8  version     kProtocolVersion (1)
//   offset 3  : uint16 messageType protocol::Id value
//   offset 5  : uint8  flags       reserved (0)
//   offset 6  : uint32 payloadSize number of payload bytes that follow
//
// The UDP media datagram format (video/audio/input, Phase 4) is documented in
// docs/PROTOCOL.md and intentionally not implemented yet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rp::net {

inline constexpr uint16_t kFrameMagic = 0x5250;   // "RP"
inline constexpr uint8_t kFrameVersion = 1;
inline constexpr size_t kFrameHeaderSize = 10;
// Control payloads are small by design; anything larger is treated as a
// protocol violation and drops the connection.
inline constexpr uint32_t kMaxControlPayload = 64 * 1024;

struct FrameHeader {
    uint16_t magic = 0;
    uint8_t version = 0;
    uint16_t type = 0;
    uint8_t flags = 0;
    uint32_t payloadSize = 0;

    [[nodiscard]] bool valid() const {
        return magic == kFrameMagic && version == kFrameVersion &&
               payloadSize <= kMaxControlPayload;
    }
};

// Build header + payload into one contiguous buffer.
[[nodiscard]] std::vector<uint8_t> makeFrame(uint16_t type, uint8_t flags,
                                             const uint8_t* payload, uint32_t payloadSize);
[[nodiscard]] std::vector<uint8_t> makeFrame(uint16_t type, const std::vector<uint8_t>& payload);

// Parses and validates a 10-byte header.
[[nodiscard]] std::optional<FrameHeader> parseFrameHeader(const uint8_t* data, size_t size);

// ---------------------------------------------------------------------------
// Little-endian byte-level serialization shared by the protocol layer.
// All read methods are bounds-checked; on failure ok() turns false and all
// further reads return zero values.
// ---------------------------------------------------------------------------
class ByteWriter {
public:
    ByteWriter() = default;

    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void i32(int32_t v);
    void boolean(bool v);
    void bytes(const uint8_t* data, size_t size);
    // UTF-8 string as uint16 length + raw bytes (length-capped).
    void str(const std::string& s);

    [[nodiscard]] const std::vector<uint8_t>& data() const { return buf_; }
    [[nodiscard]] std::vector<uint8_t> take() { return std::move(buf_); }
    [[nodiscard]] size_t size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size);
    explicit ByteReader(const std::vector<uint8_t>& data);

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int32_t i32();
    bool boolean();
    void bytes(uint8_t* out, size_t size);
    // Reads uint16 length + bytes; on length mismatch marks the reader failed.
    std::string str();

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] size_t remaining() const { return size_ - pos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace rp::net
