// crypto/crc.hpp
// ---------------------------------------------------------------------------
// CRC: табличный, compile-time, с поддержкой «отражённых» вариантов.
//
// ПРЕЖНЯЯ ВЕРСИЯ (исправлено здесь):
//   1. Значения по умолчанию InitXor/FinalXor = ~Word(0) для 8- и 16-битных
//      CRC давали 0xFFFFFFFF вместо маски ширины. Алгоритм «вытягивал»
//      результат только за счёт последующего усечения, то есть работал
//      случайно; теперь значения явно маскируются по ширине.
//   2. Не было ни одного «отражённого» (refin/refout) варианта, а именно так
//      заданы CRC-32 (zlib/PNG/Ethernet) и CRC-16/MODBUS — самые ходовые.
//   3. Пресетов не было вообще, каждый пользователь собирал параметры сам.
//   4. Длина была uint32_t, что на 64-битном хосте молча обрезает буферы
//      больше 4 ГиБ; тип — embed::Size.
// ---------------------------------------------------------------------------
#pragma once

#include "crypto_utils.hpp"

#include <cstddef>
#include <cstdint>

namespace embed {
namespace crypto {

/**
 * @brief CRC с таблицей, посчитанной на этапе компиляции.
 *
 * @tparam Word       Тип результата: uint8_t, uint16_t или uint32_t.
 * @tparam Poly       Полином в НЕотражённой записи (как в каталоге CRC):
 *                    для CRC-32 это 0x04C11DB7, а не 0xEDB88320.
 * @tparam Init       Начальное значение (init).
 * @tparam XorOut     Финальное XOR (xorout).
 * @tparam Reflected  Обрабатывать байты младшим битом вперёд (refin = refout
 *                    = true). Полином при этом разворачивается внутри.
 *
 * Готовые параметры — в пресетах ниже (Crc8, Crc16Modbus, Crc32Zlib, ...).
 */
template <typename Word, uint32_t Poly, uint32_t Init, uint32_t XorOut,
          bool Reflected = false>
struct Crc {
    static_assert(sizeof(Word) == 1 || sizeof(Word) == 2 || sizeof(Word) == 4,
                  "Crc: ширина должна быть 8, 16 или 32 бита");

    static constexpr uint32_t width_bits = static_cast<uint32_t>(8u * sizeof(Word));

    /// Маска ширины: 0xFF, 0xFFFF или 0xFFFFFFFF.
    static constexpr uint32_t mask() noexcept {
        return (width_bits >= 32u) ? 0xFFFFFFFFu : ((1u << width_bits) - 1u);
    }

    static constexpr uint32_t init_value_raw() noexcept { return Init & mask(); }
    static constexpr uint32_t xor_out_raw()  noexcept { return XorOut & mask(); }

    /// Разворот младших `bits` бит значения.
    static constexpr uint32_t reverse_bits(uint32_t v, uint32_t bits) noexcept {
        uint32_t r = 0u;
        for (uint32_t i = 0; i < bits; ++i) {
            r = (r << 1) | ((v >> i) & 1u);
        }
        return r;
    }

    /// Полином в той записи, в которой его использует выбранный вариант.
    static constexpr uint32_t effective_poly() noexcept {
        return Reflected ? reverse_bits(Poly & mask(), width_bits) : (Poly & mask());
    }

private:
    static constexpr uint32_t msb_mask() noexcept {
        return (width_bits >= 32u) ? 0x80000000u : (1u << (width_bits - 1u));
    }

    /// Один шаг сдвига по байту (8 тактов) для значения таблицы.
    ///
    /// Для НЕотражённого варианта индекс сдвигается в СТАРШИЕ биты ширины:
    /// таблица строится для байта, выровненного по старшему разряду
    /// (для CRC-8 сдвиг нулевой, поэтому ошибка проявлялась только на
    /// 16- и 32-битных CRC).
    static constexpr uint32_t table_entry(uint32_t index) noexcept {
        uint32_t t = Reflected ? index : (index << (width_bits - 8u));
        for (int i = 0; i < 8; ++i) {
            if constexpr (Reflected) {
                t = (t & 1u) ? ((t >> 1) ^ effective_poly()) : (t >> 1);
            } else {
                t = (t & msb_mask()) ? ((t << 1) ^ effective_poly()) : (t << 1);
            }
            t &= mask();
        }
        return t;
    }

    struct Table {
        Word data[256];
        constexpr Table() noexcept : data{} {
            for (uint32_t i = 0; i < 256u; ++i) {
                data[i] = static_cast<Word>(table_entry(i));
            }
        }
        constexpr Word operator[](uint32_t i) const noexcept { return data[i]; }
    };

public:
    static constexpr Table table{};

    /// Начальное «сырое» состояние (без финального XOR).
    static constexpr Word init() noexcept {
        return static_cast<Word>(init_value_raw());
    }

    /// Финальное преобразование «сырого» состояния в результат.
    static constexpr Word finish(Word crc) noexcept {
        return static_cast<Word>(static_cast<uint32_t>(crc) ^ xor_out_raw());
    }

    /**
     * @brief Продолжить вычисление: crc — «сырое» состояние (см. init()).
     *
     * Такой вид позволяет считать CRC потоково, кусками, без буферизации
     * всего сообщения.
     */
    static Word update(Word crc, const void* data, Size len) noexcept {
        uint32_t c = static_cast<uint32_t>(crc) & mask();
        if (data == nullptr) {
            return static_cast<Word>(c);
        }
        const uint8_t* bytes = static_cast<const uint8_t*>(data);

        if constexpr (Reflected) {
            for (Size i = 0; i < len; ++i) {
                c = (c >> 8) ^ static_cast<uint32_t>(table[(c ^ bytes[i]) & 0xFFu]);
            }
        } else {
            const uint32_t shift = width_bits - 8u;
            for (Size i = 0; i < len; ++i) {
                c = ((c << 8) ^ static_cast<uint32_t>(
                         table[((c >> shift) ^ bytes[i]) & 0xFFu])) & mask();
            }
        }
        return static_cast<Word>(c);
    }

    /**
     * @brief CRC от буфера.
     * @return Status::InvalidArg, если result == nullptr или data == nullptr
     *         при len > 0.
     */
    static Status calculate(Word* result, Size len, const void* data) noexcept {
        if (result == nullptr) {
            return Status::InvalidArg;
        }
        if (data == nullptr && len > 0u) {
            return Status::InvalidArg;
        }
        *result = finish(update(init(), data, len));
        return Status::Ok;
    }

    /// CRC от буфера без разбора статуса (данные всегда валидны).
    static Word compute(const void* data, Size len) noexcept {
        return finish(update(init(), data, len));
    }
};

// ---------------------------------------------------------------------------
// Пресеты. Значения Init/XorOut — из каталога CRC (reveng).
// ---------------------------------------------------------------------------

/// CRC-8/SMBUS: poly 0x07, init 0x00, xorout 0x00. «123456789» = 0xF4.
using Crc8SMBus = Crc<uint8_t, 0x07u, 0x00u, 0x00u>;

/// CRC-8/ROHC: отражённый, poly 0x07, init 0xFF. «123456789» = 0xD0.
using Crc8Rohc = Crc<uint8_t, 0x07u, 0xFFu, 0x00u, true>;

/// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF. «123456789» = 0x29B1.
using Crc16CcittFalse = Crc<uint16_t, 0x1021u, 0xFFFFu, 0x0000u>;

/// CRC-16/MODBUS: отражённый, poly 0x8005. «123456789» = 0x4B37.
using Crc16Modbus = Crc<uint16_t, 0x8005u, 0xFFFFu, 0x0000u, true>;

/// CRC-16/ARC: отражённый, poly 0x8005, init 0. «123456789» = 0xBB3D.
using Crc16Arc = Crc<uint16_t, 0x8005u, 0x0000u, 0x0000u, true>;

/**
 * CRC-32/MPEG-2 — ровно то, что считает аппаратный блок CRC STM32
 * (полином 0x04C11DB7, init 0xFFFFFFFF, без финального XOR, без отражения).
 * «123456789» = 0x0376E6E7.
 */
using Crc32Mpeg2 = Crc<uint32_t, 0x04C11DB7u, 0xFFFFFFFFu, 0x00000000u>;

/// Псевдоним: аппаратный CRC32 у STM32F1 реализует именно CRC-32/MPEG-2.
using Crc32Stm32 = Crc32Mpeg2;

/**
 * CRC-32/ISO-HDLC (zlib, PNG, gzip, Ethernet): отражённый, poly 0x04C11DB7,
 * init 0xFFFFFFFF, xorout 0xFFFFFFFF. «123456789» = 0xCBF43926.
 */
using Crc32Zlib = Crc<uint32_t, 0x04C11DB7u, 0xFFFFFFFFu, 0xFFFFFFFFu, true>;

} // namespace crypto
} // namespace embed
