// crypto/sha3.cpp — явные инстанцирования SHA-3 и SHAKE.
#include "sha3.hpp"

namespace embed {
namespace crypto {

template class Sha3<224u>;
template class Sha3<256u>;
template class Sha3<384u>;
template class Sha3<512u>;

template class Shake<128u>;
template class Shake<256u>;

} // namespace crypto
} // namespace embed
