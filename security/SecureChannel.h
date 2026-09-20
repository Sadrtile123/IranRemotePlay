#pragma once
// Phase 12 — per-session secure channel for the UDP media path.
//
// Key schedule (authenticated by the 40-bit session code):
//   1. Both sides generate an ephemeral ECDH P-256 keypair + 16 random salt bytes.
//   2. Public keys + salts travel in the TCP KEY_EXCHANGE message (after host approval).
//   3. shared = ECDH(ourPriv, peerPub)
//      okm    = HKDF-SHA256(shared, salt = sessionCode || salts(sorted), info = "RemotePlay session v1", 68)
//      hostKey = okm[0..32], clientKey = okm[32..64], nonceSalt = okm[64..68]
//      -> send/recv keys by role. A man-in-the-middle who does not know the
//         session code cannot produce the same keys, so every datagram fails
//         authentication (fail-closed).
//
// Datagram sealing (payload only; the 30-byte UDP header stays clear so the
// Phase 14 relay can route by sessionId, and the header is bound as AAD):
//   sealed = u64 counter || AES-256-GCM(clearPayload, nonce = nonceSalt||counter, aad = header)
//   tag appended at the end of the ciphertext (16 bytes).
// The receive side enforces a 64-entry sliding replay window on the counter.

#include "Crypto.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace rp::crypto {

class SecureChannel {
public:
    SecureChannel() = default;

    // Completes the key schedule. `weAreHost` selects send/recv key roles.
    // Returns false (and leaves the channel inactive) on any crypto failure.
    [[nodiscard]] bool finish(const EcdhKeyPair& ourKeyPair,
                              const std::vector<uint8_t>& ourSalt,
                              const std::vector<uint8_t>& peerPublicKey,
                              const std::vector<uint8_t>& peerSalt,
                              const std::string& sessionCode,
                              bool weAreHost);

    [[nodiscard]] bool active() const { return active_; }

    // Seals one datagram payload. `header` = the 30-byte UDP header (AAD).
    // Output sealed payload = 8 (counter) + ciphertext + 16 (tag).
    [[nodiscard]] bool seal(const uint8_t* header, const uint8_t* payload, size_t len,
                            std::vector<uint8_t>& out);

    // Opens a sealed payload. Fails closed on tag mismatch or replay.
    [[nodiscard]] bool open(const uint8_t* header, const uint8_t* sealed, size_t len,
                            std::vector<uint8_t>& out);

    [[nodiscard]] uint64_t packetsSealed() const { return sealed_.load(); }
    [[nodiscard]] uint64_t packetsOpened() const { return opened_.load(); }
    [[nodiscard]] uint64_t authFailures() const { return authFailures_.load(); }
    [[nodiscard]] uint64_t replaysRejected() const { return replaysRejected_.load(); }

    // For tests: inspect derived keys.
    [[nodiscard]] std::vector<uint8_t> sendKeyForTest() const;

private:
    bool active_ = false;
    std::array<uint8_t, 32> sendKey_{};
    std::array<uint8_t, 32> recvKey_{};
    std::array<uint8_t, 4> nonceSalt_{};

    std::atomic<uint64_t> sendCounter_{ 1 };

    // Receive replay window.
    std::mutex recvMutex_;
    uint64_t recvHighest_ = 0;
    bool recvInit_ = false;
    uint64_t recvWindow_[64]{};      // seen counters, modulus 64

    std::atomic<uint64_t> sealed_{ 0 };
    std::atomic<uint64_t> opened_{ 0 };
    std::atomic<uint64_t> authFailures_{ 0 };
    std::atomic<uint64_t> replaysRejected_{ 0 };
};

} // namespace rp::crypto
