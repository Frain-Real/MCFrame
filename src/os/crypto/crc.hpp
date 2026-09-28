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
    // Определяем маску старшего бита в зависимости от разрядности
    static constexpr uint32_t msb_mask = (sizeof(Size) == 1) ? 0x80U :
                                         (sizeof(Size) == 2) ? 0x8000U : 0x80000000U;

    // Один шаг деления полинома
    static constexpr uint32_t calc_step(uint32_t t) {
        return (t & msb_mask) ? ((t << 1) ^ Poly) : (t << 1);
    }

    // Вычисление хэша для одного байта (8 итераций сдвига)
    static constexpr uint32_t calc_hash(uint32_t t) {
        // Сдвигаем байт в старшую позицию, соответствующую разрядности Size
        uint32_t res = t << (8 * sizeof(Size) - 8);
        for (int i = 0; i < 8; ++i) {
            res = calc_step(res);
        }
        return res;
    }

    // Структура для хранения таблицы, инициализируемая в compile-time
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
    // Таблица вычисляется компилятором один раз и размещается в .rodata
    static constexpr TableData table{};

    /**
     * @brief Вычисление CRC для блока данных
     * @param result Указатель для записи результата
     * @param len    Длина данных в байтах
     * @param data   Указатель на данные
     * @return Status::Ok или код ошибки
     */
    static Status calculate(Size* result, uint32_t len, const void* data) {
        // Базовая защита от некорректных указателей (явные проверки, без исключений)
        if (!result) {
            return Status::InvalidArg; // Замените на актуальный код ошибки из common.hpp
        }
        if (!data && len > 0) {
            return Status::InvalidArg;
        }

        const uint8_t* buffer = static_cast<const uint8_t*>(data);
        uint32_t crc = InitXor;

        while (len--) {
            if constexpr (sizeof(Size) == 1) {
                // CRC-8: индекс = младший байт (crc ^ байт данных)
                crc = table[(crc ^ *buffer++) & 0xFF];
            } 
            else if constexpr (sizeof(Size) == 2) {
                // CRC-16: индекс = старший байт crc ^ байт данных
                crc = (crc << 8) ^ table[((crc >> 8) ^ *buffer++) & 0xFF];
            } 
            else if constexpr (sizeof(Size) == 4) {
                // CRC-32: индекс = старший байт crc ^ байт данных
                crc = (crc << 8) ^ table[((crc >> 24) ^ *buffer++) & 0xFF];
            }
        }

        *result = static_cast<Size>(crc ^ FinalXor);
        return Status::Ok;
    }
};
} 