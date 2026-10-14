// crypto/tdes.cpp — DES/3DES: таблицы и реализации.
//
// Таблицы (IP, FP, PC1, PC2, E, P, S-блоки) вынесены в .cpp: они нужны
// ровно одному модулю и не должны дублироваться в каждой единице трансляции,
// которая включает заголовок.
#include "tdes.hpp"

namespace embed {
namespace crypto {
namespace {

// ─────────────────────────────── Таблицы DES ───────────────────────────────

/// Начальная перестановка (IP).
constexpr uint8_t kIp[64] = {
    58, 50, 42, 34, 26, 18, 10, 2,
    60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9,  1,
    59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5,
    63, 55, 47, 39, 31, 23, 15, 7
};

/// Финальная перестановка (FP, обратна IP).
constexpr uint8_t kFp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32,
    39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28,
    35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26,
    33, 1, 41, 9,  49, 17, 57, 25
};

/// Permuted Choice 1 (PC1): 64 → 56 бит.
constexpr uint8_t kPc1[56] = {
    57, 49, 41, 33, 25, 17, 9,
    1,  58, 50, 42, 34, 26, 18,
    10, 2,  59, 51, 43, 35, 27,
    19, 11, 3,  60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,
    7,  62, 54, 46, 38, 30, 22,
    14, 6,  61, 53, 45, 37, 29,
    21, 13, 5,  28, 20, 12, 4
};

/// Permuted Choice 2 (PC2): 56 → 48 бит.
constexpr uint8_t kPc2[48] = {
    14, 17, 11, 24, 1,  5,
    3,  28, 15, 6,  21, 10,
    23, 19, 12, 4,  26, 8,
    16, 7,  27, 20, 13, 2,
    41, 52, 31, 37, 47, 55,
    30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53,
    46, 42, 50, 36, 29, 32
};

/// Сдвиги половин ключа по раундам.
constexpr uint8_t kShiftBits[16] = {
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

/// Расширяющая перестановка E: 32 → 48 бит.
constexpr uint8_t kE[48] = {
    32, 1,  2,  3,  4,  5,
    4,  5,  6,  7,  8,  9,
    8,  9,  10, 11, 12, 13,
    12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21,
    20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29,
    28, 29, 30, 31, 32, 1
};

/// Перестановка P внутри функции Фейстеля.
constexpr uint8_t kP[32] = {
    16, 7,  20, 21,
    29, 12, 28, 17,
    1,  15, 23, 26,
    5,  18, 31, 10,
    2,  8,  24, 14,
    32, 27, 3,  9,
    19, 13, 30, 6,
    22, 11, 4,  25
};

/// S-блоки (8 штук по 64 значения).
constexpr uint8_t kSBoxes[8][64] = {
    {   // S1
        14, 4,  13, 1,  2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0,  7,
        0,  15, 7,  4,  14, 2,  13, 1,  10, 6,  12, 11, 9,  5,  3,  8,
        4,  1,  14, 8,  13, 6,  2,  11, 15, 12, 9,  7,  3,  10, 5,  0,
        15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0,  6,  13
    },
    {   // S2
        15, 1,  8,  14, 6,  11, 3,  4,  9,  7,  2,  13, 12, 0,  5,  10,
        3,  13, 4,  7,  15, 2,  8,  14, 12, 0,  1,  10, 6,  9,  11, 5,
        0,  14, 7,  11, 10, 4,  13, 1,  5,  8,  12, 6,  9,  3,  2,  15,
        13, 8,  10, 1,  3,  15, 4,  2,  11, 6,  7,  12, 0,  5,  14, 9
    },
    {   // S3
        10, 0,  9,  14, 6,  3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,
        13, 7,  0,  9,  3,  4,  6,  10, 2,  8,  5,  14, 12, 11, 15, 1,
        13, 6,  4,  9,  8,  15, 3,  0,  11, 1,  2,  12, 5,  10, 14, 7,
        1,  10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12
    },
    {   // S4
        7,  13, 14, 3,  0,  6,  9,  10, 1,  2,  8,  5,  11, 12, 4,  15,
        13, 8,  11, 5,  6,  15, 0,  3,  4,  7,  2,  12, 1,  10, 14, 9,
        10, 6,  9,  0,  12, 11, 7,  13, 15, 1,  3,  14, 5,  2,  8,  4,
        3,  15, 0,  6,  10, 1,  13, 8,  9,  4,  5,  11, 12, 7,  2,  14
    },
    {   // S5
        2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0,  14, 9,
        14, 11, 2,  12, 4,  7,  13, 1,  5,  0,  15, 10, 3,  9,  8,  6,
        4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,  6,  3,  0,  14,
        11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3
    },
    {   // S6
        12, 1,  10, 15, 9,  2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11,
        10, 15, 4,  2,  7,  12, 9,  5,  6,  1,  13, 14, 0,  11, 3,  8,
        9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10, 1,  13, 11, 6,
        4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13
    },
    {   // S7
        4,  11, 2,  14, 15, 0,  8,  13, 3,  12, 9,  7,  5,  10, 6,  1,
        13, 0,  11, 7,  4,  9,  1,  10, 14, 3,  5,  12, 2,  15, 8,  6,
        1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6,  8,  0,  5,  9,  2,
        6,  11, 13, 8,  1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12
    },
    {   // S8
        13, 2,  8,  4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,
        1,  15, 13, 8,  10, 3,  7,  4,  12, 5,  6,  11, 0,  14, 9,  2,
        7,  11, 4,  1,  9,  12, 14, 2,  0,  6,  10, 13, 15, 3,  5,  8,
        2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11
    }
};

// ─────────────────────────── Вспомогательные функции ───────────────────────

/// Перестановка бит: биты нумеруются с 1 от СТАРШЕГО бита входа.
uint64_t permute(uint64_t input, const uint8_t* table,
                 uint32_t input_bits, uint32_t output_bits) noexcept {
    uint64_t output = 0u;
    for (uint32_t i = 0; i < output_bits; ++i) {
        const uint32_t src = static_cast<uint32_t>(table[i]) - 1u;
        if ((input >> (input_bits - 1u - src)) & 1u) {
            output |= (1ull << (output_bits - 1u - i));
        }
    }
    return output;
}

/// Циклический сдвиг влево 28-битной половины ключа.
void rotate_left_28(uint32_t& value, uint32_t bits) noexcept {
    value = ((value << bits) | (value >> (28u - bits))) & 0x0FFFFFFFu;
}

uint64_t bytes_to_u64_be(const uint8_t* b) noexcept {
    uint64_t v = 0u;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<uint64_t>(b[i]);
    }
    return v;
}

void u64_be_to_bytes(uint64_t v, uint8_t* b) noexcept {
    for (int i = 7; i >= 0; --i) {
        b[i] = static_cast<uint8_t>(v & 0xFFu);
        v >>= 8;
    }
}

/// Функция Фейстеля: F(R, K).
uint32_t feistel(uint32_t right, uint64_t subkey) noexcept {
    const uint64_t expanded = permute(right, kE, 32u, 48u);
    const uint64_t mixed    = expanded ^ subkey;

    uint32_t sbox_out = 0u;
    for (int i = 0; i < 8; ++i) {
        const uint8_t six = static_cast<uint8_t>(
            (mixed >> (48u - static_cast<uint32_t>(i + 1) * 6u)) & 0x3Fu);
        // Строка — крайние биты (1-й и 6-й), столбец — средние четыре.
        const uint8_t row = static_cast<uint8_t>(
            ((six & 0x20u) >> 4) | (six & 0x01u));
        const uint8_t col = static_cast<uint8_t>((six >> 1u) & 0x0Fu);
        sbox_out = (sbox_out << 4) |
                   static_cast<uint32_t>(kSBoxes[i][row * 16u + col]);
    }

    return static_cast<uint32_t>(permute(sbox_out, kP, 32u, 32u));
}

/// Один блок DES: encrypt == true — прямой порядок раундов.
void des_block(uint8_t block[8], const uint64_t subkeys[16], bool encrypt) noexcept {
    uint64_t v = permute(bytes_to_u64_be(block), kIp, 64u, 64u);

    uint32_t left  = static_cast<uint32_t>(v >> 32);
    uint32_t right = static_cast<uint32_t>(v & 0xFFFFFFFFu);

    for (int i = 0; i < 16; ++i) {
        const int round = encrypt ? i : (15 - i);
        const uint32_t tmp = right;
        right = left ^ feistel(right, subkeys[round]);
        left  = tmp;
    }

    // После последнего раунда половины меняются местами (это и есть FP-вход).
    const uint64_t joined = (static_cast<uint64_t>(right) << 32) |
                             static_cast<uint64_t>(left);
    u64_be_to_bytes(permute(joined, kFp, 64u, 64u), block);
}

} // namespace

// ─────────────────────────────────── Des ───────────────────────────────────

Des::Des(const uint8_t key[KeyBytes]) noexcept : subkeys_{}, valid_(false) {
    if (key == nullptr) {
        return;
    }

    const uint64_t permuted = permute(bytes_to_u64_be(key), kPc1, 64u, 56u);
    uint32_t c = static_cast<uint32_t>((permuted >> 28) & 0x0FFFFFFFu);
    uint32_t d = static_cast<uint32_t>(permuted & 0x0FFFFFFFu);

    for (int i = 0; i < 16; ++i) {
        rotate_left_28(c, kShiftBits[i]);
        rotate_left_28(d, kShiftBits[i]);
        const uint64_t cd = (static_cast<uint64_t>(c) << 28) |
                             static_cast<uint64_t>(d);
        subkeys_[i] = permute(cd, kPc2, 56u, 48u);
    }

    valid_ = true;
}

void Des::encrypt_block(uint8_t block[BlockSize]) const noexcept {
    if (!valid_ || block == nullptr) {
        return;
    }
    des_block(block, subkeys_, true);
}

void Des::decrypt_block(uint8_t block[BlockSize]) const noexcept {
    if (!valid_ || block == nullptr) {
        return;
    }
    des_block(block, subkeys_, false);
}

void Des::clear() noexcept {
    wipe(subkeys_, sizeof(subkeys_));
    valid_ = false;
}

// ─────────────────────────────────── Tdes ──────────────────────────────────

Tdes::Tdes(const uint8_t key1[KeyBytes], const uint8_t key2[KeyBytes],
           const uint8_t key3[KeyBytes]) noexcept
    : des1_(key1), des2_(key2), des3_(key3),
      valid_(des1_.valid() && des2_.valid() && des3_.valid()) {}

Tdes::Tdes(const uint8_t key1[KeyBytes], const uint8_t key2[KeyBytes]) noexcept
    : des1_(key1), des2_(key2), des3_(key1),
      valid_(des1_.valid() && des2_.valid()) {}

Tdes::Tdes(const uint8_t key[KeyBytes]) noexcept
    : des1_(key), des2_(key), des3_(key), valid_(des1_.valid()) {}

void Tdes::encrypt_block(uint8_t block[BlockSize]) const noexcept {
    if (!valid_) {
        return;
    }
    des1_.encrypt_block(block);
    des2_.decrypt_block(block);
    des3_.encrypt_block(block);
}

void Tdes::decrypt_block(uint8_t block[BlockSize]) const noexcept {
    if (!valid_) {
        return;
    }
    des3_.decrypt_block(block);
    des2_.encrypt_block(block);
    des1_.decrypt_block(block);
}

void Tdes::clear() noexcept {
    des1_.clear();
    des2_.clear();
    des3_.clear();
    valid_ = false;
}

Status Tdes::encrypt(const uint8_t* input, Size input_len,
                     uint8_t* output, Size output_cap,
                     Size* output_len, Mode mode,
                     const uint8_t* ivOrNonce) const noexcept {
    if (input == nullptr || output == nullptr || output_len == nullptr) {
        return Status::InvalidArg;
    }
    if (!valid_) {
        return Status::InvalidArg;
    }
    if (mode != Mode::Ecb && ivOrNonce == nullptr) {
        return Status::InvalidArg;   // IV обязателен
    }

    // CTR — потоковый режим: длина не меняется, padding не применяется.
    if (mode == Mode::Ctr) {
        if (output_cap < input_len) {
            return Status::NoMemory;
        }
        uint8_t counter[BlockSize];
        utilities::fast_memcpy(counter, ivOrNonce, BlockSize);

        Size offset = 0u;
        while (offset < input_len) {
            uint8_t gamma[BlockSize];
            utilities::fast_memcpy(gamma, counter, BlockSize);
            encrypt_block(gamma);

            Size chunk = input_len - offset;
            if (chunk > BlockSize) {
                chunk = BlockSize;
            }
            for (Size i = 0; i < chunk; ++i) {
                output[offset + i] =
                    static_cast<uint8_t>(input[offset + i] ^ gamma[i]);
            }
            offset += chunk;

            for (int i = static_cast<int>(BlockSize) - 1; i >= 0; --i) {
                counter[static_cast<Size>(i)] =
                    static_cast<uint8_t>(counter[static_cast<Size>(i)] + 1u);
                if (counter[static_cast<Size>(i)] != 0u) {
                    break;
                }
            }
            wipe(gamma, sizeof(gamma));
        }
        wipe(counter, sizeof(counter));
        *output_len = input_len;
        return Status::Ok;
    }

    const Size needed = encrypted_size(input_len);
    if (output_cap < needed) {
        return Status::NoMemory;
    }

    const Size pad = pkcs7_padding_len(input_len, BlockSize);   // 1..8
    const Size padded_len = input_len + pad;

    uint8_t chain[BlockSize];
    if (ivOrNonce != nullptr) {
        utilities::fast_memcpy(chain, ivOrNonce, BlockSize);
    } else {
        utilities::fast_memset(chain, 0, BlockSize);
    }

    // Открытый текст с padding'ом режется на блоки 8 байт; байты за
    // пределами входа — это и есть PKCS#7-дополнение.
    for (Size offset = 0u; offset < padded_len; offset += BlockSize) {
        uint8_t block[BlockSize];
        for (Size i = 0u; i < BlockSize; ++i) {
            const Size pos = offset + i;
            if (pos < input_len) {
                block[i] = input[pos];
            } else {
                block[i] = static_cast<uint8_t>(pad);
            }
        }

        if (mode == Mode::Cbc) {
            for (Size i = 0u; i < BlockSize; ++i) {
                block[i] = static_cast<uint8_t>(block[i] ^ chain[i]);
            }
        }

        encrypt_block(block);

        if (mode == Mode::Cbc) {
            utilities::fast_memcpy(chain, block, BlockSize);
        }
        utilities::fast_memcpy(output + offset, block, BlockSize);
        wipe(block, sizeof(block));
    }

    wipe(chain, sizeof(chain));
    *output_len = padded_len;
    return Status::Ok;
}

Status Tdes::decrypt(const uint8_t* input, Size input_len,
                     uint8_t* output, Size output_cap,
                     Size* output_len, Mode mode,
                     const uint8_t* ivOrNonce) const noexcept {
    if (input == nullptr || output == nullptr || output_len == nullptr) {
        return Status::InvalidArg;
    }
    if (!valid_) {
        return Status::InvalidArg;
    }
    if (input_len == 0u) {
        // Пустой шифротекст: в ECB/CBC это не может быть результатом encrypt
        // (padding всегда даёт хотя бы один блок), в CTR — допустимо.
        if (mode == Mode::Ctr) {
            *output_len = 0u;
            return Status::Ok;
        }
        return Status::InvalidArg;
    }
    if (mode != Mode::Ctr && input_len % BlockSize != 0u) {
        return Status::InvalidArg;
    }
    if (mode != Mode::Ecb && ivOrNonce == nullptr) {
        return Status::InvalidArg;
    }
    if (output_cap < input_len) {
        return Status::NoMemory;
    }

    // CTR — потоковый режим, длина не меняется и padding'а нет.
    if (mode == Mode::Ctr) {
        uint8_t counter[BlockSize];
        utilities::fast_memcpy(counter, ivOrNonce, BlockSize);

        Size offset = 0u;
        while (offset < input_len) {
            uint8_t gamma[BlockSize];
            utilities::fast_memcpy(gamma, counter, BlockSize);
            encrypt_block(gamma);

            Size chunk = input_len - offset;
            if (chunk > BlockSize) {
                chunk = BlockSize;
            }
            for (Size i = 0; i < chunk; ++i) {
                output[offset + i] =
                    static_cast<uint8_t>(input[offset + i] ^ gamma[i]);
            }
            offset += chunk;

            for (int i = static_cast<int>(BlockSize) - 1; i >= 0; --i) {
                counter[static_cast<Size>(i)] =
                    static_cast<uint8_t>(counter[static_cast<Size>(i)] + 1u);
                if (counter[static_cast<Size>(i)] != 0u) {
                    break;
                }
            }
            wipe(gamma, sizeof(gamma));
        }
        wipe(counter, sizeof(counter));
        *output_len = input_len;
        return Status::Ok;
    }

    uint8_t chain[BlockSize];
    if (ivOrNonce != nullptr) {
        utilities::fast_memcpy(chain, ivOrNonce, BlockSize);
    } else {
        utilities::fast_memset(chain, 0, BlockSize);
    }

    Size offset = 0u;
    while (offset < input_len) {
        uint8_t block[BlockSize];
        utilities::fast_memcpy(block, input + offset, BlockSize);

        if (mode == Mode::Ecb) {
            decrypt_block(block);
        } else {
            uint8_t saved[BlockSize];
            utilities::fast_memcpy(saved, block, BlockSize);
            decrypt_block(block);
            for (Size i = 0u; i < BlockSize; ++i) {
                block[i] = static_cast<uint8_t>(block[i] ^ chain[i]);
            }
            utilities::fast_memcpy(chain, saved, BlockSize);
            wipe(saved, sizeof(saved));
        }

        utilities::fast_memcpy(output + offset, block, BlockSize);
        offset += BlockSize;
        wipe(block, sizeof(block));
    }

    // Шифротекст этого API всегда содержит padding PKCS#7: некорректный
    // padding означает неверный ключ/IV или порчу данных.
    Size plain_len = 0u;
    if (!pkcs7_unpad(output, input_len, BlockSize, &plain_len)) {
        wipe(chain, sizeof(chain));
        return Status::Corruption;
    }
    *output_len = plain_len;

    wipe(chain, sizeof(chain));
    return Status::Ok;
}

} // namespace crypto
} // namespace embed
