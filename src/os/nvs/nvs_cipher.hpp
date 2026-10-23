// os/nvs/nvs_cipher.hpp
// ---------------------------------------------------------------------------
// Шифрование значений NVS: AES-128-CTR + HMAC-SHA256 (encrypt-then-MAC).
//
// Почему CTR, а не CBC: значение может быть не кратно 16 байтам (blob,
// строка), а CTR не требует padding. Почему HMAC, а не «только AES»: CTR
// сам по себе не аутентифицирует, и подмена/перестановка блоков осталась бы
// незамеченной. Тег усечён до 16 байт — этого достаточно для целостности
// настроек и экономит Flash.
//
// Ключ НЕ хранится в NVS. Приложение передаёт 32 байта: первые 16 — ключ
// AES, вторые 16 — ключ HMAC. Nonce (уникальный для записи) хранится в
// заголовке записи, AAD — полный ключ (namespace/key), поэтому значение
// нельзя переложить под другое имя.
// ---------------------------------------------------------------------------
#pragma once

#include "nvs.hpp"

namespace embed::nvs {

class AesCtrHmac final : public Cipher {
public:
    static constexpr Size KeySize = 32u;
    static constexpr Size TagSize = 16u;

    explicit AesCtrHmac(const uint8_t key[KeySize]) noexcept;
    ~AesCtrHmac() noexcept override;

    AesCtrHmac(const AesCtrHmac&) = delete;
    AesCtrHmac& operator=(const AesCtrHmac&) = delete;

    Size overhead() const noexcept override { return TagSize; }

    Status seal(uint64_t nonce, const void* aad, Size aad_len,
                const void* plain, Size len, void* out) noexcept override;

    Status open(uint64_t nonce, const void* aad, Size aad_len,
                const void* cipher, Size len, void* out,
                Size& plain_len) noexcept override;

private:
    void iv_from_nonce(uint64_t nonce, uint8_t iv[16]) const noexcept;

    uint8_t aes_key_[16]{};
    uint8_t mac_key_[16]{};
};

} // namespace embed::nvs
