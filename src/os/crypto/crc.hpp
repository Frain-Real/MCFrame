#pragma once
#include "../common.hpp"

namespace embed {

/**
 * @brief Легковесный, полностью compile-time CRC-калькулятор.
 * 
 * @tparam Size       Тип данных (uint8_t, uint16_t, uint32_t)
 * @tparam Poly       Полином (например, 0x04C11DB7 для STM32 CRC32)
 * @tparam InitXor    Начальное значение XOR (по умолчанию инверсия нуля)
 * @tparam FinalXor   Финальное значение XOR (по умолчанию инверсия нуля)
 */
template <typename Size, uint32_t Poly, uint32_t InitXor = ~Size(0), uint32_t FinalXor = ~Size(0)>
struct Crc {
private:
    static constexpr uint32_t msb_mask = (sizeof(Size) == 1) ? 0x80U :
                                         (sizeof(Size) == 2) ? 0x8000U : 0x80000000U;
    static constexpr uint32_t calc_step(uint32_t t) {
        return (t & msb_mask) ? ((t << 1) ^ Poly) : (t << 1);
    }
    static constexpr uint32_t calc_hash(uint32_t t) {
        uint32_t res = t << (8 * sizeof(Size) - 8);
        for (int i = 0; i < 8; ++i) {
            res = calc_step(res);
        }
        return res;
    }

    struct TableData {
        Size data[256];
        constexpr TableData() : data{} {
            for (int i = 0; i < 256; ++i) {
                data[i] = static_cast<Size>(calc_hash(i));
            }
        }
        constexpr Size operator[](int i) const { 
            return data[i]; 
        }
    };

public:
    static constexpr TableData table{};
    static Status calculate(Size* result, uint32_t len, const void* data) {
        if (!result) {
            return Status::InvalidArg;
        }
        if (!data && len > 0) {
            return Status::InvalidArg;
        }

        const uint8_t* buffer = static_cast<const uint8_t*>(data);
        uint32_t crc = InitXor;

        while (len--) {
            if constexpr (sizeof(Size) == 1) {
                crc = table[(crc ^ *buffer++) & 0xFF];
            } 
            else if constexpr (sizeof(Size) == 2) {
                crc = (crc << 8) ^ table[((crc >> 8) ^ *buffer++) & 0xFF];
            } 
            else if constexpr (sizeof(Size) == 4) {
                crc = (crc << 8) ^ table[((crc >> 24) ^ *buffer++) & 0xFF];
            }
        }
        *result = static_cast<Size>(crc ^ FinalXor);
        return Status::Ok;
    }
};
} 