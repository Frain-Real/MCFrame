// crypto/random.hpp
// ---------------------------------------------------------------------------
// Криптостойкий генератор (CSPRNG) на ChaCha20 (RFC 8439).
//
// ЗАЧЕМ ОТДЕЛЬНЫЙ ГЕНЕРАТОР: в ядре уже есть embed::utilities::os_rand —
// это xorshift32, то есть НЕ криптостойкий генератор (по нескольким выходам
// восстанавливается состояние, а значит и все предыдущие). Для CRC, jitter'а
// и раздачи идентификаторов его достаточно; для ключей ECC/RSA — нет.
//
// ПОЧЕМУ ChaCha20: он в разы быстрее SHA-256-DRBG на Cortex-M3 (нет таблиц
// и умножений, только сложения/XOR/сдвиги), состояние 64 байта + буфер
// блока 64 байта, и он же даёт потоковый шифр для CTR-подобных задач.
//
// ВАЖНО: генератор наследует стойкость ЗЕРНА. Модуль не содержит источника
// энтропии (его знает только платформа: RNG периферии, шум АЦП, тайминг
// прерываний). Приложение обязано подать энтропию в seed():
//
//     uint8_t seed[embed::crypto::Rng::KeySize];
//     embed::crypto::Rng::seed_from_words(seed, entropy_words);  // 8 слов
//     g_rng.seed(seed);
//     embed::crypto::wipe(seed, sizeof(seed));
//
// Пока seed() не вызван, ready() == false, а fill()/next_*() возвращают нули:
// «тихо работающий предсказуемый ГПСЧ» — худший из возможных вариантов.
// ---------------------------------------------------------------------------
#pragma once

#include "crypto_utils.hpp"

#include <cstddef>
#include <cstdint>

namespace embed {
namespace crypto {

class Rng {
public:
    static constexpr Size KeySize   = 32u;
    static constexpr Size NonceSize = 12u;
    static constexpr Size BlockSize = 64u;

    Rng() noexcept { clear(); }

    Rng(const Rng&)            = delete;
    Rng& operator=(const Rng&) = delete;

    /// Задать ключ и nonce. nonce == nullptr ⇒ нулевой nonce
    /// (детерминированный поток для одной и той же пары ключ/nonce —
    /// поэтому для каждого сеанса нужен свой nonce).
    void seed(const uint8_t key[KeySize], const uint8_t nonce[NonceSize] = nullptr) noexcept;

    /// Разложить 8 слов «сырой» энтропии в 32-байтовый ключ (little-endian).
    static void seed_from_words(uint8_t key_out[KeySize],
                                const uint32_t words[8]) noexcept;

    /// Забыть состояние: генератор снова не готов. Ключ затирается.
    void clear() noexcept;

    bool ready() const noexcept { return ready_; }

    /// Заполнить буфер потоком генератора.
    void fill(void* out, Size len) noexcept;

    /// Следующее 32/64-битное слово.
    uint32_t next_u32() noexcept;
    uint64_t next_u64() noexcept;

    /// Равномерное целое из [0, bound) методом отклонения (без modulo bias).
    /// @return false при bound == 0 или если генератор не засеян.
    bool next_below(uint32_t bound, uint32_t* out) noexcept;

    /// Случайное целое из [min, max] включительно.
    bool next_range(uint32_t min, uint32_t max, uint32_t* out) noexcept;

private:
    uint32_t state_[16];    // constants|key|counter|nonce (RFC 8439)
    uint8_t  buffer_[BlockSize];
    Size     available_;    // невыданных байт в buffer_
    bool     ready_;

    /// Следующий блок потока (64 байта) в buffer_, available_ = BlockSize.
    void refill() noexcept;
};

} // namespace crypto
} // namespace embed
