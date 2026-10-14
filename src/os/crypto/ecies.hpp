// crypto/ecies.hpp
// ---------------------------------------------------------------------------
// Гибридное шифрование на ECDH: эфемерный ключ + производные ключи
// (KDF на SHA-256) + HMAC-SHA256 для целостности.
//
// ЧТО БЫЛО ИСПРАВЛЕНО: прежние SoftECC::Encrypt/Decrypt шифровали данные
// XOR'ом с общим секретом, повторяя 32-байтовый ключ по кругу:
//   output[64 + i] = input[i] ^ sym_key[i % 32];
// Это не шифрование, а «одноразовый блокнот с повторяющимся ключом»:
// одинаковые блоки открытого текста дают одинаковый шифротекст
// (гамма восстанавливается по двум сообщениям), плюс нет никакой проверки
// целостности — подмена шифротекста не обнаруживается. Здесь общий секрет
// проходит через KDF с привязкой к обоим открытым ключам, гамма никогда не
// повторяется, а тег HMAC ловит любое изменение.
//
// Формат сообщения:
//   eph_x[32] || eph_y[32] || ciphertext[len] || tag[32]
//
// Ключи:
//   Z    = x-координата(d_эфемерный · Q_получателя)   (obaja стороны)
//   ctx  = Z || eph_pub || Q_получателя
//   Kenc = SHA-256(0x01 || ctx), Kmac = SHA-256(0x02 || ctx)
//   гамма_j = SHA-256(Kenc || be32(j)), j = 0, 1, 2, ...
//   tag  = HMAC-SHA256(Kmac, eph_pub || ciphertext)
// ---------------------------------------------------------------------------
#pragma once

#include "ecc.hpp"
#include "hmac.hpp"
#include "sha2.hpp"

#include <cstddef>
#include <cstdint>

namespace embed {
namespace crypto {

/// Размер открытого ключа на проводе: x || y (по 32 байта).
constexpr Size EccPublicKeyBytes = 64u;

template <EccCurve Curve>
class Ecies {
    using Dh = Ecdh<Curve>;

public:
    using Scalar = typename Dh::Scalar;
    using Point  = typename Dh::Point;

    static constexpr Size PublicKeyBytes = EccPublicKeyBytes;
    static constexpr Size TagBytes       = HmacSha256OutSize;
    static constexpr Size OverheadBytes  = PublicKeyBytes + TagBytes;

    /// Ключ получателя (для расшифрования).
    explicit Ecies(const Scalar& private_key) noexcept : private_key_(private_key) {}

    Ecies(const Ecies&)            = delete;
    Ecies& operator=(const Ecies&) = delete;

    /// Размер шифротекста для plain_len байт открытого текста.
    static constexpr Size encrypted_size(Size plain_len) noexcept {
        return OverheadBytes + plain_len;
    }

    /// Сериализовать точку (x||y, big-endian).
    static void encode_point(const Point& p, uint8_t out[PublicKeyBytes]) noexcept {
        for (Size i = 0; i < EccWords; ++i) {
            const uint32_t xw = (i < p.x.len) ? p.x.d[i] : 0u;
            const uint32_t yw = (i < p.y.len) ? p.y.d[i] : 0u;
            store_be32(xw, out + (EccWords - 1u - i) * 4u);
            store_be32(yw, out + PublicKeyBytes / 2u + (EccWords - 1u - i) * 4u);
        }
    }

    /// Разобрать точку; false — если координаты не лежат на кривой.
    static bool decode_point(const uint8_t in[PublicKeyBytes], Point& out) noexcept {
        Scalar x;
        Scalar y;
        for (Size i = 0; i < EccWords; ++i) {
            x.d[i] = load_be32(in + (EccWords - 1u - i) * 4u);
            y.d[i] = load_be32(in + PublicKeyBytes / 2u + (EccWords - 1u - i) * 4u);
        }
        x.len = EccWords; x.neg = false; x.normalize();
        y.len = EccWords; y.neg = false; y.normalize();

        Point p(x, y);
        if (!Dh::is_on_curve(p)) {
            return false;
        }
        out = p;
        return true;
    }

    /**
     * @brief Зашифровать сообщение для получателя.
     * @param rng       CSPRNG для эфемерного ключа (обязателен).
     * @param recipient Публичный ключ получателя (проверяется на кривой).
     */
    static Status encrypt(Rng& rng, const Point& recipient,
                          const uint8_t* plain, Size plain_len,
                          uint8_t* out, Size out_cap, Size* out_len) noexcept {
        if (plain == nullptr || out == nullptr || out_len == nullptr) {
            return Status::InvalidArg;
        }
        if (!rng.ready() || !Dh::is_on_curve(recipient)) {
            return Status::InvalidArg;
        }
        const Size needed = encrypted_size(plain_len);
        if (out_cap < needed) {
            return Status::NoMemory;
        }

        Scalar eph_priv;
        Point  eph_pub;
        if (!Dh::generate_key_pair(rng, eph_priv, eph_pub)) {
            return Status::Fault;
        }

        uint8_t z[32];
        if (!Dh::derive_shared_secret_bytes(eph_priv, recipient, z)) {
            return Status::InvalidArg;
        }

        uint8_t eph_bytes[PublicKeyBytes];
        uint8_t rec_bytes[PublicKeyBytes];
        encode_point(eph_pub, eph_bytes);
        encode_point(recipient, rec_bytes);

        uint8_t kenc[32];
        uint8_t kmac[32];
        derive_keys(z, eph_bytes, rec_bytes, kenc, kmac);

        uint8_t* cipher = out + PublicKeyBytes;
        keystream_xor(kenc, cipher, plain, plain_len);

        // Тег покрывает эфемерный ключ и шифротекст: любая подмена
        // обнаруживается до расшифрования. HMAC потоковый — склеивать
        // сообщение в памяти не нужно.
        uint8_t tag[TagBytes];
        {
            HmacSha256 mac(kmac, sizeof(kmac));
            mac.update(eph_bytes, sizeof(eph_bytes));
            mac.update(cipher, plain_len);
            mac.finalize(tag);
        }

        utilities::fast_memcpy(out, eph_bytes, PublicKeyBytes);
        utilities::fast_memcpy(cipher + plain_len, tag, TagBytes);
        *out_len = needed;

        wipe(z, sizeof(z));
        wipe(kenc, sizeof(kenc));
        wipe(kmac, sizeof(kmac));
        wipe(tag, sizeof(tag));
        wipe(eph_bytes, sizeof(eph_bytes));
        wipe(rec_bytes, sizeof(rec_bytes));
        return Status::Ok;
    }

    /// Расшифровать и проверить тег.
    Status decrypt(const uint8_t* in, Size in_len,
                   uint8_t* out, Size out_cap, Size* out_len) const noexcept {
        if (in == nullptr || out == nullptr || out_len == nullptr) {
            return Status::InvalidArg;
        }
        if (in_len < OverheadBytes) {
            return Status::InvalidArg;
        }
        if (!Dh::is_valid_private_key(private_key_)) {
            return Status::InvalidArg;
        }
        const Size plain_len = in_len - OverheadBytes;
        if (out_cap < plain_len) {
            return Status::NoMemory;
        }

        Point eph_pub;
        if (!decode_point(in, eph_pub)) {
            return Status::InvalidArg;
        }

        uint8_t z[32];
        if (!Dh::derive_shared_secret_bytes(private_key_, eph_pub, z)) {
            return Status::InvalidArg;
        }

        uint8_t own_bytes[PublicKeyBytes];
        Point own_pub;
        if (!Dh::compute_public_key(private_key_, own_pub)) {
            return Status::InvalidArg;
        }
        encode_point(own_pub, own_bytes);

        uint8_t kenc[32];
        uint8_t kmac[32];
        derive_keys(z, in, own_bytes, kenc, kmac);

        const uint8_t* cipher = in + PublicKeyBytes;
        const uint8_t* tag    = in + PublicKeyBytes + plain_len;

        uint8_t expected[TagBytes];
        {
            HmacSha256 mac(kmac, sizeof(kmac));
            mac.update(in, PublicKeyBytes);
            mac.update(cipher, plain_len);
            mac.finalize(expected);
        }
        if (!ct_equal(expected, tag, TagBytes)) {
            wipe(z, sizeof(z));
            wipe(kenc, sizeof(kenc));
            wipe(kmac, sizeof(kmac));
            wipe(expected, sizeof(expected));
            return Status::Corruption;
        }

        keystream_xor(kenc, out, cipher, plain_len);
        *out_len = plain_len;

        wipe(z, sizeof(z));
        wipe(kenc, sizeof(kenc));
        wipe(kmac, sizeof(kmac));
        wipe(expected, sizeof(expected));
        return Status::Ok;
    }

    /// Публичный ключ этого объекта (из приватного).
    bool public_key(Point& out) const noexcept {
        return Dh::compute_public_key(private_key_, out);
    }

private:
    Scalar private_key_;

    /// Kenc = SHA256(0x01||ctx), Kmac = SHA256(0x02||ctx).
    static void derive_keys(const uint8_t z[32],
                            const uint8_t* eph_bytes,
                            const uint8_t* rec_bytes,
                            uint8_t kenc[32], uint8_t kmac[32]) noexcept {
        uint8_t label_enc = 0x01u;
        uint8_t label_mac = 0x02u;
        Sha256 h;
        h.update(&label_enc, 1u);
        h.update(z, 32u);
        h.update(eph_bytes, PublicKeyBytes);
        h.update(rec_bytes, PublicKeyBytes);
        h.finalize(kenc);

        h.update(&label_mac, 1u);
        h.update(z, 32u);
        h.update(eph_bytes, PublicKeyBytes);
        h.update(rec_bytes, PublicKeyBytes);
        h.finalize(kmac);
    }

    /// Гамма: block_j = SHA256(Kenc || be32(j)).
    static void keystream_xor(const uint8_t kenc[32], uint8_t* out,
                              const uint8_t* in, Size len) noexcept {
        uint32_t counter = 0u;
        Size offset = 0u;
        while (offset < len) {
            uint8_t block[32];
            Sha256 h;
            h.update(kenc, 32u);
            uint8_t ctr[4];
            store_be32(counter, ctr);
            h.update(ctr, sizeof(ctr));
            h.finalize(block);

            Size chunk = len - offset;
            if (chunk > sizeof(block)) {
                chunk = sizeof(block);
            }
            for (Size i = 0; i < chunk; ++i) {
                out[offset + i] = static_cast<uint8_t>(in[offset + i] ^ block[i]);
            }
            offset  += chunk;
            counter += 1u;
            wipe(block, sizeof(block));
        }
    }
};

} // namespace crypto
} // namespace embed
