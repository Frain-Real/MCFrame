// crypto/hmac.cpp — HMAC-SHA256.
#include "hmac.hpp"

namespace embed {
namespace crypto {
namespace {

constexpr uint8_t kIpad = 0x36u;
constexpr uint8_t kOpad = 0x5Cu;

/// Привести ключ к длине блока: длинный — хэшируем, короткий — дополняем нулями.
void normalize_key(const void* key, Size key_len,
                   uint8_t block_key[HmacSha256BlockSize]) noexcept {
    utilities::fast_memset(block_key, 0, HmacSha256BlockSize);
    if (key == nullptr || key_len == 0u) {
        return;
    }
    if (key_len > HmacSha256BlockSize) {
        Sha256::hash(key, key_len, block_key);   // 32 байта + нули
        return;
    }
    utilities::fast_memcpy(block_key, key, key_len);
}

} // namespace

HmacSha256::HmacSha256(const void* key, Size key_len) noexcept {
    uint8_t block_key[HmacSha256BlockSize];
    normalize_key(key, key_len, block_key);

    uint8_t pad[HmacSha256BlockSize];
    for (Size i = 0; i < HmacSha256BlockSize; ++i) {
        pad[i]          = static_cast<uint8_t>(block_key[i] ^ kIpad);
        outer_key_[i]   = static_cast<uint8_t>(block_key[i] ^ kOpad);
    }
    inner_.update(pad, sizeof(pad));

    wipe(block_key, sizeof(block_key));
    wipe(pad, sizeof(pad));
}

void HmacSha256::update(const void* data, Size len) noexcept {
    inner_.update(data, len);
}

void HmacSha256::finalize(uint8_t out[HmacSha256OutSize]) noexcept {
    if (out == nullptr) {
        return;
    }
    uint8_t inner_digest[HmacSha256OutSize];
    inner_.finalize(inner_digest);

    Sha256 outer;
    outer.update(outer_key_, sizeof(outer_key_));
    outer.update(inner_digest, sizeof(inner_digest));
    outer.finalize(out);

    wipe(inner_digest, sizeof(inner_digest));
    wipe(outer_key_, sizeof(outer_key_));
}

void hmac_sha256(const void* key, Size key_len,
                 const void* data, Size data_len,
                 uint8_t out[HmacSha256OutSize]) noexcept {
    HmacSha256 mac(key, key_len);
    mac.update(data, data_len);
    mac.finalize(out);
}

bool hmac_sha256_verify(const void* key, Size key_len,
                        const void* data, Size data_len,
                        const uint8_t expected[HmacSha256OutSize]) noexcept {
    if (expected == nullptr) {
        return false;
    }
    uint8_t actual[HmacSha256OutSize];
    hmac_sha256(key, key_len, data, data_len, actual);
    const bool ok = ct_equal(actual, expected, sizeof(actual));
    wipe(actual, sizeof(actual));
    return ok;
}

} // namespace crypto
} // namespace embed
