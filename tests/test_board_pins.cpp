// tests/test_board_pins.cpp
// ---------------------------------------------------------------------------
// Декларативная карта пинов: поиск слотов и обнаружение конфликтов.
//
// Проверки делятся на две группы:
//   * static_assert — ошибки платы обязаны быть ошибкой КОМПИЛЯЦИИ, а не
//     рантайм-диагностикой (именно ради этого карта constexpr);
//   * runtime — позитивные запросы к карте (какой пин у USART1_TX, какой AF).
// ---------------------------------------------------------------------------
#include "boards/pin_map.hpp"
#include "boards/f103_generic/pins.hpp"

#include <cstdio>

namespace {

int g_failed = 0;
int g_total = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

using board::Pin;
using board::PinMap;
using board::PinUse;
using board::Port;
using board::Signal;

constexpr Pin PA9{Port::A, 9u};
constexpr Pin PA10{Port::A, 10u};
constexpr Pin PA5{Port::A, 5u};
constexpr Pin PA0{Port::A, 0u};

// ── Пригодная карта: USART1 + SPI1 + PWM + ADC + LED ──
constexpr PinMap<6> kGood = PinMap<6>{{
    {PA9,   Signal::UartTx, 1u, 7u, 0u},
    {PA10,  Signal::UartRx, 1u, 7u, 0u},
    {PA5,   Signal::SpiSck, 1u, 5u, 0u},
    {PA0,   Signal::Pwm,    2u, 1u, 1u},
    {Pin{Port::A, 1u}, Signal::Adc, 1u, 0u, 1u},
    {Pin{Port::C, 13u}, Signal::Led, 0u, 0u, 0u},
}};
static_assert(kGood.valid(), "пригодная карта не должна конфликтовать");
static_assert(kGood.pin(Signal::UartTx, 1u) == PA9, "USART1_TX должен быть на PA9");
static_assert(kGood.alternate(Signal::UartTx, 1u) == 7u, "AF USART1_TX должен быть 7");

// ── Одна нога в двух ролях ──
constexpr PinMap<2> kSamePin = PinMap<2>{{
    {PA9, Signal::UartTx, 1u, 7u, 0u},
    {PA9, Signal::SpiSck, 1u, 5u, 0u},
}};
static_assert(!kSamePin.valid(), "PA9 не может быть одновременно USART1_TX и SPI1_SCK");

// ── Один слот на двух ногах ──
constexpr PinMap<2> kSameSlot = PinMap<2>{{
    {PA9,  Signal::UartTx, 1u, 7u, 0u},
    {PA10, Signal::UartTx, 1u, 7u, 0u},
}};
static_assert(!kSameSlot.valid(), "USART1_TX не может быть на двух ногах");

// ── Незаданный пин и AF вне диапазона ──
constexpr PinMap<2> kBadPin = PinMap<2>{{
    {Pin{}, Signal::Led, 0u, 0u, 0u},
    {PA9,   Signal::UartTx, 1u, 7u, 0u},
}};
static_assert(!kBadPin.valid(), "пин без порта — ошибка карты");

constexpr PinMap<1> kBadAf = PinMap<1>{{
    {PA9, Signal::UartTx, 1u, 16u, 0u},
}};
static_assert(!kBadAf.valid(), "AF 16 не существует");

// ── GPIO/LED не считаются «слотом периферии»: два LED на разных ногах законны ──
constexpr PinMap<2> kTwoLeds = PinMap<2>{{
    {Pin{Port::C, 13u}, Signal::Led, 0u, 0u, 0u},
    {Pin{Port::C, 14u}, Signal::Led, 1u, 0u, 0u},
}};
static_assert(kTwoLeds.valid(), "два LED — не конфликт функционального слота");

} // namespace

int main() {
    std::printf("=== Декларативная карта пинов ===\n");

    check("карта f103_generic пригодна", board::kPins.valid());
    check("USART1_TX на PA9", board::kPins.pin(board::Signal::UartTx, 1u) == PA9);
    check("USART1_RX на PA10", board::kPins.pin(board::Signal::UartRx, 1u) == PA10);
    check("SPI1_SCK на PA5", board::kPins.pin(board::Signal::SpiSck, 1u) == PA5);
    check("SPI1_CS на PA4",
          board::kPins.pin(board::Signal::SpiCs, 1u) == Pin{Port::A, 4u});
    check("PWM TIM2_CH1 на PA0",
          board::kPins.pin(board::Signal::Pwm, 2u, 1u) == PA0);
    check("ADC1 IN1 на PA1",
          board::kPins.pin(board::Signal::Adc, 1u, 1u) == Pin{Port::A, 1u});
    check("LED на PC13",
          board::kPins.pin(board::Signal::Led, 0u) == Pin{Port::C, 13u});
    check("несуществующий слот не находится",
          !board::kPins.has(board::Signal::CanTx, 1u));

    std::printf("  конфликтов в f103_generic: %u\n",
                static_cast<unsigned>(board::kPins.conflicts()));
    check("конфликтов нет", board::kPins.conflicts() == 0u);

    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
