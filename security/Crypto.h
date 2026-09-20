#pragma once
// Phase 12 — crypto primitives via Windows CNG (bcrypt.dll, no external deps).
//
//   - randomBytes            BCryptGenRandom
//   - sha256 / hmacSha256    SHA-256 + HMAC via CNG
//   - hkdfSha256             RFC 5869 extract+expand (built on hmac)
//   - generateEcdhP256       ephemeral ECDH P-256 keypair (pub = 64 raw bytes)
//   - ecdhP256               shared secret (32-byte X coordinate)
//   - aesGcmEncrypt/Decrypt  AES-256-GCM (12-byte nonce, 16-byte tag)
//
// All functions fail closed (return false / empty) and never throw.

#include "../common/WinHeaders.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace rp::crypto {

constexpr size_t kAesKeyBytes = 32;    // AES-256
constexpr size_t kGcmNonceBytes = 12;
constexpr size_t kGcmTagBytes = 16;
constexpr size_t kEcdhPubBytes = 64;   // P-256: X (32) || Y (32)

[[nodiscard]] std::vector<uint8_t> randomBytes(size_t n);

[[nodiscard]] std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len);

[[nodiscard]] std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t keyLen,
                                                 const uint8_t* data, size_t dataLen);

// HKDF-SHA256 (RFC 5869): extract(ikm, salt) -> expand(info, outLen).
[[nodiscard]] std::vector<uint8_t> hkdfSha256(const std::vector<uint8_t>& ikm,
                                              const std::vector<uint8_t>& salt,
                                              const std::vector<uint8_t>& info,
                                              size_t outLen);

struct EcdhKeyPair {
    std::vector<uint8_t> publicKey;    // 64 bytes (X||Y)
    std::vector<uint8_t> privateKey;   // BCRYPT blob (opaque, do not put on wire)
};

// Generates an ephemeral P-256 keypair.
[[nodiscard]] std::optional<EcdhKeyPair> generateEcdhP256();

// ECDH shared secret from our private blob + peer public (X||Y 64 bytes).
[[nodiscard]] std::optional<std::vector<uint8_t>> ecdhP256(const EcdhKeyPair& ours,
                                                           const std::vector<uint8_t>& peerPublic);

// AES-256-GCM. `aad` is authenticated but not encrypted. On success `out`
// holds ciphertext; `tag` holds the 16-byte tag.
[[nodiscard]] bool aesGcmEncrypt(const uint8_t* key, const uint8_t* nonce,
                                 const uint8_t* plaintext, size_t len,
                                 const uint8_t* aad, size_t aadLen,
                                 std::vector<uint8_t>& out, std::array<uint8_t, kGcmTagBytes>& tag);

[[nodiscard]] bool aesGcmDecrypt(const uint8_t* key, const uint8_t* nonce,
                                 const uint8_t* ciphertext, size_t len,
                                 const uint8_t* aad, size_t aadLen,
                                 const std::array<uint8_t, kGcmTagBytes>& tag,
                                 std::vector<uint8_t>& out);

// Timing-safe comparison.
[[nodiscard]] bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n);

} // namespace rp::crypto
