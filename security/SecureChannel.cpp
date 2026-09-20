// Phase 12 — secure channel implementation. See SecureChannel.h.

#include "SecureChannel.h"

#include "../common/Log.h"

#include <algorithm>
#include <cstring>

namespace rp::crypto {

namespace {
constexpr size_t kOkmLen = 68;    // 32 + 32 + 4

std::vector<uint8_t> toVector(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
} // namespace

bool SecureChannel::finish(const EcdhKeyPair& ourKeyPair,
                           const std::vector<uint8_t>& ourSalt,
                           const std::vector<uint8_t>& peerPublicKey,
                           const std::vector<uint8_t>& peerSalt,
                           const std::string& sessionCode,
                           bool weAreHost) {
    active_ = false;
    if (peerPublicKey.size() != kEcdhPubBytes || ourSalt.size() != 16 || peerSalt.size() != 16) return false;

    const auto shared = ecdhP256(ourKeyPair, peerPublicKey);
    if (!shared || shared->size() != 32) {
        RP_ERROR() << "[secure] ECDH failed";
        return false;
    }

    // Salt = sessionCode || salts in canonical (sorted) order so both sides
    // derive identical bytes regardless of who sent which salt.
    std::vector<uint8_t> salt = toVector(sessionCode);
    const std::vector<uint8_t>& a = ourSalt;
    const std::vector<uint8_t>& b = peerSalt;
    if (a <= b) { salt.insert(salt.end(), a.begin(), a.end()); salt.insert(salt.end(), b.begin(), b.end()); }
    else        { salt.insert(salt.end(), b.begin(), b.end()); salt.insert(salt.end(), a.begin(), a.end()); }

    const std::vector<uint8_t> info{ 'R','e','m','o','t','e','P','l','a','y',' ','s','e','s','s','i','o','n',' ','v','1' };
    const std::vector<uint8_t> okm = hkdfSha256(*shared, salt, info, kOkmLen);
    if (okm.size() != kOkmLen) return false;

    std::array<uint8_t, 32> hostKey{}, clientKey{};
    std::copy(okm.begin(), okm.begin() + 32, hostKey.begin());
    std::copy(okm.begin() + 32, okm.begin() + 64, clientKey.begin());
    std::copy(okm.begin() + 64, okm.begin() + 68, nonceSalt_.begin());

    if (weAreHost) {
        sendKey_ = hostKey;
        recvKey_ = clientKey;
    } else {
        sendKey_ = clientKey;
        recvKey_ = hostKey;
    }

    // Reset replay window + counters.
    {
        std::lock_guard<std::mutex> lk(recvMutex_);
        recvHighest_ = 0;
        recvInit_ = false;
        std::memset(recvWindow_, 0, sizeof(recvWindow_));
    }
    sendCounter_.store(1);
    active_ = true;
    RP_INFO() << "[secure] session keys established (role=" << (weAreHost ? "host" : "client") << ")";
    return true;
}

bool SecureChannel::seal(const uint8_t* header, const uint8_t* payload, size_t len,
                         std::vector<uint8_t>& out) {
    if (!active_ || len > 64 * 1024) return false;
    const uint64_t counter = sendCounter_.fetch_add(1);
    if (counter == 0) return false;    // never reuse

    uint8_t nonce[kGcmNonceBytes]{};
    std::memcpy(nonce, nonceSalt_.data(), 4);
    std::memcpy(nonce + 4, &counter, 8);          // little-endian is fine (locally unique)

    std::vector<uint8_t> pt(8 + len);
    std::memcpy(pt.data(), &counter, 8);
    if (len) std::memcpy(pt.data() + 8, payload, len);

    std::array<uint8_t, kGcmTagBytes> tag{};
    std::vector<uint8_t> ct;
    if (!aesGcmEncrypt(sendKey_.data(), nonce, pt.data(), pt.size(),
                       header, 30, ct, tag)) {
        return false;
    }
    out = std::move(ct);
    out.insert(out.end(), tag.begin(), tag.end());
    ++sealed_;
    return true;
}

bool SecureChannel::open(const uint8_t* header, const uint8_t* sealed, size_t len,
                         std::vector<uint8_t>& out) {
    if (!active_ || len < 8 + kGcmTagBytes) { ++authFailures_; return false; }

    const size_t ctLen = len - kGcmTagBytes;
    const uint64_t counter = *reinterpret_cast<const uint64_t*>(sealed);
    const std::array<uint8_t, kGcmTagBytes> tag = [&] {
        std::array<uint8_t, kGcmTagBytes> t{};
        std::memcpy(t.data(), sealed + ctLen, kGcmTagBytes);
        return t;
    }();

    // Replay window check BEFORE spending cycles on decryption.
    {
        std::lock_guard<std::mutex> lk(recvMutex_);
        if (recvInit_) {
            if (counter <= recvHighest_) {
                const uint64_t back = recvHighest_ - counter;
                if (back >= 64) { ++replaysRejected_; return false; }        // too old
                const uint64_t slot = counter % 64;
                if (recvWindow_[slot] == counter) { ++replaysRejected_; return false; }  // seen
            }
        }
    }

    uint8_t nonce[kGcmNonceBytes]{};
    std::memcpy(nonce, nonceSalt_.data(), 4);
    std::memcpy(nonce + 4, &counter, 8);

    std::vector<uint8_t> pt;
    if (!aesGcmDecrypt(recvKey_.data(), nonce, sealed, ctLen, header, 30, tag, pt)) {
        ++authFailures_;
        return false;
    }
    if (pt.size() < 8) { ++authFailures_; return false; }

    // Verify the inner counter matches (defends against nonce manipulation).
    uint64_t inner;
    std::memcpy(&inner, pt.data(), 8);
    if (inner != counter) { ++authFailures_; return false; }

    // Mark seen.
    {
        std::lock_guard<std::mutex> lk(recvMutex_);
        if (!recvInit_) { recvInit_ = true; recvHighest_ = counter; }
        else if (counter > recvHighest_) recvHighest_ = counter;
        recvWindow_[counter % 64] = counter;
    }

    out.assign(pt.begin() + 8, pt.end());
    ++opened_;
    return true;
}

std::vector<uint8_t> SecureChannel::sendKeyForTest() const {
    return std::vector<uint8_t>(sendKey_.begin(), sendKey_.end());
}

} // namespace rp::crypto
