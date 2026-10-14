// crypto/sha3.hpp
// ---------------------------------------------------------------------------
// SHA-3 (FIPS 202) и SHAKE — губка Keccak-f[1600].
//
// ЧТО БЫЛО ИСПРАВЛЕНО:
//   1. В перестановке Keccak не инициализировался элемент B[0][0]: шаг Chi
//      читал неинициализированную локальную матрицу, то есть результат зависел
//      от содержимого стека (и от сборки к сборке). Теперь состояние — плоский
//      массив 25 дорожек, и Rho/Pi обходит все дорожки, кроме (0,0), которая
//      переносится без сдвига явно.
//   2. «Склеенное» состояние губки: pad_and_absorb_last_block() не сбрасывал
//      blockLen, а shake_squeeze() решал, нужен ли padding, по эвристике
//      `state[0][0] == 0` — повторный squeeze давал мусор, а пустое сообщение
//      с нулевым состоянием приводил к повторному padding'у. Теперь есть явные
//      фазы (absorbing/squeezing) и позиция выдачи.
//   3. Неверный набор параметров (например, SHAKE с outputBits == 192) не
//      диагностировался: rate оставался нулевым, и keccak_absorb() крутился в
//      бесконечном цикле (copy_len == 0, len не убывает). Теперь параметры
//      проверяются статически на этапе компиляции.
//   4. Макрос ROTL64(x, n) ломался при n == 0 (сдвиг на 64 бита — UB) и
//      протекал в чужие единицы трансляции; заменён на constexpr-функцию.
//   5. Доступ к дорожкам при выдаче результата был через [5][5] с ручным
//      пересчётом индексов — источник ошибок; теперь index = x + 5y в одном
//      месте.
// ---------------------------------------------------------------------------
#pragma once

#include "crypto_utils.hpp"

#include <cstddef>
#include <cstdint>

namespace embed {
namespace crypto {
namespace detail {

/// Перестановка Keccak-f[1600] и состояние губки (общее для SHA-3 и SHAKE).
class KeccakState {
public:
    static constexpr Size StateBytes = 200u;   // 1600 бит
    static constexpr Size MaxRate    = 168u;   // SHAKE128: (1600 - 256) / 8

protected:
    /// Подготовить губку к впитыванию.
    /// @param rate_bytes скорость (rate) в байтах.
    /// @param suffix     байт дополнения: 0x06 для SHA-3, 0x1F для SHAKE.
    void keccak_init(Size rate_bytes, uint8_t suffix) noexcept {
        for (int i = 0; i < 25; ++i) {
            state_[i] = 0u;
        }
        for (Size i = 0; i < StateBytes; ++i) {
            block_[i] = 0u;
        }
        rate_         = rate_bytes;
        suffix_       = suffix;
        block_fill_   = 0u;
        squeeze_pos_  = 0u;
        squeezing_    = false;
    }

    void keccak_absorb(const void* data, Size len) noexcept {
        if (data == nullptr || len == 0u) {
            return;
        }
        // После начала выдачи впитывать нельзя: губка уже «отжата».
        if (squeezing_) {
            return;
        }
        const uint8_t* p = static_cast<const uint8_t*>(data);

        while (len > 0u) {
            const Size free_bytes = rate_ - block_fill_;
            const Size take = (len < free_bytes) ? len : free_bytes;
            utilities::fast_memcpy(block_ + block_fill_, p, take);
            block_fill_ += take;
            p           += take;
            len         -= take;

            if (block_fill_ == rate_) {
                absorb_block();
                block_fill_ = 0u;
            }
        }
    }

    /// Дополнение (pad10*1) и впитывание последнего блока.
    void keccak_pad() noexcept {
        block_[block_fill_] = suffix_;
        for (Size i = block_fill_ + 1u; i < rate_; ++i) {
            block_[i] = 0u;
        }
        block_[rate_ - 1u] |= 0x80u;

        absorb_block();
        block_fill_  = 0u;
        squeeze_pos_ = 0u;
        squeezing_   = true;
    }

    /// Выдать len байт из губки (можно вызывать несколько раз подряд).
    void keccak_squeeze(uint8_t* out, Size len) noexcept {
        if (out == nullptr || len == 0u) {
            return;
        }
        Size done = 0u;
        while (done < len) {
            if (squeeze_pos_ == rate_) {
                permute();
                squeeze_pos_ = 0u;
            }
            Size chunk = rate_ - squeeze_pos_;
            if (chunk > (len - done)) {
                chunk = len - done;
            }
            for (Size i = 0; i < chunk; ++i) {
                const Size at   = squeeze_pos_ + i;
                const Size lane = at / 8u;
                const uint32_t sh = static_cast<uint32_t>((at % 8u) * 8u);
                out[done + i] = static_cast<uint8_t>((state_[lane] >> sh) & 0xFFu);
            }
            squeeze_pos_ += chunk;
            done         += chunk;
        }
    }

    bool finished() const noexcept { return squeezing_; }

private:
    uint64_t state_[25];            // дорожки: state_[x + 5y]
    uint8_t  block_[StateBytes];
    Size     rate_;                 // байт на блок (скорость)
    Size     block_fill_;           // сколько байт накоплено до rate_
    Size     squeeze_pos_;          // выдано байт из текущего блока
    uint8_t  suffix_;
    bool     squeezing_;

    static constexpr uint64_t rotl64(uint64_t x, uint32_t n) noexcept {
        return (n == 0u) ? x : ((x << n) | (x >> (64u - n)));
    }

    /// XOR блока в состояние + перестановка.
    void absorb_block() noexcept {
        const Size lanes = rate_ / 8u;
        for (Size i = 0; i < lanes; ++i) {
            state_[i] ^= load_le64(block_ + i * 8u);
        }
        permute();
    }

    void permute() noexcept {
        static const uint64_t round_constants[24] = {
            0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808aull,
            0x8000000080008000ull, 0x000000000000808bull, 0x0000000080000001ull,
            0x8000000080008081ull, 0x8000000000008009ull, 0x000000000000008aull,
            0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000aull,
            0x000000008000808bull, 0x800000000000008bull, 0x8000000000008089ull,
            0x8000000000008003ull, 0x8000000000008002ull, 0x8000000000000080ull,
            0x000000000000800aull, 0x800000008000000aull, 0x8000000080008081ull,
            0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull
        };
        // Порядок обхода дорожек в шаге Rho-Pi и величины сдвигов.
        static const uint32_t pi_lane[24] = {
            10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
            15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1
        };
        static const uint32_t rho_shift[24] = {
            1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
            27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44
        };

        uint64_t c[5];

        for (int round = 0; round < 24; ++round) {
            // Theta
            for (int x = 0; x < 5; ++x) {
                c[x] = state_[x] ^ state_[x + 5] ^ state_[x + 10] ^
                       state_[x + 15] ^ state_[x + 20];
            }
            for (int x = 0; x < 5; ++x) {
                const uint64_t d = c[(x + 4) % 5] ^ rotl64(c[(x + 1) % 5], 1u);
                for (int y = 0; y < 25; y += 5) {
                    state_[y + x] ^= d;
                }
            }

            // Rho + Pi. Дорожка (0,0) остаётся на месте без сдвига,
            // остальные 24 обходятся циклом — так не остаётся «забытых»
            // элементов (прежний вариант не заполнял B[0][0]).
            uint64_t carried = state_[1];
            for (int i = 0; i < 24; ++i) {
                const uint32_t lane = pi_lane[i];
                const uint64_t tmp  = state_[lane];
                state_[lane] = rotl64(carried, rho_shift[i]);
                carried      = tmp;
            }

            // Chi
            for (int y = 0; y < 25; y += 5) {
                for (int x = 0; x < 5; ++x) {
                    c[x] = state_[y + x];
                }
                for (int x = 0; x < 5; ++x) {
                    state_[y + x] ^= (~c[(x + 1) % 5]) & c[(x + 2) % 5];
                }
            }

            // Iota
            state_[0] ^= round_constants[round];
        }
    }
};

} // namespace detail

/**
 * @brief SHA-3 с фиксированной длиной дайджеста (FIPS 202).
 * @tparam DigestBits 224, 256, 384 или 512.
 *
 * @code
 *   uint8_t d[embed::crypto::Sha3_256::DigestSize];
 *   embed::crypto::Sha3_256::hash(buf, len, d);
 * @endcode
 */
template <unsigned DigestBits>
class Sha3 : public detail::KeccakState {
    static_assert(DigestBits == 224u || DigestBits == 256u ||
                  DigestBits == 384u || DigestBits == 512u,
                  "SHA-3: поддерживаются 224, 256, 384 и 512 бит");

public:
    static constexpr Size DigestSize = DigestBits / 8u;
    /// rate = 1600 - capacity, capacity = 2 * DigestBits.
    static constexpr Size RateBytes = (1600u - 2u * DigestBits) / 8u;

    Sha3() noexcept { reset(); }

    void reset() noexcept { keccak_init(RateBytes, 0x06u); }

    void update(const void* data, Size len) noexcept { keccak_absorb(data, len); }

    /// Записать дайджест (DigestSize байт) и вернуть объект в исходное
    /// состояние. Повторный вызов без нового update() даст дайджест пустого
    /// сообщения — это осознанное поведение (как у Sha2).
    void finalize(uint8_t* digest) noexcept {
        if (digest == nullptr) {
            return;
        }
        keccak_pad();
        keccak_squeeze(digest, DigestSize);
        reset();
    }

    static void hash(const void* data, Size len, uint8_t* digest) noexcept {
        Sha3<DigestBits> h;
        h.update(data, len);
        h.finalize(digest);
    }
};

/**
 * @brief SHAKE128/SHAKE256 — функция с расширяемым выходом (XOF).
 * @tparam SecurityBits 128 или 256.
 *
 * squeeze() можно вызывать многократно: поток продолжается, а не начинается
 * заново (прежняя версия на втором вызове дополняла губку повторно).
 */
template <unsigned SecurityBits>
class Shake : public detail::KeccakState {
    static_assert(SecurityBits == 128u || SecurityBits == 256u,
                  "SHAKE: поддерживаются 128 и 256 бит стойкости");

public:
    static constexpr Size RateBytes = (1600u - 2u * SecurityBits) / 8u;

    Shake() noexcept { reset(); }

    void reset() noexcept { keccak_init(RateBytes, 0x1Fu); }

    void update(const void* data, Size len) noexcept { keccak_absorb(data, len); }

    /// Выдать очередные len байт (при первом вызове губка дополняется).
    void squeeze(uint8_t* out, Size len) noexcept {
        if (out == nullptr || len == 0u) {
            return;
        }
        if (!finished()) {
            keccak_pad();
        }
        keccak_squeeze(out, len);
    }

    /// Однократный расчёт XOF.
    static void hash(const void* data, Size len,
                     uint8_t* out, Size out_len) noexcept {
        Shake<SecurityBits> h;
        h.update(data, len);
        h.squeeze(out, out_len);
    }
};

using Sha3_224 = Sha3<224u>;
using Sha3_256 = Sha3<256u>;
using Sha3_384 = Sha3<384u>;
using Sha3_512 = Sha3<512u>;

using Shake128 = Shake<128u>;
using Shake256 = Shake<256u>;

} // namespace crypto
} // namespace embed
