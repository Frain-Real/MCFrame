#include "nvs_cipher.hpp"

#include "../crypto/aes.hpp"
#include "../crypto/hmac.hpp"

#include <cstring>

namespace embed::nvs {
namespace {

bool constant_time_equal(const uint8_t* a, const uint8_t* b, Size n) noexcept {
    uint8_t diff = 0u;
    for (Size i = 0u; i < n; ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0u;
}

} // namespace

AesCtrHmac::AesCtrHmac(const uint8_t key[KeySize]) noexcept {
    if (key == nullptr) return;
    std::memcpy(aes_key_, key, sizeof(aes_key_));
    std::memcpy(mac_key_, key + sizeof(aes_key_), sizeof(mac_key_));
}

AesCtrHmac::~AesCtrHmac() noexcept {
    for (auto& b : aes_key_) b = 0u;
    for (auto& b : mac_key_) b = 0u;
}

void AesCtrHmac::iv_from_nonce(uint64_t nonce, uint8_t iv[16]) const noexcept {
    for (uint8_t i = 0u; i < 8u; ++i) iv[i] = static_cast<uint8_t>(nonce >> (8u * i));
    for (uint8_t i = 8u; i < 16u; ++i) iv[i] = 0u;
}

Status AesCtrHmac::seal(uint64_t nonce, const void* aad, Size aad_len,
                        const void* plain, Size len, void* out) noexcept {
    if ((plain == nullptr && len != 0u) || out == nullptr) return Status::InvalidArg;

    uint8_t iv[16]{};
    iv_from_nonce(nonce, iv);
    crypto::Aes<crypto::AesKeySize::Aes128, crypto::AesMode::Ctr, false, false>
        aes(aes_key_, iv);
    if (!aes.valid()) return Status::HardwareError;

    Size out_len = 0u;
    if (len != 0u) {
        const Status st = aes.encrypt(static_cast<const uint8_t*>(plain), len,
                                      static_cast<uint8_t*>(out), len, &out_len);
        if (st != Status::Ok || out_len != len) {
            return (st == Status::Ok) ? Status::Fault : st;
        }
    }

    crypto::HmacSha256 hmac(mac_key_, sizeof(mac_key_));
    if (aad != nullptr && aad_len != 0u) hmac.update(aad, aad_len);
    hmac.update(out, len);
    uint8_t tag[crypto::HmacSha256OutSize]{};
    hmac.finalize(tag);
    std::memcpy(static_cast<uint8_t*>(out) + len, tag, TagSize);
    return Status::Ok;
}

Status AesCtrHmac::open(uint64_t nonce, const void* aad, Size aad_len,
                        const void* cipher, Size len, void* out,
                        Size& plain_len) noexcept {
    plain_len = 0u;
    if (cipher == nullptr || out == nullptr || len < TagSize) return Status::InvalidArg;

    const auto* in = static_cast<const uint8_t*>(cipher);
    const Size body = len - TagSize;

    crypto::HmacSha256 hmac(mac_key_, sizeof(mac_key_));
    if (aad != nullptr && aad_len != 0u) hmac.update(aad, aad_len);
    hmac.update(in, body);
    uint8_t tag[crypto::HmacSha256OutSize]{};
    hmac.finalize(tag);

    if (!constant_time_equal(tag, in + body, TagSize)) return Status::Corruption;

    uint8_t iv[16]{};
    iv_from_nonce(nonce, iv);
    crypto::Aes<crypto::AesKeySize::Aes128, crypto::AesMode::Ctr, false, false>
        aes(aes_key_, iv);
    if (!aes.valid()) return Status::HardwareError;

    Size out_len = 0u;
    if (body != 0u) {
        const Status st = aes.decrypt(in, body, static_cast<uint8_t*>(out), body, &out_len);
        if (st != Status::Ok) return st;
    }
    plain_len = out_len;
    return Status::Ok;
}

} // namespace embed::nvs
