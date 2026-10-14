// crypto/ecc.cpp — явные инстанцирования ECDH для поддерживаемых кривых.
// Здесь же компилируется вся арифметика поля (Монтгомери) и формулы точек.
#include "ecc.hpp"

namespace embed {
namespace crypto {

template class Ecdh<EccCurve::Secp256k1>;
template class Ecdh<EccCurve::Secp256r1>;

} // namespace crypto
} // namespace embed
