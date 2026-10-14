// crypto/crc.cpp
// ---------------------------------------------------------------------------
// Модуль CRC целиком шаблонный, поэтому здесь — явные инстанцирования
// готовых пресетов. Смысл: параметры и таблицы каждого пресета проверяются
// компилятором в составе сборки, а не только в тестах, и не «растворяются»
// в единицах трансляции приложения.
// ---------------------------------------------------------------------------
#include "crc.hpp"

namespace embed {
namespace crypto {

template struct Crc<uint8_t,  0x07u,       0x00u,       0x00u,       false>; // CRC-8/SMBUS
template struct Crc<uint8_t,  0x07u,       0xFFu,       0x00u,       true>;  // CRC-8/ROHC
template struct Crc<uint16_t, 0x1021u,     0xFFFFu,     0x0000u,     false>; // CRC-16/CCITT-FALSE
template struct Crc<uint16_t, 0x8005u,     0xFFFFu,     0x0000u,     true>;  // CRC-16/MODBUS
template struct Crc<uint16_t, 0x8005u,     0x0000u,     0x0000u,     true>;  // CRC-16/ARC
template struct Crc<uint32_t, 0x04C11DB7u, 0xFFFFFFFFu, 0x00000000u, false>; // CRC-32/MPEG-2 (аппаратный CRC STM32)
template struct Crc<uint32_t, 0x04C11DB7u, 0xFFFFFFFFu, 0xFFFFFFFFu, true>;  // CRC-32/ISO-HDLC (zlib)

} // namespace crypto
} // namespace embed
