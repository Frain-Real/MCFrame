// crypto/hmac.hpp
// ---------------------------------------------------------------------------
// HMAC-SHA256 (RFC 2104 / FIPS 198-1) — код аутентификации сообщения.
//
// Нужен для проверки целостности: схема «XOR с гаммой из общего секрета»
// не даёт никакой защиты от подмены шифротекста.
//
// Класс поддерживает потоковое обновление: сообщение не нужно склеивать
// в один буфер (на МКУ лишней памяти нет).
// ---------------------------------------------------------------------------
#pragma once

#include "crypto_utils.hpp"
#include "sha2.hpp"

#include <cstddef>
#include <cstdint>

namespace embed {
namespace crypto {

constexpr Size HmacSha256BlockSize = 64u;   // размер блока SHA-256
constexpr Size HmacSha256OutSize   = 32u;

class HmacSha256 {
public:
    /// @param key Может быть nullptr при key_len == 0 (тогда ключ — нули).
    HmacSha256(const void* key, Size key_len) noexcept;

    void update(const void* data, Size len) noexcept;

    /// Записать тег и обнулить состояние.
    void finalize(uint8_t out[HmacSha256OutSize]) noexcept;

private:
    Sha256  inner_;
    uint8_t outer_key_[HmacSha256BlockSize];
};

/// Однократный расчёт тега.
void hmac_sha256(const void* key, Size key_len,
                 const void* data, Size data_len,
                 uint8_t out[HmacSha256OutSize]) noexcept;

/// Проверка тега за постоянное время.
bool hmac_sha256_verify(const void* key, Size key_len,
                        const void* data, Size data_len,
                        const uint8_t expected[HmacSha256OutSize]) noexcept;

} // namespace crypto
} // namespace embed
