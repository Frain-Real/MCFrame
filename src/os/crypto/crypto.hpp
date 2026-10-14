// crypto/crypto.hpp
// ---------------------------------------------------------------------------
// Единая точка входа модуля криптографии MCFrame.
//
//   #include "os/crypto/crypto.hpp"
//
// Состав модуля:
//   crypto_utils.hpp  байтовый порядок, сравнение за постоянное время, PKCS#7
//   random.hpp        CSPRNG на ChaCha20 (RFC 8439) — обязателен для ключей
//   crc.hpp           CRC-8/16/32: обычные и «отражённые» (zlib, MODBUS)
//   sha2.hpp          SHA-224/256/384/512 и SHA-512/224, SHA-512/256
//   sha3.hpp          SHA3-224/256/384/512 и SHAKE128/256
//   hmac.hpp          HMAC-SHA256 (целостность, KDF)
//   aes.hpp           AES-128/192/256: ECB, CBC, CTR (+PKCS#7)
//   tdes.hpp          DES и Triple-DES (EDE): ECB, CBC, CTR
//   rsa.hpp           RSA на 16/32-битных целых (только учебные задачи!)
//   ecc.hpp           ECDH: secp256k1, secp256r1 (P-256)
//   ecies.hpp         гибридное шифрование ECDH + SHA-256-KDF + HMAC
//
// Общие правила модуля (единые для всех алгоритмов):
//   1. Аргументы и результат проверяются: ёмкость выходного буфера передаётся
//      явно, ошибки возвращаются как embed::Status, а не «магическим» 0.
//   2. Секреты затираются (быстрые функции wipe/clear), ключевой материал не
//      остаётся на стеке после выхода из функции.
//   3. Порядок байт на проводе фиксирован (big-endian там, где так требует
//      стандарт) — результат не зависит от архитектуры.
//   4. Никаких исключений, никакого динамического выделения памяти, никаких
//      виртуальных функций: только статические таблицы и стек.
//   5. Ничего не «само» не безопасно: для ключей нужен CSPRNG, засеянный
//      энтропией платформы (см. random.hpp).
//
// Пример:
// @code
//   #include "os/crypto/crypto.hpp"
//   using namespace embed::crypto;
//
//   uint8_t digest[Sha256::DigestSize];
//   Sha256::hash("abc", 3, digest);
//
//   uint8_t key[16] = { ... };
//   Aes128CbcPkcs7 aes(key, iv);
//   uint8_t out[64]; Size out_len = 0;
//   if (aes.encrypt(plain, plain_len, out, sizeof(out), &out_len) != Status::Ok) { ... }
// @endcode
// ---------------------------------------------------------------------------
#pragma once

#include "crypto_utils.hpp"
#include "random.hpp"
#include "crc.hpp"
#include "sha2.hpp"
#include "sha3.hpp"
#include "hmac.hpp"
#include "aes.hpp"
#include "tdes.hpp"
#include "rsa.hpp"
#include "ecc.hpp"
#include "ecies.hpp"
