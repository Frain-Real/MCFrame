// boards/pin_map.hpp
// ---------------------------------------------------------------------------
// ДЕКЛАРАТИВНАЯ КАРТА ПИНОВ ПЛАТЫ.
//
// Зачем это отдельно от драйверов: плата — это не «набор синглтонов
// board::uart1()», а описание того, какие функции на каких физических пинах
// висят. Пока это описание лежит рядом с драйверами, ошибка «USART1_TX и
// PWM на одном пине» обнаруживается только на железе (или, что хуже, не
// обнаруживается вовсе). Здесь карта проверяется на этапе компиляции:
//
//     inline constexpr PinMap<7> kPins = PinMap<7>{{
//         {PA9,  Signal::UartTx, 1, 7},   // USART1_TX, AF7
//         {PA10, Signal::UartRx, 1, 7},
//         {PA0,  Signal::Pwm,    2, 1, 1} // TIM2_CH1
//     }};
//     static_assert(kPins.valid(), "конфликт пинов на плате");
//
// Модель сознательно простая и статическая:
//   * пины не «арендуются» в рантайме и не переключаются на лету;
//   * одна и та же физическая нога не может быть объявлена дважды;
//   * один функциональный слот (USART1_TX, SPI1_SCK, TIM2_CH1, ADC1_IN5)
//     не может быть объявлен на двух ногах.
//
// Этого достаточно, чтобы карта была единственным источником истины для BSP,
// а не комментарием, который расходится с кодом.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

namespace board {

/// Порт GPIO. None — «пин не задан» (ловим забытую инициализацию).
enum class Port : uint8_t {
    None = 0,
    A, B, C, D, E, F, G, H
};

/// Физический пин: порт + номер 0..15.
struct Pin {
    Port    port   = Port::None;
    uint8_t number = 0u;

    constexpr bool valid() const noexcept {
        return port != Port::None && number < 16u;
    }

    constexpr bool operator==(const Pin& other) const noexcept {
        return port == other.port && number == other.number;
    }
    constexpr bool operator!=(const Pin& other) const noexcept {
        return !(*this == other);
    }
};

/// Сигнал, который может занимать пин.
///
/// Значения — не «функция периферии» конкретного SoC, а роль в плате:
/// AF-номер и номер инстанса задаются отдельно, поэтому одна карта читается
/// и для F1 (AF нет), и для F4/G4 (AF есть).
enum class Signal : uint8_t {
    Gpio = 0,
    Led,
    Button,
    UartTx,
    UartRx,
    UartCts,
    UartRts,
    SpiSck,
    SpiMiso,
    SpiMosi,
    SpiCs,
    I2cScl,
    I2cSda,
    Adc,
    Pwm,
    Exti,
    UsbDm,
    UsbDp,
    CanTx,
    CanRx,
};

/// Одна строка карты: пин + роль + инстанс/канал + AF.
///
/// instance  — номер периферии: USART1 → 1, SPI2 → 2, I2C1 → 1, TIM2 → 2,
///             ADC1 → 1. Для GPIO/LED/Button это логический номер (не
///             проверяется на уникальность).
/// alternate — AF-номер для F4/G4; 0 означает «не задан» (для F1 и для
///             сигналов, которым AF не нужен).
/// channel   — канал внутри периферии: PWM-канал 1..4, ADC-канал, номер CS
///             у SPI (0 — первый). Для UART/I2C/USB — 0.
///
/// Порядок полей выбран так, чтобы строка карты читалась как
/// «пин, роль, инстанс, AF, канал» — именно в этом порядке её и заполняют.
struct PinUse {
    Pin      pin{};
    Signal   signal = Signal::Gpio;
    uint8_t  instance = 0u;
    uint8_t  alternate = 0u;
    uint8_t  channel = 0u;
};

/// Уникален ли функциональный слот (signal, instance, channel).
///
/// GPIO/LED/Button — это не «слот периферии»: несколько светодиодов и кнопок
/// законны. Для всех остальных сигналов один и тот же слот не может быть
/// выведен на две ноги.
constexpr bool unique_slot(Signal signal) noexcept {
    switch (signal) {
        case Signal::Gpio:
        case Signal::Led:
        case Signal::Button:
            return false;
        default:
            return true;
    }
}

/// Нужен ли сигналу AF-номер (для проверки диапазона).
constexpr bool needs_alternate(Signal signal) noexcept {
    switch (signal) {
        case Signal::Gpio:
        case Signal::Led:
        case Signal::Button:
        case Signal::Adc:
        case Signal::Exti:
            return false;
        default:
            return true;
    }
}

/// Карта пинов фиксированного размера (размер известен на этапе компиляции —
/// никакой динамики и никакого «массива до 64»).
template <std::size_t N>
struct PinMap {
    PinUse uses[N]{};

    constexpr std::size_t size() const noexcept { return N; }

    /// Найти использование по слоту. nullptr — слот не объявлен.
    constexpr const PinUse* find(Signal signal, uint8_t instance,
                                 uint8_t channel = 0u) const noexcept {
        for (std::size_t i = 0u; i < N; ++i) {
            if (uses[i].signal == signal &&
                uses[i].instance == instance &&
                uses[i].channel == channel) {
                return &uses[i];
            }
        }
        return nullptr;
    }

    constexpr bool has(Signal signal, uint8_t instance,
                       uint8_t channel = 0u) const noexcept {
        return find(signal, instance, channel) != nullptr;
    }

    /// Пин слота или невалидный Pin, если слот не объявлен.
    constexpr Pin pin(Signal signal, uint8_t instance,
                      uint8_t channel = 0u) const noexcept {
        const PinUse* use = find(signal, instance, channel);
        return (use != nullptr) ? use->pin : Pin{};
    }

    /// AF-номер слота (0, если слот не объявлен или AF не задан).
    constexpr uint8_t alternate(Signal signal, uint8_t instance,
                                uint8_t channel = 0u) const noexcept {
        const PinUse* use = find(signal, instance, channel);
        return (use != nullptr) ? use->alternate : 0u;
    }

    /// Сколько конфликтов в карте (0 — карта пригодна).
    ///
    /// Считаются оба класса ошибок:
    ///   1) одна физическая нога объявлена дважды (в любых ролях);
    ///   2) один функциональный слот объявлен на двух ногах.
    /// Плюс проверяются диапазоны: номер пина 0..15 и AF 0..15.
    constexpr std::size_t conflicts() const noexcept {
        std::size_t bad = 0u;
        for (std::size_t i = 0u; i < N; ++i) {
            if (!uses[i].pin.valid()) ++bad;
            if (needs_alternate(uses[i].signal) && uses[i].alternate > 15u) ++bad;

            for (std::size_t j = i + 1u; j < N; ++j) {
                if (uses[i].pin == uses[j].pin) ++bad;

                if (unique_slot(uses[i].signal) &&
                    uses[i].signal == uses[j].signal &&
                    uses[i].instance == uses[j].instance &&
                    uses[i].channel == uses[j].channel) {
                    ++bad;
                }
            }
        }
        return bad;
    }

    /// Карта пригодна: все пины заданы, конфликтов нет.
    constexpr bool valid() const noexcept { return conflicts() == 0u; }
};

/// Проверка карты без обращения к объекту (удобно для static_assert).
template <std::size_t N>
constexpr bool pins_valid(const PinMap<N>& map) noexcept {
    return map.valid();
}

/// Описание платы: имя + карта. Имя — для диагностики/печати, не для логики.
template <std::size_t N>
struct BoardPins {
    const char* name = nullptr;
    PinMap<N>   map{};

    constexpr bool valid() const noexcept { return map.valid(); }
};

} // namespace board
