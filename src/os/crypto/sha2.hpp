// crypto/sha2.hpp
// ---------------------------------------------------------------------------
// SHA-2: SHA-224/256/384/512 и SHA-512/224, SHA-512/256 (FIPS 180-4).
//
// ЧТО БЫЛО ИСПРАВЛЕНО:
//   1. Таблицы констант. K256 совпадала с эталоном только до 45-го слова,
//      K512 — до 19-го, дальше шёл мусор (в K512 вообще было 92 слова вместо
//      80). SHA-256/512 считали неправильный хэш на любых данных, для
//      которых нужно больше одного раунда сверх первых.
//   2. Начальные значения SHA-512/224 были выдуманы целиком, у SHA-512/256
//      неверны последние два слова. Оба варианта давали неверный дайджест.
//   3. Усечение дайджеста не выполнялось вообще: SHA224::finalize() отдавала
//      28-байтовый буфер, а SHA256_Family::get_digest_bytes() писала в него
//      32 байта — переполнение буфера вызывающего (портил стек). То же для
//      SHA-512/224 (28 из 64) и SHA-512/256 (32 из 64).
//   4. Наследование с чисто виртуальными функциями в КОНСТРУКТОРЕ базового
//      класса: SHA2_Base() вызывал init() → init_hash_values(), то есть
//      виртуальный вызов в конструкторе базового класса, который для
//      производного объекта попадает в базовую версию. Заменено на
//      статический полиморфизм (шаблон по варианту) — виртуальных функций
//      нет, размер объекта меньше, вызовы разворачиваются на этапе компиляции.
//   5. Длина сообщения хранилась только в 64 битах, из-за чего у семейства
//      SHA-512 старшие 8 байт поля длины были прибиты к нулю. Теперь длина —
//      128-битная (две пары 64-битных счётчиков).
//   6. Базовый класс не был защищён от повторного finalize()/update() после
//      finalize(): состояние сбрасывалось в init(), и это было неявно.
// ---------------------------------------------------------------------------
#pragma once

#include "crypto_utils.hpp"

#include <cstddef>
#include <cstdint>

namespace embed {
namespace crypto {

/// Варианты SHA-2 (различаются размером дайджеста и блоком).
enum class Sha2Variant {
    Sha224,
    Sha256,
    Sha384,
    Sha512,
    Sha512_224,
    Sha512_256
};

template <Sha2Variant V>
struct Sha2Traits;

template <>
struct Sha2Traits<Sha2Variant::Sha224> {
    using Word = uint32_t;
    static constexpr Size BlockSize  = 64u;
    static constexpr Size DigestSize = 28u;
    static constexpr int  Rounds     = 64;
    static constexpr bool Is64Bit    = false;
    static constexpr uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    static constexpr uint32_t H[8] = {
        0xc1059ed8u, 0x367cd507u, 0x3070dd17u, 0xf70e5939u,
        0xffc00b31u, 0x68581511u, 0x64f98fa7u, 0xbefa4fa4u
    };
};

template <>
struct Sha2Traits<Sha2Variant::Sha256> {
    using Word = uint32_t;
    static constexpr Size BlockSize  = 64u;
    static constexpr Size DigestSize = 32u;
    static constexpr int  Rounds     = 64;
    static constexpr bool Is64Bit    = false;
    static constexpr uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    static constexpr uint32_t H[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
};

template <>
struct Sha2Traits<Sha2Variant::Sha384> {
    using Word = uint64_t;
    static constexpr Size BlockSize  = 128u;
    static constexpr Size DigestSize = 48u;
    static constexpr int  Rounds     = 80;
    static constexpr bool Is64Bit    = true;
    static constexpr uint64_t K[80] = {
        0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
        0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
        0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
        0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
        0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
        0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
        0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
        0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
        0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
        0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
        0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
        0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
        0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
        0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
        0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
        0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
        0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
        0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
        0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
        0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
    };
    static constexpr uint64_t H[8] = {
        0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
        0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull
    };
};

template <>
struct Sha2Traits<Sha2Variant::Sha512> {
    using Word = uint64_t;
    static constexpr Size BlockSize  = 128u;
    static constexpr Size DigestSize = 64u;
    static constexpr int  Rounds     = 80;
    static constexpr bool Is64Bit    = true;
    static constexpr uint64_t K[80] = {
        0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
        0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
        0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
        0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
        0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
        0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
        0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
        0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
        0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
        0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
        0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
        0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
        0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
        0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
        0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
        0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
        0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
        0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
        0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
        0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull
    };
    static constexpr uint64_t H[8] = {
        0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
        0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull
    };
};

// SHA-512/224 и SHA-512/256 используют те же K, что SHA-512.
template <>
struct Sha2Traits<Sha2Variant::Sha512_224> : Sha2Traits<Sha2Variant::Sha512> {
    static constexpr Size DigestSize = 28u;
    static constexpr uint64_t H[8] = {
        0x8c3d37c819544da2ull, 0x73e1996689dcd4d6ull, 0x1dfab7ae32ff9c82ull, 0x679dd514582f9fcfull,
        0x0f6d2b697bd44da8ull, 0x77e36f7304c48942ull, 0x3f9d85a86a1d36c8ull, 0x1112e6ad91d692a1ull
    };
};

template <>
struct Sha2Traits<Sha2Variant::Sha512_256> : Sha2Traits<Sha2Variant::Sha512> {
    static constexpr Size DigestSize = 32u;
    static constexpr uint64_t H[8] = {
        0x22312194fc2bf72cull, 0x9f555fa3c84c64c2ull, 0x2393b86b6f53b151ull, 0x963877195940eabdull,
        0x96283ee2a88effe3ull, 0xbe5e1e2553863992ull, 0x2b0199fc2c85b8aaull, 0x0eb72ddc81c52ca2ull
    };
};

/**
 * @brief SHA-2 выбранного варианта.
 *
 * Использование:
 * @code
 *   embed::crypto::Sha256 h;
 *   h.update(buf, len);
 *   uint8_t digest[embed::crypto::Sha256::DigestSize];
 *   h.finalize(digest);
 * @endcode
 *
 * Дайджест пишется РОВНО DigestSize байт: буфер вызывающего не переполняется.
 * После finalize() объект возвращается в исходное состояние и пригоден для
 * следующего сообщения.
 */
template <Sha2Variant V>
class Sha2 {
public:
    using Traits = Sha2Traits<V>;
    using Word   = typename Traits::Word;

    static constexpr Size BlockSize  = Traits::BlockSize;
    static constexpr Size DigestSize = Traits::DigestSize;

    Sha2() noexcept { reset(); }

    /// Начальное состояние (можно переиспользовать объект без пересоздания).
    void reset() noexcept {
        for (int i = 0; i < 8; ++i) {
            h_[i] = Traits::H[i];
        }
        buffer_fill_ = 0u;
        bytes_lo_    = 0u;
        bytes_hi_    = 0u;
    }

    /// Добавить данные. data == nullptr при len > 0 игнорируется.
    void update(const void* data, Size len) noexcept {
        if (data == nullptr || len == 0u) {
            return;
        }
        const uint8_t* p = static_cast<const uint8_t*>(data);

        // 128-битный счётчик байт (младшая и старшая половины).
        const uint64_t prev_lo = bytes_lo_;
        bytes_lo_ += static_cast<uint64_t>(len);
        if (bytes_lo_ < prev_lo) {
            bytes_hi_ += 1u;
        }

        if (buffer_fill_ != 0u) {
            const Size need = BlockSize - buffer_fill_;
            const Size take = (len < need) ? len : need;
            utilities::fast_memcpy(buffer_ + buffer_fill_, p, take);
            buffer_fill_ += take;
            p            += take;
            len          -= take;
            if (buffer_fill_ == BlockSize) {
                transform(buffer_);
                buffer_fill_ = 0u;
            }
        }

        while (len >= BlockSize) {
            transform(p);
            p   += BlockSize;
            len -= BlockSize;
        }

        if (len != 0u) {
            utilities::fast_memcpy(buffer_, p, len);
            buffer_fill_ = len;
        }
    }

    /// Записать дайджест (DigestSize байт) и сбросить состояние.
    void finalize(uint8_t* digest) noexcept {
        if (digest == nullptr) {
            return;
        }
        const uint64_t bits_lo = bytes_lo_ << 3;
        const uint64_t bits_hi = (bytes_hi_ << 3) | (bytes_lo_ >> 61);

        // 1. Бит '1' + нули; если места под длину не осталось — ещё блок.
        buffer_[buffer_fill_++] = 0x80u;
        const Size length_field = Traits::Is64Bit ? 16u : 8u;
        if (buffer_fill_ > BlockSize - length_field) {
            utilities::fast_memset(buffer_ + buffer_fill_, 0, BlockSize - buffer_fill_);
            transform(buffer_);
            buffer_fill_ = 0u;
        }
        utilities::fast_memset(buffer_ + buffer_fill_, 0,
                               BlockSize - length_field - buffer_fill_);

        // 2. Длина сообщения в битах, big-endian. У семейства SHA-512 поле
        //    128-битное, поэтому пишутся обе половины.
        if (Traits::Is64Bit) {
            store_be64(bits_hi, buffer_ + BlockSize - 16u);
        }
        store_be64(bits_lo, buffer_ + BlockSize - 8u);

        transform(buffer_);

        // 3. Дайджест — старшие DigestSize байт состояния, big-endian.
        uint8_t full[64];
        for (int i = 0; i < 8; ++i) {
            if constexpr (Traits::Is64Bit) {
                store_be64(h_[i], full + i * 8);
            } else {
                store_be32(h_[i], full + i * 4);
            }
        }
        utilities::fast_memcpy(digest, full, DigestSize);
        wipe(full, sizeof(full));

        reset();
    }

    /// Однократный расчёт: hash(data, len, digest).
    static void hash(const void* data, Size len, uint8_t* digest) noexcept {
        Sha2<V> h;
        h.update(data, len);
        h.finalize(digest);
    }

private:
    using StateWord = typename Traits::Word;

    StateWord h_[8];
    uint8_t   buffer_[Traits::BlockSize];
    Size      buffer_fill_;
    uint64_t  bytes_lo_;
    uint64_t  bytes_hi_;

    static constexpr StateWord rotr(StateWord x, uint32_t n) noexcept {
        return (x >> n) | (x << ((sizeof(StateWord) * 8u) - n));
    }

    static constexpr StateWord ch(StateWord x, StateWord y, StateWord z) noexcept {
        return (x & y) ^ (~x & z);
    }

    static constexpr StateWord maj(StateWord x, StateWord y, StateWord z) noexcept {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    /// Σ0 (старшая сигма) для раунда сжатия.
    static constexpr StateWord big_sigma0(StateWord x) noexcept {
        if constexpr (Traits::Is64Bit) {
            return rotr(x, 28u) ^ rotr(x, 34u) ^ rotr(x, 39u);
        } else {
            return rotr(x, 2u) ^ rotr(x, 13u) ^ rotr(x, 22u);
        }
    }

    /// Σ1 (старшая сигма) для раунда сжатия.
    static constexpr StateWord big_sigma1(StateWord x) noexcept {
        if constexpr (Traits::Is64Bit) {
            return rotr(x, 14u) ^ rotr(x, 18u) ^ rotr(x, 41u);
        } else {
            return rotr(x, 6u) ^ rotr(x, 11u) ^ rotr(x, 25u);
        }
    }

    /// σ0 (младшая сигма) для разворачивания расписания сообщения.
    static constexpr StateWord small_sigma0(StateWord x) noexcept {
        if constexpr (Traits::Is64Bit) {
            return rotr(x, 1u) ^ rotr(x, 8u) ^ (x >> 7u);
        } else {
            return rotr(x, 7u) ^ rotr(x, 18u) ^ (x >> 3u);
        }
    }

    /// σ1 (младшая сигма) для разворачивания расписания сообщения.
    static constexpr StateWord small_sigma1(StateWord x) noexcept {
        if constexpr (Traits::Is64Bit) {
            return rotr(x, 19u) ^ rotr(x, 61u) ^ (x >> 6u);
        } else {
            return rotr(x, 17u) ^ rotr(x, 19u) ^ (x >> 10u);
        }
    }

    /// Обработать один блок (BlockSize байт).
    void transform(const uint8_t* block) noexcept {
        StateWord w[Traits::Rounds];
        constexpr Size word_bytes = sizeof(StateWord);

        for (int i = 0; i < 16; ++i) {
            if constexpr (Traits::Is64Bit) {
                w[i] = static_cast<StateWord>(load_be64(block + i * word_bytes));
            } else {
                w[i] = static_cast<StateWord>(load_be32(block + i * word_bytes));
            }
        }
        for (int i = 16; i < Traits::Rounds; ++i) {
            w[i] = small_sigma1(w[i - 2]) + w[i - 7] +
                   small_sigma0(w[i - 15]) + w[i - 16];
        }

        StateWord a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        StateWord e = h_[4], f = h_[5], g = h_[6], hh = h_[7];

        for (int i = 0; i < Traits::Rounds; ++i) {
            const StateWord t1 = hh + big_sigma1(e) + ch(e, f, g) +
                                 static_cast<StateWord>(Traits::K[i]) + w[i];
            const StateWord t2 = big_sigma0(a) + maj(a, b, c);

            hh = g;
            g  = f;
            f  = e;
            e  = d + t1;
            d  = c;
            c  = b;
            b  = a;
            a  = t1 + t2;
        }

        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;

        wipe(w, sizeof(w));
    }
};

using Sha224     = Sha2<Sha2Variant::Sha224>;
using Sha256     = Sha2<Sha2Variant::Sha256>;
using Sha384     = Sha2<Sha2Variant::Sha384>;
using Sha512     = Sha2<Sha2Variant::Sha512>;
using Sha512_224 = Sha2<Sha2Variant::Sha512_224>;
using Sha512_256 = Sha2<Sha2Variant::Sha512_256>;

// Однократные обёртки.
inline void sha224(const void* data, Size len, uint8_t digest[28]) noexcept {
    Sha224::hash(data, len, digest);
}
inline void sha256(const void* data, Size len, uint8_t digest[32]) noexcept {
    Sha256::hash(data, len, digest);
}
inline void sha384(const void* data, Size len, uint8_t digest[48]) noexcept {
    Sha384::hash(data, len, digest);
}
inline void sha512(const void* data, Size len, uint8_t digest[64]) noexcept {
    Sha512::hash(data, len, digest);
}

} // namespace crypto
} // namespace embed
