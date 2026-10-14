// crypto/rsa.cpp — явные инстанцирования RSA на 16- и 32-битных целых.
#include "rsa.hpp"

namespace embed {
namespace crypto {

template class Rsa<uint16_t>;
template class Rsa<uint32_t>;

} // namespace crypto
} // namespace embed
