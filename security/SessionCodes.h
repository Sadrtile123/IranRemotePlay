// RemotePlay - security/SessionCodes.h
// Short-lived, human-transcribable session codes ("ABC7-K92P").
//
// Phase 1 role: a shared secret that (a) routes a join request to the right
// host session and (b) gates the host's ACCEPT/REJECT decision.
// Phase 12 upgrades this to a full authenticated key exchange; the code then
// becomes an entry key into that exchange.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace rp::security {

// Alphabet: 32 unambiguous characters (no I/O/0/1). One character = 5 bits of
// entropy, so an 8-character code carries 40 bits.
extern const char* const kCodeAlphabet;
extern const size_t kCodeAlphabetSize;

// Generates a fresh code in the form "XXXX-XXXX".
[[nodiscard]] std::string generateSessionCode();

// Normalizes user input: uppercases, strips separators/spaces.
// Returns the canonical "XXXX-XXXX" form, or nullopt when the input does not
// contain exactly 8 valid characters.
[[nodiscard]] std::optional<std::string> normalizeSessionCode(const std::string& input);

// Constant-time comparison so code validation does not leak how many leading
// characters matched. Both arguments must already be in canonical form
// (use normalizeSessionCode on raw user input first).
[[nodiscard]] bool constantTimeEqual(const std::string& a, const std::string& b);

// Validates a presented code against the active session code.
[[nodiscard]] bool verifySessionCode(const std::string& presented, const std::string& actual);

// Cryptographically strong random bytes for future key material.
// Windows: std::random_device is backed by rand_s (BCryptGenRandom).
[[nodiscard]] uint32_t secureRandom32();

} // namespace rp::security
