// crypto/sha2.cpp
// ---------------------------------------------------------------------------
// Явные инстанцирования всех вариантов SHA-2: таблицы констант и логика
// сжатия компилируются в составе библиотеки, а не «когда-нибудь потом»
// в приложении. Ошибка в константах или разрядности ловится сборкой.
// ---------------------------------------------------------------------------
#include "sha2.hpp"

namespace embed {
namespace crypto {

template class Sha2<Sha2Variant::Sha224>;
template class Sha2<Sha2Variant::Sha256>;
template class Sha2<Sha2Variant::Sha384>;
template class Sha2<Sha2Variant::Sha512>;
template class Sha2<Sha2Variant::Sha512_224>;
template class Sha2<Sha2Variant::Sha512_256>;

} // namespace crypto
} // namespace embed
