#pragma once
// Portable crypto sink interface for the UDP transport (Phase 12).
// The Windows CNG implementation lives in security/SecureChannel.h; keeping
// the interface here lets UdpTransport stay portable and testable everywhere.

#include <cstdint>
#include <vector>

namespace rp::net {

class UdpCryptoSink {
public:
    virtual ~UdpCryptoSink() = default;

    // Seals one datagram payload. `header` = the 30-byte UDP header (AAD).
    // Returns false -> the datagram must NOT be sent.
    [[nodiscard]] virtual bool seal(const uint8_t* header, const uint8_t* payload, size_t len,
                                    std::vector<uint8_t>& out) = 0;

    // Opens a sealed payload. Returns false -> drop the datagram (fail closed).
    [[nodiscard]] virtual bool open(const uint8_t* header, const uint8_t* sealed, size_t len,
                                    std::vector<uint8_t>& out) = 0;

    [[nodiscard]] virtual bool active() const = 0;
};

} // namespace rp::net
