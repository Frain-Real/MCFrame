#pragma once
#include "../common.hpp"
#include "../utilities.hpp"
namespace embed {
// Общие определения
#define SHA256_BLOCK_SIZE  64  // 512 бит
#define SHA512_BLOCK_SIZE 128 // 1024 бит

// Размеры дайджестов
#define SHA224_DIGEST_SIZE 28
#define SHA256_DIGEST_SIZE 32
#define SHA384_DIGEST_SIZE 48
#define SHA512_DIGEST_SIZE 64
#define SHA512_224_DIGEST_SIZE 28
#define SHA512_256_DIGEST_SIZE 32

// --- Базовый класс для общей логики SHA-2 ---
class SHA2_Base {
public:
    // Виртуальный деструктор для корректного удаления
    virtual ~SHA2_Base() = default;

    // Инициализация хеш-функции
    void init();

    // Обновление хеша с новыми данными
    void update(const uint8_t* data, size_t length);

    // Завершение хеширования и получение результата
    // size_t digest_size - фактический размер дайджеста для конкретной версии
    void finalize(uint8_t* digest, size_t digest_size);

protected:
    // Конструктор -protected, так как класс базовый
    SHA2_Base(uint8_t block_size_bytes);

    uint8_t m_block_size_bytes; // Размер блока в байтах (64 или 128)
    uint8_t m_buffer[SHA512_BLOCK_SIZE]; // Максимальный буфер для 128 байт
    uint32_t m_buffer_idx;              // Индекс текущей позиции в буфере
    uint64_t m_bit_count;               // Общее количество обработанных бит

    // Чисто виртуальные функции, которые должны быть реализованы в производных классах
    virtual void init_hash_values() = 0; // Инициализация начальных H значений
    virtual void transform() = 0;        // Обработка одного блока
    virtual void get_digest_bytes(uint8_t* digest) = 0; // Копирование H значений в байты

    // Вспомогательная функция для дополнения (padding)
    void pad_message_and_transform();
};

// --- Семейство SHA-256 (32-битные операции) ---
class SHA256_Family : public SHA2_Base {
protected:
    SHA256_Family(); // Конструктор для наследования

    uint32_t m_h[8]; // Промежуточные значения хеша (H0 - H7)

    // Реализация чисто виртуальных функций из SHA2_Base
    void transform() override;
    void get_digest_bytes(uint8_t* digest) override;

    // Вспомогательные функции для битовых операций (32-битные)
    static uint32_t rotr32(uint32_t x, uint32_t n);
    static uint32_t shr32(uint32_t x, uint32_t n);
    static uint32_t ch32(uint32_t x, uint32_t y, uint32_t z);
    static uint32_t maj32(uint32_t x, uint32_t y, uint32_t z);
    static uint32_t sigma0_32(uint32_t x);
    static uint32_t sigma1_32(uint32_t x);
    static uint32_t e_sigma0_32(uint32_t x);
    static uint32_t e_sigma1_32(uint32_t x);
};

// --- Семейство SHA-512 (64-битные операции) ---
class SHA512_Family : public SHA2_Base {
protected:
    SHA512_Family(); // Конструктор для наследования

    uint64_t m_h[8]; // Промежуточные значения хеша (H0 - H7)

    // Реализация чисто виртуальных функций из SHA2_Base
    void transform() override;
    void get_digest_bytes(uint8_t* digest) override;

    // Вспомогательные функции для битовых операций (64-битные)
    static uint64_t rotr64(uint64_t x, uint64_t n);
    static uint64_t shr64(uint64_t x, uint64_t n);
    static uint64_t ch64(uint64_t x, uint64_t y, uint64_t z);
    static uint64_t maj64(uint64_t x, uint64_t y, uint64_t z);
    static uint64_t sigma0_64(uint64_t x);
    static uint64_t sigma1_64(uint64_t x);
    static uint64_t e_sigma0_64(uint64_t x);
    static uint64_t e_sigma1_64(uint64_t x);
};

// --- Конкретные реализации SHA-224, SHA-256 ---
class SHA224 : public SHA256_Family {
public:
    SHA224();
    void finalize(uint8_t digest[SHA224_DIGEST_SIZE]);
protected:
    void init_hash_values() override;
};

class SHA256 : public SHA256_Family {
public:
    SHA256();
    void finalize(uint8_t digest[SHA256_DIGEST_SIZE]);
protected:
    void init_hash_values() override;
};

// --- Конкретные реализации SHA-384, SHA-512, SHA-512/224, SHA-512/256 ---
class SHA384 : public SHA512_Family {
public:
    SHA384();
    void finalize(uint8_t digest[SHA384_DIGEST_SIZE]);
protected:
    void init_hash_values() override;
};

class SHA512 : public SHA512_Family {
public:
    SHA512();
    void finalize(uint8_t digest[SHA512_DIGEST_SIZE]);
protected:
    void init_hash_values() override;
};

class SHA512_224 : public SHA512_Family {
public:
    SHA512_224();
    void finalize(uint8_t digest[SHA512_224_DIGEST_SIZE]);
protected:
    void init_hash_values() override;
};

class SHA512_256 : public SHA512_Family {
public:
    SHA512_256();
    void finalize(uint8_t digest[SHA512_256_DIGEST_SIZE]);
protected:
    void init_hash_values() override;
};

// --- Константы SHA-256 ---
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xe4dbfacc, 0xf00327ed,
    0xf57c0faf, 0x6f07d2dc, 0x720a67ab, 0x81c4c92e, 0x9289297d, 0xa2c8c94e, 0xac42292a, 0xc55c7a1c,
    0xe479b7a1, 0xebf201e1, 0xfa7270f3, 0xfdc488c0, 0x2117559b, 0x33b006a1, 0x4785264b, 0x4cb0a9dc
};

// Начальные значения хеша SHA-256
static const uint32_t H256_INIT[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
};

// Начальные значения хеша SHA-224 (первые 8 простых чисел ^ 1/2, усеченные до 224 бит)
static const uint32_t H224_INIT[8] = {
    0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939,
    0xffc00b31, 0x68581511, 0x64f98fa7, 0xbefa4fa4
};

// --- Константы SHA-512 ---
static const uint64_t K512[92] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f4a337653ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2b6100c7ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bc9566fa1ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xe4dbfacc34ed1a4aULL, 0xf00327edb87dbfceULL,
    0xf57c0fafedbb4f8eULL, 0x6f07d2dc24477610ULL, 0x720a67ab766e257aULL, 0x81c4c92e47edaee6ULL,
    0x9289297d2871fbd4ULL, 0xa2c8c94e47d21384ULL, 0xac42292a434c449cULL, 0xc55c7a1c72a8c8e6ULL,
    0xc67178f2e3725a66ULL, 0xd1b020703f514869ULL, 0xd4d80a13d712e52eULL, 0xdd623f7215163777ULL,
    0xe0033286b245a498ULL, 0xe4b51c19b6ef72e1ULL, 0xeb07d2d3a3754b23ULL, 0xf769ea821cbfe980ULL,
    0xfc6543216893693fULL, 0xfd8655ed59e984f4ULL, 0x01a33a921ed0f4b3ULL, 0x0c681283d6a2f392ULL,
    0x147814a5118525b6ULL, 0x217088f1754f15d0ULL, 0x2748774c47d79b69ULL, 0x2d0c2e9b0b4b2397ULL,
    0x34b0bcb5e1719b36ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5370d919ULL, 0x78a5636f43e5c709ULL, 0x84c87814a1c5d985ULL,
    0x8cc702081a6439ecULL, 0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xa82b40b8a4f9bc08ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e3725a66ULL, 0xcc785bc3e2b7e192ULL, 0xe4dbfacc34ed1a4aULL,
    0xe53028198f2b2176ULL, 0xf769ea821cbfe980ULL, 0xfa7270f3ed02e077ULL, 0xfc6543216893693fULL
};

// Начальные значения хеша SHA-512
static const uint64_t H512_INIT[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

// Начальные значения хеша SHA-384
static const uint64_t H384_INIT[8] = {
    0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
    0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL
};

// Начальные значения хеша SHA-512/224
static const uint64_t H512_224_INIT[8] = {
    0x8c3d05260f898c6bULL, 0x741d402b8586cdd2ULL, 0x77037e37ed3c4606ULL, 0xee0a0d2e6097d9d0ULL,
    0x2c64baf86b723703ULL, 0x23ee733792c903a1ULL, 0xd0e01763741a8753ULL, 0x2213264996d51c10ULL
};

// Начальные значения хеша SHA-512/256
static const uint64_t H512_256_INIT[8] = {
    0x22312194fc2bf72cULL, 0x9f555fa3c84c64c2ULL, 0x2393b86b6f53b151ULL, 0x963877195940eabdULL,
    0x96283ee2a88effe3ULL, 0xbe5e1e2553863992ULL, 0x2b0199fc2c658461ULL, 0x0ab63df0a2c5f1cdULL
};

// --- Реализация SHA2_Base ---
SHA2_Base::SHA2_Base(uint8_t block_size_bytes) :
    m_block_size_bytes(block_size_bytes) {
    init();
}

void SHA2_Base::init() {
    init_hash_values(); // Вызывается виртуальная функция для инициализации H
    m_buffer_idx = 0;
    m_bit_count = 0;
}

void SHA2_Base::update(const uint8_t* data, size_t length) {
    m_bit_count += (uint64_t)length * 8; // Обновляем общее количество бит

    size_t data_idx = 0;
    while (length > 0) {
        size_t bytes_to_copy = m_block_size_bytes - m_buffer_idx;
        if (bytes_to_copy > length) {
            bytes_to_copy = length;
        }

        utilities::fast_memcpy(m_buffer + m_buffer_idx, data + data_idx, bytes_to_copy);
        m_buffer_idx += bytes_to_copy;
        data_idx += bytes_to_copy;
        length -= bytes_to_copy;

        if (m_buffer_idx == m_block_size_bytes) {
            transform(); // Вызывается виртуальная функция для обработки блока
            m_buffer_idx = 0;
        }
    }
}

void SHA2_Base::finalize(uint8_t* digest, size_t digest_size) {
    pad_message_and_transform(); // Выполняем дополнение и обработку последних блоков
    get_digest_bytes(digest);    // Копируем H значения в байты

    // Обрезаем хеш до нужного размера, если это необходимо
    if (digest_size < SHA512_DIGEST_SIZE && m_block_size_bytes == SHA512_BLOCK_SIZE) {
        // Для SHA-384, SHA-512/224, SHA-512/256
        // digest уже имеет необходимый размер, но мы должны обрезать его
        // (На самом деле, finalize будет принимать digest нужного размера,
        // поэтому копирование только нужного количества байт уже будет достаточно)
    } else if (digest_size < SHA256_DIGEST_SIZE && m_block_size_bytes == SHA256_BLOCK_SIZE) {
        // Для SHA-224
        // Аналогично, digest уже будет нужного размера
    }

    // Сбрасываем состояние для возможности повторного использования
    init();
}

void SHA2_Base::pad_message_and_transform() {
    // 1. Добавляем '1' бит (0x80)
    m_buffer[m_buffer_idx++] = 0x80;

    // 2. Если осталось недостаточно места для длины (8 байт для SHA256-семейства, 16 для SHA512-семейства),
    //    дополняем нулями и обрабатываем блок
    uint8_t length_field_size = (m_block_size_bytes == SHA256_BLOCK_SIZE) ? 8 : 16;
    if (m_buffer_idx > m_block_size_bytes - length_field_size) {
        utilities::fast_memset(m_buffer + m_buffer_idx, 0, m_block_size_bytes - m_buffer_idx);
        transform();
        m_buffer_idx = 0;
    }

    // 3. Дополняем нулями до последних 'length_field_size' байтов блока
    utilities::fast_memset(m_buffer + m_buffer_idx, 0, m_block_size_bytes - length_field_size - m_buffer_idx);

    // 4. Добавляем длину сообщения в битах (Big-endian)
    if (m_block_size_bytes == SHA256_BLOCK_SIZE) { // Для SHA-256/224 (8 байт длины)
        m_buffer[m_block_size_bytes - 8] = (uint8_t)((m_bit_count >> 56) & 0xFF);
        m_buffer[m_block_size_bytes - 7] = (uint8_t)((m_bit_count >> 48) & 0xFF);
        m_buffer[m_block_size_bytes - 6] = (uint8_t)((m_bit_count >> 40) & 0xFF);
        m_buffer[m_block_size_bytes - 5] = (uint8_t)((m_bit_count >> 32) & 0xFF);
        m_buffer[m_block_size_bytes - 4] = (uint8_t)((m_bit_count >> 24) & 0xFF);
        m_buffer[m_block_size_bytes - 3] = (uint8_t)((m_bit_count >> 16) & 0xFF);
        m_buffer[m_block_size_bytes - 2] = (uint8_t)((m_bit_count >> 8) & 0xFF);
        m_buffer[m_block_size_bytes - 1] = (uint8_t)(m_bit_count & 0xFF);
    } else { // Для SHA-512/384/512/224/512/256 (16 байт длины)
        // Общая длина сообщения в битах - это m_bit_count (uint64_t).
        // SHA-512 использует 128-битное представление длины, но с учетом того,
        // что m_bit_count - это uint64_t, старшие 64 бита (которые должны быть нулями) не используются.
        // Это обычно не проблема, так как длина сообщения редко превышает 2^64 бита.
        utilities::fast_memset(m_buffer + m_block_size_bytes - 16, 0, 8); // Старшие 8 байт для длины (обычно 0)
        m_buffer[m_block_size_bytes - 8]  = (uint8_t)((m_bit_count >> 56) & 0xFF);
        m_buffer[m_block_size_bytes - 7]  = (uint8_t)((m_bit_count >> 48) & 0xFF);
        m_buffer[m_block_size_bytes - 6]  = (uint8_t)((m_bit_count >> 40) & 0xFF);
        m_buffer[m_block_size_bytes - 5]  = (uint8_t)((m_bit_count >> 32) & 0xFF);
        m_buffer[m_block_size_bytes - 4]  = (uint8_t)((m_bit_count >> 24) & 0xFF);
        m_buffer[m_block_size_bytes - 3]  = (uint8_t)((m_bit_count >> 16) & 0xFF);
        m_buffer[m_block_size_bytes - 2]  = (uint8_t)((m_bit_count >> 8) & 0xFF);
        m_buffer[m_block_size_bytes - 1]  = (uint8_t)(m_bit_count & 0xFF);
    }

    // 5. Обрабатываем последний блок
    transform();
}

// --- Реализация SHA256_Family (32-битные операции) ---
SHA256_Family::SHA256_Family() : SHA2_Base(SHA256_BLOCK_SIZE) {}

// Вспомогательные функции (32-битные)
uint32_t SHA256_Family::rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}
uint32_t SHA256_Family::shr32(uint32_t x, uint32_t n) {
    return x >> n;
}
uint32_t SHA256_Family::ch32(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}
uint32_t SHA256_Family::maj32(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
uint32_t SHA256_Family::sigma0_32(uint32_t x) {
    return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}
uint32_t SHA256_Family::sigma1_32(uint32_t x) {
    return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}
uint32_t SHA256_Family::e_sigma0_32(uint32_t x) {
    return rotr32(x, 7) ^ rotr32(x, 18) ^ shr32(x, 3);
}
uint32_t SHA256_Family::e_sigma1_32(uint32_t x) {
    return rotr32(x, 17) ^ rotr32(x, 19) ^ shr32(x, 10);
}

void SHA256_Family::transform() {
    uint32_t W[64];

    for (int i = 0; i < 16; ++i) {
        W[i] = (uint32_t)m_buffer[i * 4] << 24 |
               (uint32_t)m_buffer[i * 4 + 1] << 16 |
               (uint32_t)m_buffer[i * 4 + 2] << 8 |
               (uint32_t)m_buffer[i * 4 + 3];
    }

    for (int i = 16; i < 64; ++i) {
        W[i] = e_sigma1_32(W[i - 2]) + W[i - 7] + e_sigma0_32(W[i - 15]) + W[i - 16];
    }

    uint32_t a = m_h[0];
    uint32_t b = m_h[1];
    uint32_t c = m_h[2];
    uint32_t d = m_h[3];
    uint32_t e = m_h[4];
    uint32_t f = m_h[5];
    uint32_t g = m_h[6];
    uint32_t h = m_h[7];

    uint32_t T1, T2;

    for (int i = 0; i < 64; ++i) {
        T1 = h + sigma1_32(e) + ch32(e, f, g) + K256[i] + W[i];
        T2 = sigma0_32(a) + maj32(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    m_h[0] += a;
    m_h[1] += b;
    m_h[2] += c;
    m_h[3] += d;
    m_h[4] += e;
    m_h[5] += f;
    m_h[6] += g;
    m_h[7] += h;
}

void SHA256_Family::get_digest_bytes(uint8_t* digest) {
    for (int i = 0; i < 8; ++i) {
        digest[i * 4]     = (uint8_t)((m_h[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((m_h[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((m_h[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = (uint8_t)(m_h[i] & 0xFF);
    }
}

// --- Реализация SHA512_Family (64-битные операции) ---
SHA512_Family::SHA512_Family() : SHA2_Base(SHA512_BLOCK_SIZE) {}

// Вспомогательные функции (64-битные)
uint64_t SHA512_Family::rotr64(uint64_t x, uint64_t n) {
    return (x >> n) | (x << (64 - n));
}
uint64_t SHA512_Family::shr64(uint64_t x, uint64_t n) {
    return x >> n;
}
uint64_t SHA512_Family::ch64(uint64_t x, uint64_t y, uint64_t z) {
    return (x & y) ^ (~x & z);
}
uint64_t SHA512_Family::maj64(uint64_t x, uint64_t y, uint64_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
uint64_t SHA512_Family::sigma0_64(uint64_t x) {
    return rotr64(x, 28) ^ rotr64(x, 34) ^ rotr64(x, 39);
}
uint64_t SHA512_Family::sigma1_64(uint64_t x) {
    return rotr64(x, 14) ^ rotr64(x, 18) ^ rotr64(x, 41);
}
uint64_t SHA512_Family::e_sigma0_64(uint64_t x) {
    return rotr64(x, 1) ^ rotr64(x, 8) ^ shr64(x, 7);
}
uint64_t SHA512_Family::e_sigma1_64(uint64_t x) {
    return rotr64(x, 19) ^ rotr64(x, 61) ^ shr64(x, 6);
}

void SHA512_Family::transform() {
    uint64_t W[80];

    // Копируем 16 слов (1024 бит) из буфера в W[0...15]
    for (int i = 0; i < 16; ++i) {
        W[i] = (uint64_t)m_buffer[i * 8] << 56 |
               (uint64_t)m_buffer[i * 8 + 1] << 48 |
               (uint64_t)m_buffer[i * 8 + 2] << 40 |
               (uint64_t)m_buffer[i * 8 + 3] << 32 |
               (uint64_t)m_buffer[i * 8 + 4] << 24 |
               (uint64_t)m_buffer[i * 8 + 5] << 16 |
               (uint64_t)m_buffer[i * 8 + 6] << 8 |
               (uint64_t)m_buffer[i * 8 + 7];
    }

    // Расширяем W[16...79]
    for (int i = 16; i < 80; ++i) {
        W[i] = e_sigma1_64(W[i - 2]) + W[i - 7] + e_sigma0_64(W[i - 15]) + W[i - 16];
    }

    uint64_t a = m_h[0];
    uint64_t b = m_h[1];
    uint64_t c = m_h[2];
    uint64_t d = m_h[3];
    uint64_t e = m_h[4];
    uint64_t f = m_h[5];
    uint64_t g = m_h[6];
    uint64_t h = m_h[7];

    uint64_t T1, T2;

    // Основной цикл сжатия (80 раундов)
    for (int i = 0; i < 80; ++i) {
        T1 = h + sigma1_64(e) + ch64(e, f, g) + K512[i] + W[i];
        T2 = sigma0_64(a) + maj64(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + T1;
        d = c;
        c = b;
        b = a;
        a = T1 + T2;
    }

    // Обновление промежуточных хеш-значений
    m_h[0] += a;
    m_h[1] += b;
    m_h[2] += c;
    m_h[3] += d;
    m_h[4] += e;
    m_h[5] += f;
    m_h[6] += g;
    m_h[7] += h;
}

void SHA512_Family::get_digest_bytes(uint8_t* digest) {
    for (int i = 0; i < 8; ++i) {
        digest[i * 8]     = (uint8_t)((m_h[i] >> 56) & 0xFF);
        digest[i * 8 + 1] = (uint8_t)((m_h[i] >> 48) & 0xFF);
        digest[i * 8 + 2] = (uint8_t)((m_h[i] >> 40) & 0xFF);
        digest[i * 8 + 3] = (uint8_t)((m_h[i] >> 32) & 0xFF);
        digest[i * 8 + 4] = (uint8_t)((m_h[i] >> 24) & 0xFF);
        digest[i * 8 + 5] = (uint8_t)((m_h[i] >> 16) & 0xFF);
        digest[i * 8 + 6] = (uint8_t)((m_h[i] >> 8) & 0xFF);
        digest[i * 8 + 7] = (uint8_t)(m_h[i] & 0xFF);
    }
}

// --- Реализация конкретных классов SHA256_Family ---
SHA224::SHA224() {} // Конструктор базового класса уже вызван
void SHA224::init_hash_values() { utilities::fast_memcpy(m_h, H224_INIT, sizeof(m_h)); }
void SHA224::finalize(uint8_t digest[SHA224_DIGEST_SIZE]) {
    SHA2_Base::finalize(digest, SHA224_DIGEST_SIZE);
}

SHA256::SHA256() {}
void SHA256::init_hash_values() { utilities::fast_memcpy(m_h, H256_INIT, sizeof(m_h)); }
void SHA256::finalize(uint8_t digest[SHA256_DIGEST_SIZE]) {
    SHA2_Base::finalize(digest, SHA256_DIGEST_SIZE);
}

// --- Реализация конкретных классов SHA512_Family ---
SHA384::SHA384() {}
void SHA384::init_hash_values() { utilities::fast_memcpy(m_h, H384_INIT, sizeof(m_h)); }
void SHA384::finalize(uint8_t digest[SHA384_DIGEST_SIZE]) {
    // SHA-384 использует начальные H-значения от SHA-512, но усекает вывод
    SHA2_Base::finalize(digest, SHA384_DIGEST_SIZE);
}

SHA512::SHA512() {}
void SHA512::init_hash_values() { utilities::fast_memcpy(m_h, H512_INIT, sizeof(m_h)); }
void SHA512::finalize(uint8_t digest[SHA512_DIGEST_SIZE]) {
    SHA2_Base::finalize(digest, SHA512_DIGEST_SIZE);
}

SHA512_224::SHA512_224() {}
void SHA512_224::init_hash_values() { utilities::fast_memcpy(m_h, H512_224_INIT, sizeof(m_h)); }
void SHA512_224::finalize(uint8_t digest[SHA512_224_DIGEST_SIZE]) {
    SHA2_Base::finalize(digest, SHA512_224_DIGEST_SIZE);
}

SHA512_256::SHA512_256() {}
void SHA512_256::init_hash_values() { utilities::fast_memcpy(m_h, H512_256_INIT, sizeof(m_h)); }
void SHA512_256::finalize(uint8_t digest[SHA512_256_DIGEST_SIZE]) {
    SHA2_Base::finalize(digest, SHA512_256_DIGEST_SIZE);
}
}