// Phase 12 — CNG crypto implementation. See Crypto.h.

#include <array>
#include <optional>
#include <utility>
#include <vector>
#include "Crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

#ifdef _MSC_VER
#pragma comment(lib, "bcrypt.lib")
#endif

namespace rp::crypto {
namespace {

// ---- tiny RAII guards ----
struct AlgHandle {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~AlgHandle() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};
struct KeyHandle {
    BCRYPT_KEY_HANDLE h = nullptr;
    ~KeyHandle() { if (h) BCryptDestroyKey(h); }
};
struct SecretHandle {
    BCRYPT_SECRET_HANDLE h = nullptr;
    ~SecretHandle() { if (h) BCryptDestroySecret(h); }
};

bool openAlgorithm(AlgHandle& alg, LPCWSTR id, ULONG flags = 0) {
    const NTSTATUS st = BCryptOpenAlgorithmProvider(&alg.h, id, nullptr, flags);
    return BCRYPT_SUCCESS(st);
}

// SHA-256 helper (one-shot)
bool sha256Impl(const uint8_t* data, size_t len, uint8_t out[32]) {
    AlgHandle alg;
    if (!openAlgorithm(alg, BCRYPT_SHA256_ALGORITHM)) return false;
    BCRYPT_HASH_HANDLE hh = nullptr;
    NTSTATUS st = BCryptCreateHash(alg.h, &hh, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(st)) return false;
    st = BCryptHashData(hh, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0);
    if (BCRYPT_SUCCESS(st)) {
        st = BCryptFinishHash(hh, out, 32, 0);
    }
    BCryptDestroyHash(hh);
    return BCRYPT_SUCCESS(st);
}

bool hmacImpl(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen, uint8_t out[32]) {
    AlgHandle alg;
    if (!openAlgorithm(alg, BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG)) return false;
    BCRYPT_HASH_HANDLE hh = nullptr;
    NTSTATUS st = BCryptCreateHash(alg.h, &hh, nullptr, 0,
                                   const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0);
    if (!BCRYPT_SUCCESS(st)) return false;
    st = BCryptHashData(hh, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0);
    if (BCRYPT_SUCCESS(st)) {
        st = BCryptFinishHash(hh, out, 32, 0);
    }
    BCryptDestroyHash(hh);
    return BCRYPT_SUCCESS(st);
}

} // namespace

std::vector<uint8_t> randomBytes(size_t n) {
    std::vector<uint8_t> out(n);
    if (n == 0) return out;
    NTSTATUS st = BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st)) out.clear();
    return out;
}

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len) {
    std::array<uint8_t, 32> out{};
    sha256Impl(data, len, out.data());
    return out;
}

std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen) {
    std::array<uint8_t, 32> out{};
    hmacImpl(key, keyLen, data, dataLen, out.data());
    return out;
}

std::vector<uint8_t> hkdfSha256(const std::vector<uint8_t>& ikm,
                                const std::vector<uint8_t>& salt,
                                const std::vector<uint8_t>& info,
                                size_t outLen) {
    std::vector<uint8_t> out;
    if (outLen == 0 || outLen > 255 * 32) return out;

    // extract: PRK = HMAC(salt, ikm)
    std::array<uint8_t, 32> prk = hmacSha256(salt.data(), salt.size(), ikm.data(), ikm.size());

    // expand: T(i) = HMAC(PRK, T(i-1) || info || byte(i))
    out.reserve(outLen);
    std::array<uint8_t, 32> t{};
    size_t tLen = 0;
    for (uint8_t counter = 1; out.size() < outLen; ++counter) {
        std::vector<uint8_t> input;
        input.reserve(tLen + info.size() + 1);
        input.insert(input.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(tLen));
        input.insert(input.end(), info.begin(), info.end());
        input.push_back(counter);
        t = hmacSha256(prk.data(), prk.size(), input.data(), input.size());
        tLen = 32;
        const size_t take = std::min<size_t>(32, outLen - out.size());
        out.insert(out.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return out;
}

std::optional<EcdhKeyPair> generateEcdhP256() {
    AlgHandle alg;
    if (!openAlgorithm(alg, BCRYPT_ECDH_P256_ALGORITHM)) return std::nullopt;

    BCRYPT_KEY_HANDLE kh = nullptr;
    NTSTATUS st = BCryptGenerateKeyPair(alg.h, &kh, 0, 0);
    if (!BCRYPT_SUCCESS(st)) return std::nullopt;
    st = BCryptFinalizeKeyPair(kh, 0);
    if (!BCRYPT_SUCCESS(st)) { BCryptDestroyKey(kh); return std::nullopt; }

    EcdhKeyPair kp;
    // Public: BCRYPT_ECCPUBLIC_BLOB = ECCKEY_BLOB header (8) + X(32) + Y(32).
    ULONG pubLen = 0;
    st = BCryptExportKey(kh, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &pubLen, 0);
    if (BCRYPT_SUCCESS(st) && pubLen == 8 + kEcdhPubBytes) {
        std::vector<uint8_t> blob(pubLen);
        st = BCryptExportKey(kh, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), pubLen, &pubLen, 0);
        if (BCRYPT_SUCCESS(st)) {
            kp.publicKey.assign(blob.begin() + 8, blob.end());
        }
    }
    // Private: keep the full BCRYPT_ECCPRIVATE_BLOB (opaque).
    ULONG privLen = 0;
    st = BCryptExportKey(kh, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &privLen, 0);
    if (BCRYPT_SUCCESS(st) && privLen > 0) {
        std::vector<uint8_t> blob(privLen);
        st = BCryptExportKey(kh, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), privLen, &privLen, 0);
        if (BCRYPT_SUCCESS(st)) kp.privateKey = std::move(blob);
    }
    BCryptDestroyKey(kh);

    if (kp.publicKey.size() != kEcdhPubBytes || kp.privateKey.empty()) return std::nullopt;
    return kp;
}

std::optional<std::vector<uint8_t>> ecdhP256(const EcdhKeyPair& ours, const std::vector<uint8_t>& peerPublic) {
    if (peerPublic.size() != kEcdhPubBytes || ours.privateKey.empty()) return std::nullopt;

    AlgHandle alg;
    if (!openAlgorithm(alg, BCRYPT_ECDH_P256_ALGORITHM)) return std::nullopt;

    NTSTATUS st = 0;
    // Import our private key.
    BCRYPT_KEY_HANDLE privKey = nullptr;
    st = BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, &privKey,
                             const_cast<PUCHAR>(ours.privateKey.data()),
                             static_cast<ULONG>(ours.privateKey.size()), 0);
    if (!BCRYPT_SUCCESS(st)) return std::nullopt;

    // Import peer public (rebuild ECCPUBLIC_BLOB with the 8-byte header).
    std::vector<uint8_t> pubBlob(8 + kEcdhPubBytes);
    {
        ULONG magic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
        std::memcpy(pubBlob.data(), &magic, 4);
        ULONG cb = static_cast<ULONG>(kEcdhPubBytes / 2);     // cbKey = 32 (size of X)
        std::memcpy(pubBlob.data() + 4, &cb, 4);
        std::memcpy(pubBlob.data() + 8, peerPublic.data(), kEcdhPubBytes);
    }
    BCRYPT_KEY_HANDLE peerKey = nullptr;
    st = BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peerKey,
                             pubBlob.data(), static_cast<ULONG>(pubBlob.size()), 0);
    if (!BCRYPT_SUCCESS(st)) { BCryptDestroyKey(privKey); return std::nullopt; }

    SecretHandle secret;
    st = BCryptSecretAgreement(privKey, peerKey, &secret.h, 0);
    BCryptDestroyKey(privKey);
    BCryptDestroyKey(peerKey);
    if (!BCRYPT_SUCCESS(st) || !secret.h) return std::nullopt;

    // Derive 32 raw bytes (the X coordinate of the shared point).
    std::vector<uint8_t> out(32);
    ULONG derived = 0;
    st = BCryptDeriveKey(secret.h, BCRYPT_KDF_RAW_SECRET, nullptr, out.data(), 32, &derived, 0);
    if (!BCRYPT_SUCCESS(st) || derived != 32) return std::nullopt;
    // CNG returns the raw secret little-endian; normalize to big-endian.
    std::reverse(out.begin(), out.end());
    return out;
}

bool aesGcmEncrypt(const uint8_t* key, const uint8_t* nonce,
                   const uint8_t* plaintext, size_t len,
                   const uint8_t* aad, size_t aadLen,
                   std::vector<uint8_t>& out, std::array<uint8_t, kGcmTagBytes>& tag) {
    AlgHandle alg;
    if (!openAlgorithm(alg, BCRYPT_AES_ALGORITHM, false)) return false;
    NTSTATUS st = BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE,
                                    reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(BCRYPT_CHAIN_MODE_GCM)),
                                    sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(st)) return false;

    KeyHandle kh;
    st = BCryptGenerateSymmetricKey(alg.h, &kh.h, nullptr, 0,
                                    const_cast<PUCHAR>(key), kAesKeyBytes, 0);
    if (!BCRYPT_SUCCESS(st)) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = const_cast<PUCHAR>(nonce);
    auth.cbNonce = kGcmNonceBytes;
    auth.pbAuthData = const_cast<PUCHAR>(aad);
    auth.cbAuthData = static_cast<ULONG>(aadLen);
    auth.pbTag = tag.data();
    auth.cbTag = kGcmTagBytes;

    out.assign(len, 0);
    ULONG written = 0;
    st = BCryptEncrypt(kh.h, const_cast<PUCHAR>(plaintext), static_cast<ULONG>(len), &auth,
                       nullptr, 0, out.data(), static_cast<ULONG>(out.size()), &written, 0);
    if (!BCRYPT_SUCCESS(st)) { out.clear(); return false; }
    out.resize(written);
    return true;
}

bool aesGcmDecrypt(const uint8_t* key, const uint8_t* nonce,
                   const uint8_t* ciphertext, size_t len,
                   const uint8_t* aad, size_t aadLen,
                   const std::array<uint8_t, kGcmTagBytes>& tag,
                   std::vector<uint8_t>& out) {
    AlgHandle alg;
    if (!openAlgorithm(alg, BCRYPT_AES_ALGORITHM, false)) return false;
    NTSTATUS st = BCryptSetProperty(alg.h, BCRYPT_CHAINING_MODE,
                                    reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(BCRYPT_CHAIN_MODE_GCM)),
                                    sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    if (!BCRYPT_SUCCESS(st)) return false;

    KeyHandle kh;
    st = BCryptGenerateSymmetricKey(alg.h, &kh.h, nullptr, 0,
                                    const_cast<PUCHAR>(key), kAesKeyBytes, 0);
    if (!BCRYPT_SUCCESS(st)) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = const_cast<PUCHAR>(nonce);
    auth.cbNonce = kGcmNonceBytes;
    auth.pbAuthData = const_cast<PUCHAR>(aad);
    auth.cbAuthData = static_cast<ULONG>(aadLen);
    auth.pbTag = const_cast<PUCHAR>(tag.data());
    auth.cbTag = kGcmTagBytes;

    out.assign(len, 0);
    ULONG written = 0;
    st = BCryptDecrypt(kh.h, const_cast<PUCHAR>(ciphertext), static_cast<ULONG>(len), &auth,
                       nullptr, 0, out.data(), static_cast<ULONG>(out.size()), &written, 0);
    if (!BCRYPT_SUCCESS(st)) { out.clear(); return false; }
    out.resize(written);
    return true;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace rp::crypto
