// RemotePlay - security/SessionCodes.cpp
#include "security/SessionCodes.h"

#include "common/Log.h"

#include <cstdint>
#include <random>

namespace rp::security {

const char* const kCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const size_t kCodeAlphabetSize = 32; // 24 letters + 8 digits

uint32_t secureRandom32() {
    // std::random_device on MSVC is backed by rand_s (BCryptGenRandom) and on
    // libstdc++ by /dev/urandom: both are cryptographically strong sources.
    static std::random_device rd;
    return rd();
}

namespace {

// Uniform characters from the 32-symbol alphabet. 32 divides 256, so every
// byte maps without modulo bias.
std::string randomCodeChars(size_t count) {
    std::string out;
    out.reserve(count);
    uint32_t buf = 0;
    unsigned bits = 0;
    while (out.size() < count) {
        if (bits < 8) {
            buf = secureRandom32();
            bits = 32;
        }
        const uint8_t byte = static_cast<uint8_t>(buf & 0xFF);
        buf >>= 8;
        bits -= 8;
        out.push_back(kCodeAlphabet[byte & (kCodeAlphabetSize - 1)]);
    }
    return out;
}

} // namespace

std::string generateSessionCode() {
    return randomCodeChars(4) + "-" + randomCodeChars(4);
}

std::optional<std::string> normalizeSessionCode(const std::string& input) {
    std::string compact;
    compact.reserve(input.size());
    for (char ch : input) {
        if (ch == '-' || ch == ' ') continue;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        compact.push_back(ch);
    }
    if (compact.size() != 8) return std::nullopt;
    for (char ch : compact) {
        bool found = false;
        for (size_t i = 0; i < kCodeAlphabetSize; ++i) {
            if (kCodeAlphabet[i] == ch) {
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    return compact.substr(0, 4) + "-" + compact.substr(4, 4);
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false; // length is not secret
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<uint8_t>(diff | (static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i])));
    }
    return diff == 0;
}

bool verifySessionCode(const std::string& presented, const std::string& actual) {
    const auto p = normalizeSessionCode(presented);
    const auto a = normalizeSessionCode(actual);
    if (!p || !a) return false;
    return constantTimeEqual(*p, *a);
}

} // namespace rp::security
