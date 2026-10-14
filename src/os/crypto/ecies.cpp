// crypto/ecies.cpp — явные инстанцирования гибридного шифрования.
#include "ecies.hpp"

namespace embed {
namespace crypto {

template class Ecies<EccCurve::Secp256k1>;
template class Ecies<EccCurve::Secp256r1>;

} // namespace crypto
} // namespace embed
