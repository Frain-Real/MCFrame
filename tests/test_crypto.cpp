// tests/test_crypto.cpp
// ---------------------------------------------------------------------------
// Тесты модуля криптографии (mock-платформа).
//
// Каждая проверка — либо стандартный тестовый вектор (FIPS/NIST/RFC), либо
// фиксация исправленного дефекта. Эталонные значения взяты из:
//   * FIPS 180-4 / FIPS 202 — SHA-2, SHA-3, SHAKE;
//   * FIPS 197, NIST SP 800-38A — AES (ECB/CBC/CTR);
//   * FIPS 46-3 — DES (классический вектор), 3DES EDE;
//   * RFC 4231 — HMAC-SHA256;
//   * каталог CRC (reveng) — параметры и контрольные значения CRC;
//   * SEC 2 / FIPS 186-4 — тестовые точки secp256k1 и P-256.
//
// Запуск: ctest --test-dir .build/mock --output-on-failure
// ---------------------------------------------------------------------------
#include "os/crypto/crypto.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int g_failed = 0;
int g_total  = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

template <typename T>
void check_eq(const char* what, T got, T expected) {
    const bool ok = (got == expected);
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %-52s got=%llu expected=%llu\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<unsigned long long>(got),
                      static_cast<unsigned long long>(expected));
}

/// Сравнить байты с эталоном, заданным hex-строкой.
void check_hex(const char* what, const uint8_t* got, size_t len,
               const char* expected_hex) {
    char buf[256];
    static const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < len && (i * 2 + 2) < sizeof(buf); ++i) {
        buf[i * 2]     = digits[(got[i] >> 4) & 0xF];
        buf[i * 2 + 1] = digits[got[i] & 0xF];
    }
    buf[len * 2] = '\0';
    check(what, std::strcmp(buf, expected_hex) == 0);
    if (std::strcmp(buf, expected_hex) != 0) {
        (void)std::printf("        получено: %s\n        ожидалось: %s\n", buf, expected_hex);
    }
}

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Разобрать hex-строку в буфер; возвращает число байт.
/// Нечётная длина допустима: одиночная цифра — младший полубайт ("1" ⇒ 0x01).
size_t parse_hex(const char* hex, uint8_t* out, size_t cap) {
    const size_t digits = std::strlen(hex);
    const size_t bytes  = (digits + 1u) / 2u;
    const bool   odd    = (digits % 2u) != 0u;

    size_t n = 0;
    size_t i = 0;
    if (odd && bytes > 0u && n < cap) {
        out[n++] = static_cast<uint8_t>(hex_val(hex[0]));
        i = 1;
    }
    for (; i + 1u < digits && n < cap; i += 2) {
        out[n++] = static_cast<uint8_t>((hex_val(hex[i]) << 4) | hex_val(hex[i + 1]));
    }
    return n;
}

/// 256-битное число из hex-строки (big-endian; короткая строка не «сдвигает» разряды).
embed::BigInt<embed::crypto::EccWords> bigint_from_hex(const char* hex) {
    using embed::BigInt;
    uint8_t raw[32] = {};
    uint8_t aligned[32] = {};
    const size_t len = parse_hex(hex, raw, sizeof(raw));
    // Выравниваем по правому краю: "1" — это младший разряд.
    std::memcpy(aligned + (sizeof(aligned) - len), raw, len);

    BigInt<8> v;
    for (size_t i = 0; i < 8; ++i) {
        const size_t off = 28u - i * 4u;   // старшие байты — в начале строки
        v.d[i] = (static_cast<uint32_t>(aligned[off]) << 24) |
                 (static_cast<uint32_t>(aligned[off + 1u]) << 16) |
                 (static_cast<uint32_t>(aligned[off + 2u]) << 8) |
                  static_cast<uint32_t>(aligned[off + 3u]);
    }
    v.len = 8;
    v.neg = false;
    v.normalize();
    return v;
}

/// hex (big-endian) из 256-битного числа.
/// Собирается по символам: snprintf() дописывает '\0' после каждого поля и
/// при записи «справа налево» обрезал бы уже записанные слова.
void bigint_to_hex(const embed::BigInt<8>& v, char out[65]) {
    static const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < 8u; ++i) {
        const uint32_t w = (i < v.len) ? v.d[i] : 0u;
        const size_t off = (7u - i) * 8u;
        for (size_t j = 0; j < 8u; ++j) {
            out[off + j] = digits[(w >> (4u * (7u - j))) & 0xFu];
        }
    }
    out[64] = '\0';
}

// ──────────────────────────────── CRC ────────────────────────────────
void test_crc() {
    using namespace embed;
    using namespace embed::crypto;
    (void)std::puts("crc:");

    const char* data = "123456789";
    const Size len = 9u;

    check_eq<uint32_t>("CRC-8/SMBUS  (poly 0x07, init 0)",
                       Crc8SMBus::compute(data, len), 0xF4u);
    check_eq<uint32_t>("CRC-8/ROHC   (отражённый, init 0xFF)",
                       Crc8Rohc::compute(data, len), 0xD0u);
    check_eq<uint32_t>("CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF)",
                       Crc16CcittFalse::compute(data, len), 0x29B1u);
    check_eq<uint32_t>("CRC-16/MODBUS (отражённый)",
                       Crc16Modbus::compute(data, len), 0x4B37u);
    check_eq<uint32_t>("CRC-16/ARC   (отражённый, init 0)",
                       Crc16Arc::compute(data, len), 0xBB3Du);
    check_eq<uint32_t>("CRC-32/MPEG-2 (аппаратный CRC STM32)",
                       Crc32Mpeg2::compute(data, len), 0x0376E6E7u);
    check_eq<uint32_t>("CRC-32/ISO-HDLC (zlib/PNG)",
                       Crc32Zlib::compute(data, len), 0xCBF43926u);

    // Потоковое вычисление совпадает с однократным.
    uint32_t crc = Crc32Zlib::update(Crc32Zlib::init(), data, 4u);
    crc = Crc32Zlib::update(crc, data + 4, 5u);
    check_eq<uint32_t>("CRC-32: потоково == однократно",
                       static_cast<uint32_t>(Crc32Zlib::finish(crc)), 0xCBF43926u);

    // CRC пустого сообщения: init ^ xorout.
    check_eq<uint32_t>("CRC-32 (zlib): пустое сообщение == 0",
                       Crc32Zlib::compute(nullptr, 0u), 0x00000000u);

    uint16_t out = 0u;
    check("CRC: результат nullptr ⇒ InvalidArg",
          Crc16Modbus::calculate(nullptr, len, data) == Status::InvalidArg);
    check("CRC: данные nullptr при len > 0 ⇒ InvalidArg",
          Crc16Modbus::calculate(&out, len, nullptr) == Status::InvalidArg);
}

// ─────────────────────────────── SHA-2 ───────────────────────────────
void test_sha2() {
    using namespace embed::crypto;
    (void)std::puts("sha2:");

    uint8_t d[64];

    Sha224::hash("abc", 3u, d);
    check_hex("SHA-224(\"abc\")", d, 28,
              "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7");

    Sha256::hash("abc", 3u, d);
    check_hex("SHA-256(\"abc\")", d, 32,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    Sha384::hash("abc", 3u, d);
    check_hex("SHA-384(\"abc\")", d, 48,
              "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
              "8086072ba1e7cc2358baeca134c825a7");

    Sha512::hash("abc", 3u, d);
    check_hex("SHA-512(\"abc\")", d, 64,
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

    Sha512_224::hash("abc", 3u, d);
    check_hex("SHA-512/224(\"abc\")", d, 28,
              "4634270f707b6a54daae7530460842e20e37ed265ceee9a43e8924aa");

    Sha512_256::hash("abc", 3u, d);
    check_hex("SHA-512/256(\"abc\")", d, 32,
              "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23");

    Sha256::hash(nullptr, 0u, d);
    check_hex("SHA-256(\"\")", d, 32,
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    Sha512::hash(nullptr, 0u, d);
    check_hex("SHA-512(\"\")", d, 64,
              "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
              "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");

    // Границы padding'а: длина 55/56/64 и 111/112/128 байт.
    uint8_t msg[200];
    std::memset(msg, 'a', sizeof(msg));

    Sha256::hash(msg, 55u, d);
    check_hex("SHA-256(55×'a') — граница блока", d, 32,
              "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    Sha256::hash(msg, 56u, d);
    check_hex("SHA-256(56×'a') — padding в два блока", d, 32,
              "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    Sha256::hash(msg, 64u, d);
    check_hex("SHA-256(64×'a')", d, 32,
              "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    Sha256::hash(msg, 112u, d);
    check_hex("SHA-256(112×'a')", d, 32,
              "f54353008a2553262ecdc4a34749563ba0950e8b0fc8652780b0a614b99683c1");

    Sha512::hash(msg, 111u, d);
    check_hex("SHA-512(111×'a') — граница блока", d, 64,
              "fa9121c7b32b9e01733d034cfc78cbf67f926c7ed83e82200ef8681819692176"
              "0b4beff48404df811b953828274461673c68d04e297b0eb7b2b4d60fc6b566a2");
    Sha512::hash(msg, 112u, d);
    check_hex("SHA-512(112×'a') — padding в два блока", d, 64,
              "c01d080efd492776a1c43bd23dd99d0a2e626d481e16782e75d54c2503b5dc32"
              "bd05f0f1ba33e568b88fd2d970929b719ecbb152f58f130a407c8830604b70ca");

    // Потоковое обновление произвольными кусками == однократный расчёт.
    Sha256 h;
    h.update(msg, 7u);
    h.update(msg + 7, 0u);
    h.update(msg + 7, 13u);
    h.update(msg + 20, 92u);
    h.finalize(d);
    check_hex("SHA-256: потоково (7+13+92) == однократно", d, 32,
              "f54353008a2553262ecdc4a34749563ba0950e8b0fc8652780b0a614b99683c1");

    // Дайджест пишется РОВНО DigestSize байт: сторожевые байты не тронуты.
    uint8_t guarded[40];
    std::memset(guarded, 0xA5, sizeof(guarded));
    Sha224::hash("abc", 3u, guarded);
    bool intact = true;
    for (size_t i = 28u; i < sizeof(guarded); ++i) {
        if (guarded[i] != 0xA5u) intact = false;
    }
    check("SHA-224: буфер вызывающего не переполняется", intact);

    // «1 000 000 × 'a'» — классический вектор FIPS 180-4.
    static uint8_t million[1000000];
    std::memset(million, 'a', sizeof(million));
    Sha256::hash(million, sizeof(million), d);
    check_hex("SHA-256(10^6 × 'a')", d, 32,
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ─────────────────────────────── SHA-3 ───────────────────────────────
void test_sha3() {
    using namespace embed::crypto;
    (void)std::puts("sha3:");

    uint8_t d[64];

    Sha3_224::hash("abc", 3u, d);
    check_hex("SHA3-224(\"abc\")", d, 28,
              "e642824c3f8cf24ad09234ee7d3c766fc9a3a5168d0c94ad73b46fdf");

    Sha3_256::hash("abc", 3u, d);
    check_hex("SHA3-256(\"abc\")", d, 32,
              "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");

    Sha3_384::hash("abc", 3u, d);
    check_hex("SHA3-384(\"abc\")", d, 48,
              "ec01498288516fc926459f58e2c6ad8df9b473cb0fc08c2596da7cf0e49be4b2"
              "98d88cea927ac7f539f1edf228376d25");

    Sha3_512::hash("abc", 3u, d);
    check_hex("SHA3-512(\"abc\")", d, 64,
              "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
              "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0");

    // Границы губки: 135/136 байт для SHA3-256 (rate = 136).
    uint8_t msg[200];
    std::memset(msg, 'a', sizeof(msg));
    Sha3_256::hash(msg, 135u, d);
    check_hex("SHA3-256(135×'a') — граница rate", d, 32,
              "8094bb53c44cfb1e67b7c30447f9a1c33696d2463ecc1d9c92538913392843c9");
    Sha3_256::hash(msg, 136u, d);
    check_hex("SHA3-256(136×'a') — ровно один блок", d, 32,
              "3fc5559f14db8e453a0a3091edbd2bc25e11528d81c66fa570a4efdcc2695ee1");

    // Потоковое обновление.
    Sha3_256 h;
    h.update(msg, 11u);
    h.update(msg + 11, 124u);
    h.finalize(d);
    check_hex("SHA3-256: потоково == однократно", d, 32,
              "8094bb53c44cfb1e67b7c30447f9a1c33696d2463ecc1d9c92538913392843c9");

    // SHAKE: XOF задаёт выход произвольной длины.
    Shake128::hash(nullptr, 0u, d, 32u);
    check_hex("SHAKE128(\"\", 32)", d, 32,
              "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26");

    Shake256::hash(nullptr, 0u, d, 32u);
    check_hex("SHAKE256(\"\", 32)", d, 32,
              "46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762f");

    // Несколько squeeze() подряд — продолжение потока, а не новый расчёт.
    Shake128 x;
    uint8_t part1[16], part2[16], full[32];
    x.squeeze(part1, sizeof(part1));
    x.squeeze(part2, sizeof(part2));
    std::memcpy(full, part1, 16);
    std::memcpy(full + 16, part2, 16);
    check_hex("SHAKE128: два squeeze() == один вызов", full, 32,
              "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26");

    // Выдача длиннее rate (168 байт для SHAKE128) — губка переставляется.
    uint8_t long_out[400];
    Shake128::hash("abc", 3u, long_out, sizeof(long_out));
    uint8_t long_ref[400];
    Shake128 y;
    y.update("abc", 3u);
    y.squeeze(long_ref, sizeof(long_ref));
    check("SHAKE128: 400 байт совпадают при двух способах вызова",
          std::memcmp(long_out, long_ref, sizeof(long_out)) == 0);

    // Rate не должен «залипать»: мусор за пределами rate не влияет.
    check_eq<uint32_t>("SHA3-256: rate == 136 байт",
                       static_cast<uint32_t>(Sha3_256::RateBytes), 136u);
    check_eq<uint32_t>("SHAKE128: rate == 168 байт",
                       static_cast<uint32_t>(Shake128::RateBytes), 168u);
}

// ─────────────────────────── HMAC-SHA256 ───────────────────────────
void test_hmac() {
    using namespace embed::crypto;
    (void)std::puts("hmac:");

    uint8_t tag[32];

    // RFC 4231, тест 1: ключ 20×0x0b, данные "Hi There".
    uint8_t key[20];
    std::memset(key, 0x0b, sizeof(key));
    hmac_sha256(key, sizeof(key), "Hi There", 8u, tag);
    check_hex("HMAC-SHA256, RFC 4231 тест 1", tag, 32,
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // RFC 4231, тест 2: ключ "Jefe".
    hmac_sha256("Jefe", 4u, "what do ya want for nothing?", 28u, tag);
    check_hex("HMAC-SHA256, RFC 4231 тест 2", tag, 32,
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // Потоковый расчёт == однократный.
    HmacSha256 mac("Jefe", 4u);
    mac.update("what do ya ", 11u);
    mac.update("want for nothing?", 17u);
    mac.finalize(tag);
    check_hex("HMAC-SHA256: потоково == однократно", tag, 32,
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    check("HMAC: проверка верного тега",
          hmac_sha256_verify("Jefe", 4u, "what do ya want for nothing?", 28u, tag));

    tag[0] ^= 0x01u;
    check("HMAC: подмена тега обнаруживается",
          !hmac_sha256_verify("Jefe", 4u, "what do ya want for nothing?", 28u, tag));
}

// ───────────────────────────────── AES ─────────────────────────────────
void test_aes() {
    using namespace embed;
    using namespace embed::crypto;
    (void)std::puts("aes:");

    uint8_t key128[16], key192[24], key256[32], iv[16], plain[16], out[128];
    parse_hex("000102030405060708090a0b0c0d0e0f", key128, sizeof(key128));
    parse_hex("000102030405060708090a0b0c0d0e0f1011121314151617", key192, sizeof(key192));
    parse_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
              key256, sizeof(key256));
    parse_hex("00112233445566778899aabbccddeeff", plain, sizeof(plain));
    parse_hex("000102030405060708090a0b0c0d0e0f", iv, sizeof(iv));

    Size out_len = 0u;

    // FIPS 197, приложение C — ECB без padding.
    {
        Aes128Ecb aes(key128);
        check("AES-128-ECB: encrypt", aes.encrypt(plain, 16u, out, sizeof(out), &out_len) == Status::Ok);
        check_hex("AES-128-ECB шифротекст", out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a");

        uint8_t back[16];
        Size back_len = 0u;
        check("AES-128-ECB: decrypt", aes.decrypt(out, 16u, back, sizeof(back), &back_len) == Status::Ok);
        check("AES-128-ECB: round-trip", std::memcmp(back, plain, 16) == 0);
    }
    {
        Aes<AesKeySize::Aes192, AesMode::Ecb, true> aes192(key192);
        aes192.encrypt(plain, 16u, out, sizeof(out), &out_len);
        check_hex("AES-192-ECB шифротекст", out, 16, "dda97ca4864cdfe06eaf70a0ec0d7191");
    }
    {
        Aes256Ecb aes256(key256);
        aes256.encrypt(plain, 16u, out, sizeof(out), &out_len);
        check_hex("AES-256-ECB шифротекст", out, 16, "8ea2b7ca516745bfeafc49904b496089");
    }

    // NIST SP 800-38A, F.2.1 — CBC (4 блока, без padding).
    {
        uint8_t pt[64], ct_ref[64];
        parse_hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                  "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
                  pt, sizeof(pt));
        parse_hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                  "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7",
                  ct_ref, sizeof(ct_ref));

        // Внимание: вектор NIST SP 800-38A использует ДРУГОЙ ключ, чем
        // FIPS 197 (там проверялся AES-128 на ключе 00..0f).
        uint8_t nist_key[16];
        parse_hex("2b7e151628aed2a6abf7158809cf4f3c", nist_key, sizeof(nist_key));

        Aes128Cbc aes(nist_key, iv);
        check("AES-128-CBC: encrypt", aes.encrypt(pt, 64u, out, sizeof(out), &out_len) == Status::Ok);
        check_hex("AES-128-CBC шифротекст (NIST F.2.1)", out, 64,
                  "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                  "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");

        uint8_t back[64];
        Size back_len = 0u;
        check("AES-128-CBC: decrypt", aes.decrypt(ct_ref, 64u, back, sizeof(back), &back_len) == Status::Ok);
        check("AES-128-CBC: round-trip", std::memcmp(back, pt, 64) == 0);

        // Шифрование «на месте» даёт тот же результат.
        uint8_t inplace[64];
        std::memcpy(inplace, pt, sizeof(inplace));
        Aes128Cbc aes2(nist_key, iv);
        aes2.encrypt(inplace, 64u, inplace, sizeof(inplace), &out_len);
        check("AES-128-CBC: работа на месте", std::memcmp(inplace, ct_ref, 64) == 0);

        // Мало места в буфере — Status::NoMemory, а не порча памяти.
        uint8_t small[8];
        check("AES-128-CBC: малый буфер ⇒ NoMemory",
              aes.encrypt(pt, 64u, small, sizeof(small), &out_len) == Status::NoMemory);
        check("AES-128-CBC: длина не кратна блоку ⇒ InvalidArg",
              aes.encrypt(pt, 20u, out, sizeof(out), &out_len) == Status::InvalidArg);
    }

    // NIST SP 800-38A, F.5.1 — CTR.
    {
        uint8_t pt[64], ctr[16];
        parse_hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                  "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
                  pt, sizeof(pt));
        parse_hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", ctr, sizeof(ctr));

        // ВАЖНО: EnableDecrypt == false — расшифрование CTR всё равно работает
        // (раньше возвращало false всегда).
        uint8_t nist_key[16];
        parse_hex("2b7e151628aed2a6abf7158809cf4f3c", nist_key, sizeof(nist_key));

        Aes128Ctr aes(nist_key, ctr);
        check("AES-128-CTR: encrypt", aes.encrypt(pt, 64u, out, sizeof(out), &out_len) == Status::Ok);
        check_hex("AES-128-CTR шифротекст (NIST F.5.1)", out, 64,
                  "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
                  "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");

        uint8_t back[64];
        Size back_len = 0u;
        check("AES-128-CTR: decrypt при EnableDecrypt=false",
              aes.decrypt(out, 64u, back, sizeof(back), &back_len) == Status::Ok);
        check("AES-128-CTR: round-trip", std::memcmp(back, pt, 64) == 0);
        check("AES-128-CTR: длина не меняется (padding не добавляется)",
              Aes128Ctr::encrypted_size(7u) == 7u);
    }

    // PKCS#7: длина, не кратная блоку.
    {
        const char* text = "MCFrame: секретное сообщение";
        const Size text_len = std::strlen(text);
        Aes128CbcPkcs7 aes(key128, iv);

        check("AES-128-CBC+PKCS7: encrypt",
              aes.encrypt(reinterpret_cast<const uint8_t*>(text), text_len,
                          out, sizeof(out), &out_len) == Status::Ok);
        check_eq<uint32_t>("AES-128-CBC+PKCS7: длина шифротекста",
                           static_cast<uint32_t>(out_len),
                           static_cast<uint32_t>(pkcs7_padded_size(text_len, 16u)));

        uint8_t back[64];
        Size back_len = 0u;
        check("AES-128-CBC+PKCS7: decrypt",
              aes.decrypt(out, out_len, back, sizeof(back), &back_len) == Status::Ok);
        check_eq<uint32_t>("AES-128-CBC+PKCS7: длина открытого текста",
                           static_cast<uint32_t>(back_len), static_cast<uint32_t>(text_len));
        check("AES-128-CBC+PKCS7: round-trip",
              std::memcmp(back, text, text_len) == 0);

        // Порча шифротекста (неверный ключ) — PaddingError, не «тихо мусор».
        uint8_t wrong_key[16];
        std::memcpy(wrong_key, key128, sizeof(wrong_key));
        wrong_key[0] ^= 0xFFu;
        Aes128CbcPkcs7 bad(wrong_key, iv);
        check("AES-128-CBC+PKCS7: неверный ключ ⇒ Corruption",
              bad.decrypt(out, out_len, back, sizeof(back), &back_len) == Status::Corruption);

        // Длина вектора 16 байт кратна блоку ⇒ добавляется целый блок.
        check_eq<uint32_t>("AES: GetPaddedSize(16) == 32",
                           static_cast<uint32_t>(Aes128CbcPkcs7::encrypted_size(16u)), 32u);
    }

    // Ключ не задан — операции отклоняются, а не работают «на мусоре».
    {
        Aes128CbcPkcs7 aes_null(nullptr, iv);
        check("AES: nullptr-ключ ⇒ InvalidArg",
              aes_null.encrypt(plain, 16u, out, sizeof(out), &out_len) == Status::InvalidArg);
    }
}

// ───────────────────────────────── TDES ─────────────────────────────────
void test_tdes() {
    using namespace embed;
    using namespace embed::crypto;
    (void)std::puts("tdes:");

    // key1 — для классического вектора DES; пара (key3, key2) задаёт
    // двухключевой 3DES из эталонных значений: K1 = K3, K2.
    uint8_t key1[8], key2[8], key3[8];
    parse_hex("133457799BBCDFF1", key1, sizeof(key1));
    parse_hex("FEDCBA9876543210", key2, sizeof(key2));
    parse_hex("0123456789ABCDEF", key3, sizeof(key3));

    // Классический вектор DES (FIPS 46-3).
    {
        uint8_t pt[8];
        parse_hex("0123456789ABCDEF", pt, sizeof(pt));

        Des des(key1);
        check("DES: ключ принят", des.valid());
        des.encrypt_block(pt);
        check_hex("DES: шифрование (классический вектор)", pt, 8, "85e813540f0ab405");
        des.decrypt_block(pt);
        check_hex("DES: расшифрование", pt, 8, "0123456789abcdef");

        // TDES с одним ключом (K1=K2=K3) обязан совпасть с одиночным DES.
        uint8_t pt2[8];
        parse_hex("0123456789ABCDEF", pt2, sizeof(pt2));
        Tdes tdes_one(key1);
        tdes_one.encrypt_block(pt2);
        check_hex("3DES с одним ключом == DES (EDE вырождается)", pt2, 8, "85e813540f0ab405");
        tdes_one.decrypt_block(pt2);
        check_hex("3DES с одним ключом: расшифрование", pt2, 8, "0123456789abcdef");
    }

    // 3DES EDE, три ключа (проверено отдельной реализацией).
    {
        uint8_t pt[8];
        parse_hex("0123456789ABCDEF", pt, sizeof(pt));
        Tdes tdes(key3, key2, key3);
        tdes.encrypt_block(pt);
        check_hex("3DES-EDE: шифрование блока", pt, 8, "1a4d672dca6cb335");
        tdes.decrypt_block(pt);
        check_hex("3DES-EDE: расшифрование блока", pt, 8, "0123456789abcdef");
    }

    // ECB с padding'ом: длина кратна 8 ⇒ добавляется целый блок.
    {
        uint8_t pt[8], out[64];
        parse_hex("0123456789ABCDEF", pt, sizeof(pt));
        Tdes tdes(key3, key2, key3);
        Size out_len = 0u;

        check("TDES-ECB: encrypt", tdes.encrypt(pt, 8u, out, sizeof(out), &out_len,
                                                Tdes::Mode::Ecb) == Status::Ok);
        check_eq<uint32_t>("TDES-ECB: длина = 16 (padding-блок)",
                           static_cast<uint32_t>(out_len), 16u);
        check_hex("TDES-ECB: первый блок — известный вектор", out, 8, "1a4d672dca6cb335");

        // Раньше байты 8..63 каждого 64-байтного «блока» уходили ОТКРЫТЫМИ
        // (BLOCK_SIZE = 64 бита использовался как 64 байта). Проверяем
        // шифрование всех 8 блоков по эталону openssl -des-ede3-ecb.
        uint8_t pt64[64], out64[128];
        parse_hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                  "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
                  pt64, sizeof(pt64));
        Size len64 = 0u;
        check("TDES-ECB: encrypt 64 байта",
              tdes.encrypt(pt64, sizeof(pt64), out64, sizeof(out64), &len64,
                           Tdes::Mode::Ecb) == Status::Ok);
        check_hex("TDES-ECB: все 8 блоков (эталон openssl)", out64, 72,
                  "ea43f9aa27f82f595a3e3056e1710d3492d9a7b73ab3b15840eaf7b333e9740c"
                  "19043c5d20d87bcfacc85afcc2bf1b7b4e0d97a6f49e5dd2a2a15e82a6a60922"
                  "2e24eeb85aef49ae");

        uint8_t back[128];
        Size back_len = 0u;
        check("TDES-ECB: decrypt", tdes.decrypt(out64, len64, back, sizeof(back),
                                                &back_len, Tdes::Mode::Ecb) == Status::Ok);
        check_eq<uint32_t>("TDES-ECB: длина открытого текста",
                           static_cast<uint32_t>(back_len), 64u);
        check("TDES-ECB: round-trip", std::memcmp(back, pt64, 64) == 0);
    }

    // CBC: 64 байта с нулевым IV (сверено с независимой реализацией).
    {
        uint8_t pt[64], iv[8], out[128];
        parse_hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                  "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
                  pt, sizeof(pt));
        std::memset(iv, 0, sizeof(iv));

        Tdes tdes(key3, key2, key3);
        Size out_len = 0u;
        check("TDES-CBC: encrypt", tdes.encrypt(pt, 64u, out, sizeof(out), &out_len,
                                                Tdes::Mode::Cbc, iv) == Status::Ok);
        // Эталон: openssl enc -des-ede3-cbc -K <3 ключа> -iv 0 (PKCS#7).
        check_hex("TDES-CBC: шифротекст (4 блока + padding)", out, 72,
                  "ea43f9aa27f82f599735d93d56b5449ec2e751f52363e6fb8631775c1110429f"
                  "3be50890fbfac5639d7f84fa523fb9d32238b310aa1572895403551570b6541b"
                  "aa011e1602aab8a0");

        uint8_t back[128];
        Size back_len = 0u;
        check("TDES-CBC: decrypt", tdes.decrypt(out, out_len, back, sizeof(back),
                                                &back_len, Tdes::Mode::Cbc, iv) == Status::Ok);
        check_eq<uint32_t>("TDES-CBC: длина открытого текста",
                           static_cast<uint32_t>(back_len), 64u);
        check("TDES-CBC: round-trip", std::memcmp(back, pt, 64) == 0);

        // CBC НЕ даёт целостности: неверный IV портит только ПЕРВЫЙ блок,
        // последний блок (и его padding) остаются корректными. Поэтому
        // шифровать «в чистом виде» CBC нельзя — нужен MAC/HMAC.
        uint8_t bad_iv[8];
        std::memset(bad_iv, 0xAA, sizeof(bad_iv));
        check("TDES-CBC: неверный IV ⇒ decrypt проходит (нет целостности)",
              tdes.decrypt(out, out_len, back, sizeof(back), &back_len,
                           Tdes::Mode::Cbc, bad_iv) == Status::Ok);
        check("TDES-CBC: при неверном IV испорчен первый блок",
              back_len == 64u && std::memcmp(back, pt, 8) != 0);
        check("TDES-CBC: остальные блоки при неверном IV сохраняются",
              std::memcmp(back + 8, pt + 8, 56) == 0);

        // А порча ЛЮБОГО блока шифротекста ломает padding последнего блока.
        uint8_t broken[128];
        std::memcpy(broken, out, out_len);
        broken[out_len - 1u] ^= 0x01u;
        check("TDES-CBC: порча шифротекста ⇒ Corruption",
              tdes.decrypt(broken, out_len, back, sizeof(back), &back_len,
                           Tdes::Mode::Cbc, iv) == Status::Corruption);
        check("TDES-CBC: без IV ⇒ InvalidArg",
              tdes.encrypt(pt, 64u, out, sizeof(out), &out_len, Tdes::Mode::Cbc) == Status::InvalidArg);
    }

    // CTR: гамма — шифрование счётчика (значения сверены независимо).
    {
        uint8_t zeros[64], iv[8];
        std::memset(zeros, 0, sizeof(zeros));
        parse_hex("0001020304050607", iv, sizeof(iv));

        Tdes tdes(key3, key2, key3);
        Size out_len = 0u;
        uint8_t out[80];
        check("TDES-CTR: encrypt", tdes.encrypt(zeros, 64u, out, sizeof(out), &out_len,
                                                Tdes::Mode::Ctr, iv) == Status::Ok);
        check_eq<uint32_t>("TDES-CTR: длина не меняется",
                           static_cast<uint32_t>(out_len), 64u);
        check_hex("TDES-CTR: гамма для нулевого текста", out, 64,
                  "52c5c0705d9089e1300309d984ff55263503f27b085aa7798881f7428eb35346"
                  "5c9d7a9263c5a92fcd28440ef8b6300a1338850819b3da5d191c7e92a824d7cb");

        // Частичный последний блок (7 байт) — длина сохраняется.
        uint8_t partial[7] = {1, 2, 3, 4, 5, 6, 7};
        uint8_t enc7[7], dec7[7];
        Size len7 = 0u, back7 = 0u;
        tdes.encrypt(partial, sizeof(partial), enc7, sizeof(enc7), &len7,
                     Tdes::Mode::Ctr, iv);
        check_eq<uint32_t>("TDES-CTR: длина 7 байт сохраняется",
                           static_cast<uint32_t>(len7), 7u);
        tdes.decrypt(enc7, len7, dec7, sizeof(dec7), &back7, Tdes::Mode::Ctr, iv);
        check("TDES-CTR: round-trip 7 байт",
              back7 == 7u && std::memcmp(dec7, partial, 7) == 0);

        // Мало места ⇒ NoMemory.
        check("TDES-CTR: малый буфер ⇒ NoMemory",
              tdes.encrypt(zeros, 64u, out, 8u, &out_len, Tdes::Mode::Ctr, iv) == Status::NoMemory);
    }
}

// ───────────────────────────────── RSA ─────────────────────────────────
void test_rsa() {
    using namespace embed;
    using namespace embed::crypto;
    (void)std::puts("rsa:");

    // Классический учебный пример: p=61, q=53, n=3233, e=17, d=413.
    {
        Rsa<uint32_t> rsa;
        check("RSA: set_key_pair(3233, 17, 413)", rsa.set_key_pair(3233u, 17u, 413u));

        uint32_t cipher = 0u, plain = 0u;
        check("RSA: encrypt(65)", rsa.encrypt(65u, &cipher));
        check_eq<uint32_t>("RSA: 65^17 mod 3233 == 2790", cipher, 2790u);
        check("RSA: decrypt(2790)", rsa.decrypt(cipher, &plain));
        check_eq<uint32_t>("RSA: round-trip", plain, 65u);

        check("RSA: сообщение >= n отклоняется", !rsa.encrypt(3233u, &cipher));
        check("RSA: некорректный ключ отклоняется",
              !rsa.set_public_key(1u, 3u));
    }

    // Генерация ключей на CSPRNG: проверяем работоспособность пары.
    {
        uint8_t seed[Rng::KeySize];
        uint32_t words[8] = {0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u,
                             0x0F1E2D3Cu, 0x4B5A6978u, 0x8796A5B4u, 0xC3D2E1F0u};
        Rng::seed_from_words(seed, words);

        Rng rng;
        rng.seed(seed);

        Rsa<uint32_t> rsa;
        check("RSA: generate_keys (простые 1000..4000)",
              rsa.generate_keys(1000u, 4000u, rng));

        bool ok = rsa.has_public_key() && rsa.has_private_key() && rsa.modulus() > 1000u;
        check("RSA: ключи заполнены", ok);

        bool roundtrip_ok = true;
        for (uint32_t m = 1u; m < 50u; ++m) {
            uint32_t c = 0u, back = 0u;
            if (!rsa.encrypt(m, &c) || !rsa.decrypt(c, &back) || back != m) {
                roundtrip_ok = false;
                break;
            }
        }
        check("RSA: round-trip для m = 1..49", roundtrip_ok);

        // Модуль обязан умещаться в Uint: иначе произведение p*q «свернулось».
        check("RSA: модуль не переполнен", rsa.modulus() > 1000u);
    }

    // Без CSPRNG (не засеян) генерация запрещена.
    {
        Rng empty;
        Rsa<uint32_t> rsa;
        check("RSA: генерация без CSPRNG запрещена",
              !rsa.generate_keys(100u, 200u, empty));
    }
}

// ───────────────────────────────── ECC ─────────────────────────────────
void test_ecc() {
    using namespace embed;
    using namespace embed::crypto;
    (void)std::puts("ecc:");

    using K1 = Ecdh<EccCurve::Secp256k1>;
    using R1 = Ecdh<EccCurve::Secp256r1>;

    // Базовая точка лежит на кривой.
    check("secp256k1: G на кривой", K1::is_on_curve(K1::generator()));
    check("secp256r1: G на кривой", R1::is_on_curve(R1::generator()));

    // Точка бесконечности и точка вне кривой отвергаются.
    check("ECC: бесконечность не является публичным ключом",
          !K1::is_on_curve(EccPoint()));
    {
        EccPoint bad(K1::generator().x, K1::generator().y);
        bad.y = bad.y + BigInt<8>(1u);      // сдвиг по y выводит из кривой
        check("ECC: точка вне кривой отвергается", !K1::is_on_curve(bad));
    }

    // Эталонные публичные ключи (получены независимой реализацией).
    struct Vector {
        const char* d;
        const char* x;
        const char* y;
    };
    const Vector k1_vectors[] = {
        {"1",
         "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798",
         "483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8"},
        {"2",
         "c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5",
         "1ae168fea63dc339a3c58419466ceaeef7f632653266d0e1236431a950cfe52a"},
        {"3",
         "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9",
         "388f7b0f632de8140fe337e62a37f3566500a99934c2231b6cb9fd7584b8e672"},
        {"1234567890abcdef",
         "f973a0b87062c389d125d8199e803b832b6ac6bf7867a4f6cd87506060fc4c58",
         "4b4a0a3f26c988c54c236b224c48bb605b265949e65c098ecd87a581ca10e25d"}
    };
    for (const Vector& v : k1_vectors) {
        K1::Point pub;
        const bool ok = K1::compute_public_key(bigint_from_hex(v.d), pub);
        char x[65], y[65];
        bigint_to_hex(pub.x, x);
        bigint_to_hex(pub.y, y);
        char what[96];
        (void)std::snprintf(what, sizeof(what), "secp256k1: pub(d=%s) x", v.d);
        check(what, ok && std::strcmp(x, v.x) == 0);
        (void)std::snprintf(what, sizeof(what), "secp256k1: pub(d=%s) y", v.d);
        check(what, ok && std::strcmp(y, v.y) == 0);
    }

    const Vector r1_vectors[] = {
        {"1",
         "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
         "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"},
        {"2",
         "7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978",
         "07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1"},
        {"3",
         "5ecbe4d1a6330a44c8f7ef951d4bf165e6c6b721efada985fb41661bc6e7fd6c",
         "8734640c4998ff7e374b06ce1a64a2ecd82ab036384fb83d9a79b127a27d5032"},
        {"1234567890abcdef",
         "9fad84aeae08bbef7f010014d82cef6a09de2b0cf871b5ce0c4f1d13a59a5934",
         "07cb45769f1070e2c2470fe5b1bfe63133c0b0cdc64ea4bf3791a8ec2a07fd4f"}
    };
    for (const Vector& v : r1_vectors) {
        R1::Point pub;
        const bool ok = R1::compute_public_key(bigint_from_hex(v.d), pub);
        char x[65], y[65];
        bigint_to_hex(pub.x, x);
        bigint_to_hex(pub.y, y);
        char what[96];
        (void)std::snprintf(what, sizeof(what), "secp256r1: pub(d=%s) x", v.d);
        check(what, ok && std::strcmp(x, v.x) == 0);
        (void)std::snprintf(what, sizeof(what), "secp256r1: pub(d=%s) y", v.d);
        check(what, ok && std::strcmp(y, v.y) == 0);
    }

    // (n-1)·G == -G: проверка порядка группы и корректности отрицания.
    {
        const char* n_minus_1_k1 =
            "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364140";
        K1::Point pub;
        check("secp256k1: (n-1)·G вычисляется",
              K1::compute_public_key(bigint_from_hex(n_minus_1_k1), pub));
        char x[65];
        bigint_to_hex(pub.x, x);
        check("secp256k1: (n-1)·G имеет ту же x, что G",
              std::strcmp(x, "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798") == 0);
        check("secp256k1: (n-1)·G имеет -y", pub.y != K1::generator().y);
    }

    // Неверные приватные ключи.
    {
        K1::Point pub;
        check("ECC: d = 0 отклоняется", !K1::compute_public_key(BigInt<8>(0u), pub));
        const char* order_k1 =
            "fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141";
        check("ECC: d = n отклоняется", !K1::compute_public_key(bigint_from_hex(order_k1), pub));
    }

    // ECDH: обе стороны получают один и тот же секрет.
    {
        uint8_t seed_a[32], seed_b[32];
        uint32_t wa[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
        uint32_t wb[8] = {0xDEADBEEFu, 0xCAFEBABEu, 0x12345678u, 0x9ABCDEF0u,
                          0x0BADC0DEu, 0xFACEFEEDu, 0x5A5A5A5Au, 0xA5A5A5A5u};
        Rng::seed_from_words(seed_a, wa);
        Rng::seed_from_words(seed_b, wb);

        Rng rng_a, rng_b;
        rng_a.seed(seed_a);
        rng_b.seed(seed_b);

        K1::Scalar da, db;
        K1::Point qa, qb;
        check("ECDH: ключевая пара A генерируется", K1::generate_key_pair(rng_a, da, qa));
        check("ECDH: ключевая пара B генерируется", K1::generate_key_pair(rng_b, db, qb));
        check("ECDH: публичный ключ A на кривой", K1::is_on_curve(qa));
        check("ECDH: публичный ключ B на кривой", K1::is_on_curve(qb));
        check("ECDH: приватные ключи в [1, n-1]",
              K1::is_valid_private_key(da) && K1::is_valid_private_key(db));

        uint8_t sa[32], sb[32];
        check("ECDH: A считает секрет", K1::derive_shared_secret_bytes(da, qb, sa));
        check("ECDH: B считает секрет", K1::derive_shared_secret_bytes(db, qa, sb));
        check("ECDH: секреты совпадают", std::memcmp(sa, sb, 32) == 0);

        // Секрет с чужим ключом не совпадает (проверка, что это не константа).
        uint8_t other[32];
        K1::derive_shared_secret_bytes(da, qa, other);
        check("ECDH: секрет зависит от ключей", std::memcmp(sa, other, 32) != 0);

        // Отвергается точка вне кривой (атака подстановкой точки).
        K1::Point bad = qb;
        bad.y = bad.y + BigInt<8>(1u);
        check("ECDH: точка вне кривой отвергается",
              !K1::derive_shared_secret_bytes(da, bad, sa));

        // P-256: то же самое.
        R1::Scalar da2, db2;
        R1::Point qa2, qb2;
        check("ECDH (P-256): ключевые пары",
              R1::generate_key_pair(rng_a, da2, qa2) && R1::generate_key_pair(rng_b, db2, qb2));
        uint8_t sa2[32], sb2[32];
        check("ECDH (P-256): секреты совпадают",
              R1::derive_shared_secret_bytes(da2, qb2, sa2) &&
              R1::derive_shared_secret_bytes(db2, qa2, sb2) &&
              std::memcmp(sa2, sb2, 32) == 0);
    }
}

// ──────────────────────────────── ECIES ────────────────────────────────
void test_ecies() {
    using namespace embed;
    using namespace embed::crypto;
    (void)std::puts("ecies:");

    using K1 = Ecdh<EccCurve::Secp256k1>;
    using E1 = Ecies<EccCurve::Secp256k1>;

    uint8_t seed[32];
    uint32_t words[8] = {0x0F0E0D0Cu, 0x0B0A0908u, 0x07060504u, 0x03020100u,
                         0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u};
    Rng::seed_from_words(seed, words);

    Rng rng;
    rng.seed(seed);

    K1::Scalar recipient_priv;
    K1::Point recipient_pub;
    check("ECIES: ключевая пара получателя",
          K1::generate_key_pair(rng, recipient_priv, recipient_pub));

    E1 ecies(recipient_priv);

    const char* text = "MCFrame: гибридное шифрование на secp256k1";
    const Size text_len = std::strlen(text);

    uint8_t cipher[256];
    Size cipher_len = 0u;
    check("ECIES: encrypt",
          E1::encrypt(rng, recipient_pub,
                      reinterpret_cast<const uint8_t*>(text), text_len,
                      cipher, sizeof(cipher), &cipher_len) == Status::Ok);
    check_eq<uint32_t>("ECIES: длина сообщения = 96 + len",
                       static_cast<uint32_t>(cipher_len),
                       static_cast<uint32_t>(E1::OverheadBytes + text_len));

    uint8_t plain[256];
    Size plain_len = 0u;
    check("ECIES: decrypt",
          ecies.decrypt(cipher, cipher_len, plain, sizeof(plain), &plain_len) == Status::Ok);
    check("ECIES: round-trip",
          plain_len == text_len && std::memcmp(plain, text, text_len) == 0);

    // Шифротекст не содержит открытый текст (это не «XOR с повтором ключа»).
    bool leaked = false;
    for (Size i = 0; i + text_len <= cipher_len; ++i) {
        if (std::memcmp(cipher + i, text, text_len) == 0) leaked = true;
    }
    check("ECIES: открытый текст не виден в шифротексте", !leaked);

    // Подмена шифротекста и тега обнаруживается.
    uint8_t tampered[256];
    std::memcpy(tampered, cipher, cipher_len);
    tampered[E1::PublicKeyBytes + 3u] ^= 0x01u;
    check("ECIES: подмена шифротекста ⇒ Corruption",
          ecies.decrypt(tampered, cipher_len, plain, sizeof(plain), &plain_len) == Status::Corruption);

    std::memcpy(tampered, cipher, cipher_len);
    tampered[cipher_len - 1u] ^= 0x80u;
    check("ECIES: подмена тега ⇒ Corruption",
          ecies.decrypt(tampered, cipher_len, plain, sizeof(plain), &plain_len) == Status::Corruption);

    // Чужой приватный ключ — тоже Corruption (тег не сходится).
    K1::Scalar stranger_priv;
    K1::Point stranger_pub;
    K1::generate_key_pair(rng, stranger_priv, stranger_pub);
    E1 stranger(stranger_priv);
    check("ECIES: чужой закрытый ключ ⇒ Corruption",
          stranger.decrypt(cipher, cipher_len, plain, sizeof(plain), &plain_len) == Status::Corruption);

    // Мусорный эфемерный ключ — отказ до вычислений.
    std::memcpy(tampered, cipher, cipher_len);
    tampered[0] ^= 0xFFu;
    check("ECIES: испорченный эфемерный ключ ⇒ InvalidArg",
          ecies.decrypt(tampered, cipher_len, plain, sizeof(plain), &plain_len) == Status::InvalidArg);

    // Мало места в буфере.
    check("ECIES: малый буфер ⇒ NoMemory",
          E1::encrypt(rng, recipient_pub, reinterpret_cast<const uint8_t*>(text), text_len,
                      cipher, 8u, &cipher_len) == Status::NoMemory);

    // Пустое сообщение (только overhead).
    Size empty_len = 0u;
    check("ECIES: пустое сообщение шифруется",
          E1::encrypt(rng, recipient_pub, plain, 0u, cipher, sizeof(cipher), &empty_len) == Status::Ok);
    check_eq<uint32_t>("ECIES: длина пустого сообщения = overhead",
                       static_cast<uint32_t>(empty_len),
                       static_cast<uint32_t>(E1::OverheadBytes));
}

// ────────────────────────────── CSPRNG ──────────────────────────────
void test_rng() {
    using namespace embed::crypto;
    (void)std::puts("csprng:");

    // ChaCha20 (RFC 8439): ключ 00..1f, nonce 000000090000004a00000000,
    // первый блок — счётчик 0 (проверено независимой реализацией).
    uint8_t key[32];
    uint8_t nonce[12];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
    parse_hex("000000090000004a00000000", nonce, sizeof(nonce));

    uint8_t stream[64];
    Rng rng;
    check("CSPRNG: до seed() генератор не готов", !rng.ready());

    rng.seed(key, nonce);
    check("CSPRNG: после seed() генератор готов", rng.ready());
    rng.fill(stream, sizeof(stream));
    check_hex("CSPRNG: поток ChaCha20 (блок 0)", stream, 64,
              "8adc91fd9ff4f0f51b0fad50ff15d637e40efda206cc52c783a74200503c1582"
              "cd9833367d0a54d57d3c9e998f490ee69ca34c1ff9e939a75584c52d690a35d4");

    // Тот же seed — тот же поток; второй блок отличается от первого.
    Rng rng2;
    rng2.seed(key, nonce);
    uint8_t stream2[128];
    rng2.fill(stream2, sizeof(stream2));
    check("CSPRNG: детерминированность при том же seed",
          std::memcmp(stream2, stream, 64) == 0);
    check("CSPRNG: следующий блок отличается",
          std::memcmp(stream2, stream2 + 64, 64) != 0);

    // Незасеянный генератор отдаёт нули и не «притворяется» случайным.
    Rng empty;
    uint8_t zeros[16];
    std::memset(zeros, 0xFF, sizeof(zeros));
    empty.fill(zeros, sizeof(zeros));
    bool all_zero = true;
    for (uint8_t b : zeros) {
        if (b != 0u) all_zero = false;
    }
    check("CSPRNG: без seed() выдаются нули", all_zero);

    // next_below не выходит за границу и не зацикливается.
    bool in_range = true;
    for (int i = 0; i < 64; ++i) {
        uint32_t v = 0u;
        if (!rng.next_below(10u, &v) || v >= 10u) in_range = false;
    }
    check("CSPRNG: next_below(10) в диапазоне", in_range);

    uint32_t r = 0u;
    check("CSPRNG: next_below(0) отклоняется", !rng.next_below(0u, &r));

    // seed_from_words: 8 слов → 32 байта little-endian.
    uint8_t seed_out[32];
    uint32_t w[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    Rng::seed_from_words(seed_out, w);
    check_hex("CSPRNG: seed_from_words", seed_out, 32,
              "0100000002000000030000000400000005000000060000000700000008000000");
}

// ────────────────────────────── утилиты ──────────────────────────────
void test_utils() {
    using namespace embed;
    using namespace embed::crypto;
    (void)std::puts("crypto_utils:");

    uint8_t a[4] = {1, 2, 3, 4};
    uint8_t b[4] = {1, 2, 3, 4};
    uint8_t c[4] = {1, 2, 3, 5};
    check("ct_equal: одинаковые буферы", ct_equal(a, b, 4u));
    check("ct_equal: разные буферы", !ct_equal(a, c, 4u));

    // Порядок байт.
    uint8_t be[4];
    store_be32(0x01020304u, be);
    check_hex("store_be32", be, 4, "01020304");
    check_eq<uint32_t>("load_be32", load_be32(be), 0x01020304u);

    uint8_t le[8];
    store_le64(0x0102030405060708ull, le);
    check_hex("store_le64", le, 8, "0807060504030201");
    check("load_le64", load_le64(le) == 0x0102030405060708ull);

    // PKCS#7.
    check_eq<uint32_t>("pkcs7_padding_len(0,16) == 16",
                       static_cast<uint32_t>(pkcs7_padding_len(0u, 16u)), 16u);
    check_eq<uint32_t>("pkcs7_padding_len(15,16) == 1",
                       static_cast<uint32_t>(pkcs7_padding_len(15u, 16u)), 1u);
    check_eq<uint32_t>("pkcs7_padding_len(16,16) == 16",
                       static_cast<uint32_t>(pkcs7_padding_len(16u, 16u)), 16u);

    uint8_t buf[32];
    std::memset(buf, 0, sizeof(buf));
    const Size padded = pkcs7_pad(buf, 20u, sizeof(buf), 16u);
    Size plain = 0u;
    check("pkcs7: pad/unpad обратимы",
          padded == 32u && pkcs7_unpad(buf, padded, 16u, &plain) && plain == 20u);

    buf[31] = 0u;   // padding-байт не может быть нулём
    check("pkcs7: нулевой padding отвергается",
          !pkcs7_unpad(buf, padded, 16u, &plain));

    buf[31] = 200u; // значение больше размера блока
    check("pkcs7: слишком большой padding отвергается",
          !pkcs7_unpad(buf, padded, 16u, &plain));

    check("pkcs7: длина не кратна блоку отвергается",
          !pkcs7_unpad(buf, 31u, 16u, &plain));
}

} // namespace

int main() {
    (void)std::puts("=== MCFrame crypto self-tests (mock) ===");
    test_utils();
    test_crc();
    test_sha2();
    test_sha3();
    test_hmac();
    test_aes();
    test_tdes();
    test_rsa();
    test_rng();
    test_ecc();
    test_ecies();

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed != 0) {
        (void)std::printf(", провалено: %d\n", g_failed);
        return 1;
    }
    (void)std::printf(" — все тесты пройдены\n");
    return 0;
}
