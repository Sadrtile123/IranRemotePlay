// RemotePlay - networking/Packet.cpp
#include "networking/Packet.h"

#include <cstring>

namespace rp::net {

namespace {

void appendU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void appendU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void appendU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

} // namespace

std::vector<uint8_t> makeFrame(uint16_t type, uint8_t flags, const uint8_t* payload,
                               uint32_t payloadSize) {
    std::vector<uint8_t> out;
    out.reserve(kFrameHeaderSize + payloadSize);
    appendU16(out, kFrameMagic);
    out.push_back(kFrameVersion);
    appendU16(out, type);
    out.push_back(flags);
    appendU32(out, payloadSize);
    if (payload && payloadSize) out.insert(out.end(), payload, payload + payloadSize);
    return out;
}

std::vector<uint8_t> makeFrame(uint16_t type, const std::vector<uint8_t>& payload) {
    return makeFrame(type, 0, payload.empty() ? nullptr : payload.data(),
                     static_cast<uint32_t>(payload.size()));
}

std::optional<FrameHeader> parseFrameHeader(const uint8_t* data, size_t size) {
    if (size < kFrameHeaderSize) return std::nullopt;
    FrameHeader h;
    h.magic = static_cast<uint16_t>(data[0] | (static_cast<uint16_t>(data[1]) << 8));
    h.version = data[2];
    h.type = static_cast<uint16_t>(data[3] | (static_cast<uint16_t>(data[4]) << 8));
    h.flags = data[5];
    h.payloadSize = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                    (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    if (!h.valid()) return std::nullopt;
    return h;
}

void ByteWriter::u8(uint8_t v) { buf_.push_back(v); }

void ByteWriter::u16(uint16_t v) { appendU16(buf_, v); }

void ByteWriter::u32(uint32_t v) { appendU32(buf_, v); }

void ByteWriter::u64(uint64_t v) { appendU64(buf_, v); }

void ByteWriter::i32(int32_t v) { appendU32(buf_, static_cast<uint32_t>(v)); }

void ByteWriter::boolean(bool v) { buf_.push_back(v ? 1 : 0); }

void ByteWriter::bytes(const uint8_t* data, size_t size) {
    if (data && size) buf_.insert(buf_.end(), data, data + size);
}

void ByteWriter::str(const std::string& s) {
    const size_t n = s.size() > 1024 ? 1024 : s.size(); // hard protocol cap
    appendU16(buf_, static_cast<uint16_t>(n));
    buf_.insert(buf_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

ByteReader::ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

ByteReader::ByteReader(const std::vector<uint8_t>& data)
    : data_(data.data()), size_(data.size()) {}

uint8_t ByteReader::u8() {
    if (!ok_ || pos_ + 1 > size_) {
        ok_ = false;
        return 0;
    }
    return data_[pos_++];
}

uint16_t ByteReader::u16() {
    if (!ok_ || pos_ + 2 > size_) {
        ok_ = false;
        return 0;
    }
    const uint16_t v = static_cast<uint16_t>(data_[pos_] | (static_cast<uint16_t>(data_[pos_ + 1]) << 8));
    pos_ += 2;
    return v;
}

uint32_t ByteReader::u32() {
    if (!ok_ || pos_ + 4 > size_) {
        ok_ = false;
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[pos_ + static_cast<size_t>(i)]) << (8 * i);
    pos_ += 4;
    return v;
}

uint64_t ByteReader::u64() {
    if (!ok_ || pos_ + 8 > size_) {
        ok_ = false;
        return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data_[pos_ + static_cast<size_t>(i)]) << (8 * i);
    pos_ += 8;
    return v;
}

int32_t ByteReader::i32() { return static_cast<int32_t>(u32()); }

bool ByteReader::boolean() { return u8() != 0; }

void ByteReader::bytes(uint8_t* out, size_t size) {
    if (!ok_ || pos_ + size > size_) {
        ok_ = false;
        return;
    }
    if (out && size) std::memcpy(out, data_ + pos_, size);
    pos_ += size;
}

std::string ByteReader::str() {
    const uint16_t n = u16();
    if (!ok_ || pos_ + n > size_) {
        ok_ = false;
        return {};
    }
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
}

} // namespace rp::net
