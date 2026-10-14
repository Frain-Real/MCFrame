// crypto/random.cpp — реализация CSPRNG на ChaCha20 (RFC 8439).
#include "random.hpp"

namespace embed {
namespace crypto {
namespace {

constexpr uint32_t kSigma[4] = {
    0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u   // "expand 32-byte k"
};

constexpr int kDoubleRounds = 10;   // 20 раундов ChaCha20

inline uint32_t rotl32(uint32_t x, uint32_t n) noexcept {
    return (x << n) | (x >> (32u - n));
}

inline void quarter_round(uint32_t& a, uint32_t& b,
                          uint32_t& c, uint32_t& d) noexcept {
    a += b; d ^= a; d = rotl32(d, 16u);
    c += d; b ^= c; b = rotl32(b, 12u);
    a += b; d ^= a; d = rotl32(d, 8u);
    c += d; b ^= c; b = rotl32(b, 7u);
}

/// Блок ChaCha20: out = 20 раундов(in) + in.
void chacha20_block(uint32_t out[16], const uint32_t in[16]) noexcept {
    uint32_t x[16];
    for (int i = 0; i < 16; ++i) {
        x[i] = in[i];
    }

    for (int i = 0; i < kDoubleRounds; ++i) {
        // Столбцы
        quarter_round(x[0], x[4], x[8],  x[12]);
        quarter_round(x[1], x[5], x[9],  x[13]);
        quarter_round(x[2], x[6], x[10], x[14]);
        quarter_round(x[3], x[7], x[11], x[15]);
        // Диагонали
        quarter_round(x[0], x[5], x[10], x[15]);
        quarter_round(x[1], x[6], x[11], x[12]);
        quarter_round(x[2], x[7], x[8],  x[13]);
        quarter_round(x[3], x[4], x[9],  x[14]);
    }

    for (int i = 0; i < 16; ++i) {
        out[i] = x[i] + in[i];
    }
}

} // namespace

void Rng::seed(const uint8_t key[KeySize],
               const uint8_t nonce[NonceSize]) noexcept {
    clear();
    if (key == nullptr) {
        return;
    }

    state_[0] = kSigma[0];
    state_[1] = kSigma[1];
    state_[2] = kSigma[2];
    state_[3] = kSigma[3];

    for (int i = 0; i < 8; ++i) {
        state_[4 + i] = load_le32(key + i * 4);
    }

    state_[12] = 0u;    // счётчик блока

    for (int i = 0; i < 3; ++i) {
        state_[13 + i] = (nonce != nullptr) ? load_le32(nonce + i * 4) : 0u;
    }

    ready_ = true;
}

void Rng::seed_from_words(uint8_t key_out[KeySize],
                          const uint32_t words[8]) noexcept {
    if (key_out == nullptr || words == nullptr) {
        return;
    }
    for (int i = 0; i < 8; ++i) {
        store_le32(words[i], key_out + i * 4);
    }
}

void Rng::clear() noexcept {
    // state_ затирается потоком из самого же state_: содержимое не должно
    // остаться в памяти после clear().
    for (int i = 0; i < 16; ++i) {
        state_[i] = 0u;
    }
    for (Size i = 0; i < BlockSize; ++i) {
        buffer_[i] = 0u;
    }
    available_ = 0u;
    ready_     = false;
}

void Rng::refill() noexcept {
    uint32_t block[16];
    chacha20_block(block, state_);

    for (int i = 0; i < 16; ++i) {
        store_le32(block[i], buffer_ + i * 4);
    }
    // Затираем рабочий блок: это тот же поток, что ушёл наружу, но
    // оставлять его на стеке незачем.
    wipe(block, sizeof(block));

    state_[12] += 1u;   // счётчик 32-битный по RFC 8439
    available_ = BlockSize;
}

void Rng::fill(void* out, Size len) noexcept {
    if (out == nullptr || len == 0u) {
        return;
    }
    uint8_t* dst = static_cast<uint8_t*>(out);

    if (!ready_) {
        // Не засеян: отдаём нули, но НЕ делаем вид, что это случайность.
        for (Size i = 0; i < len; ++i) {
            dst[i] = 0u;
        }
        return;
    }

    Size done = 0u;
    while (done < len) {
        if (available_ == 0u) {
            refill();
        }
        Size chunk = len - done;
        if (chunk > available_) {
            chunk = available_;
        }
        const Size start = BlockSize - available_;
        for (Size i = 0; i < chunk; ++i) {
            dst[done + i] = buffer_[start + i];
        }
        // Выданные байты потока больше не нужны.
        for (Size i = 0; i < chunk; ++i) {
            buffer_[start + i] = 0u;
        }
        available_ -= chunk;
        done       += chunk;
    }
}

uint32_t Rng::next_u32() noexcept {
    uint8_t b[4];
    fill(b, sizeof(b));
    return load_le32(b);
}

uint64_t Rng::next_u64() noexcept {
    uint8_t b[8];
    fill(b, sizeof(b));
    return load_le64(b);
}

bool Rng::next_below(uint32_t bound, uint32_t* out) noexcept {
    if (out == nullptr || bound == 0u || !ready_) {
        return false;
    }
    if (bound == 1u) {
        *out = 0u;
        return true;
    }

    // Отбрасываем значения из «хвоста», иначе младшие числа выпадали бы чаще.
    const uint32_t limit = 0xFFFFFFFFu - (0xFFFFFFFFu % bound);
    uint32_t r;
    do {
        r = next_u32();
    } while (r >= limit);

    *out = r % bound;
    return true;
}

bool Rng::next_range(uint32_t min, uint32_t max, uint32_t* out) noexcept {
    if (out == nullptr || min > max) {
        return false;
    }
    if (min == max) {
        *out = min;
        return true;
    }
    if (max - min == 0xFFFFFFFFu) {
        *out = next_u32();
        return ready_;
    }
    uint32_t v = 0u;
    if (!next_below(max - min + 1u, &v)) {
        return false;
    }
    *out = min + v;
    return true;
}

} // namespace crypto
} // namespace embed
