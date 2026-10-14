// crypto/aes.cpp
// ---------------------------------------------------------------------------
// Явные инстанцирования ходовых конфигураций AES. Заодно проверяется, что
// собираются и «только шифрование» (без таблицы обратных подстановок),
// и режимы CBC/CTR с padding'ом и без.
// ---------------------------------------------------------------------------
#include "aes.hpp"

namespace embed {
namespace crypto {

// Только шифрование (обратные таблицы в прошивку не попадают).
template class Aes<AesKeySize::Aes128, AesMode::Ecb, false, false>;
template class Aes<AesKeySize::Aes128, AesMode::Cbc, false, false>;
template class Aes<AesKeySize::Aes128, AesMode::Ctr, false, false>;
template class Aes<AesKeySize::Aes256, AesMode::Ctr, false, false>;

// Шифрование и расшифрование.
template class Aes<AesKeySize::Aes128, AesMode::Ecb, true, false>;
template class Aes<AesKeySize::Aes128, AesMode::Cbc, true, false>;
template class Aes<AesKeySize::Aes128, AesMode::Cbc, true, true>;
template class Aes<AesKeySize::Aes192, AesMode::Ecb, true, false>;
template class Aes<AesKeySize::Aes192, AesMode::Cbc, true, true>;
template class Aes<AesKeySize::Aes256, AesMode::Ecb, true, false>;
template class Aes<AesKeySize::Aes256, AesMode::Cbc, true, false>;
template class Aes<AesKeySize::Aes256, AesMode::Cbc, true, true>;
template class Aes<AesKeySize::Aes256, AesMode::Ctr, true, false>;

} // namespace crypto
} // namespace embed
