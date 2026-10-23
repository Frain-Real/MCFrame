// tests/test_m4_drivers.cpp
// ---------------------------------------------------------------------------
// F4: маршруты DMA и карта пинов.
//
// Драйверы F4/G4 проверяются кросс-сборкой (регистры/типы), а здесь —
// то, что можно проверить без железа: таблица маршрутов не пересекается,
// аренда канала работает, карта пинов f407 корректна.
// ---------------------------------------------------------------------------
#include "dma_map_m4.hpp"
#include "boards/f407_generic/pins.hpp"

#include <cstdio>
#include <cstring>

namespace {

int g_failed = 0;
int g_total = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

using embed::stm32::dma::DmaLease;
using embed::stm32::dma::Request;
using embed::stm32::dma::Route;

constexpr Route kSpi1Rx = embed::stm32::dma::route(Request::Spi1Rx);
constexpr Route kSpi1Tx = embed::stm32::dma::route(Request::Spi1Tx);
constexpr Route kSpi2Rx = embed::stm32::dma::route(Request::Spi2Rx);
constexpr Route kSpi2Tx = embed::stm32::dma::route(Request::Spi2Tx);

static_assert(kSpi1Rx.valid() && kSpi1Tx.valid(), "SPI1 DMA route invalid");
static_assert(kSpi2Rx.valid() && kSpi2Tx.valid(), "SPI2 DMA route invalid");
static_assert(kSpi1Rx.controller == 2u && kSpi1Rx.index == 0u, "SPI1_RX = DMA2 Stream0");
static_assert(kSpi1Tx.controller == 2u && kSpi1Tx.index == 3u, "SPI1_TX = DMA2 Stream3");
static_assert(kSpi2Rx.controller == 1u && kSpi2Rx.index == 3u, "SPI2_RX = DMA1 Stream3");
static_assert(kSpi2Tx.controller == 1u && kSpi2Tx.index == 4u, "SPI2_TX = DMA1 Stream4");
static_assert(kSpi1Rx.channel == 3u && kSpi1Tx.channel == 3u, "SPI1 CHSEL=3");
static_assert(board::kPins.valid(), "f407 pin map invalid");

bool same_slot(Route a, Route b) {
    return a.controller == b.controller && a.index == b.index;
}

} // namespace

int main() {
    std::printf("=== F4 DMA routes / board map ===\n");

    check("SPI1 RX/TX на разных потоках", !same_slot(kSpi1Rx, kSpi1Tx));
    check("SPI2 RX/TX на разных потоках", !same_slot(kSpi2Rx, kSpi2Tx));
    check("SPI1 и SPI2 не делят поток", !same_slot(kSpi1Rx, kSpi2Rx) &&
                                        !same_slot(kSpi1Tx, kSpi2Tx));
    check("DMA2 Stream0 — SPI1_RX", kSpi1Rx.controller == 2u && kSpi1Rx.index == 0u);
    check("несуществующий контроллер отвергнут", !Route{3u, 0u, 0u, 0u}.valid());
    check("индекс 8 отвергнут", !Route{1u, 8u, 0u, 0u}.valid());

    DmaLease lease1;
    DmaLease lease2;
    check("первая аренда", lease1.acquire(kSpi1Rx));
    check("повторная аренда занятого маршрута", !lease2.acquire(kSpi1Rx));
    check("другой маршрут свободен", lease2.acquire(kSpi1Tx));
    lease1.release();
    lease2.release();
    check("после release маршрут свободен", lease1.acquire(kSpi1Rx));
    lease1.release();

    check("f407: USART1_TX на PA9",
          board::kPins.pin(board::Signal::UartTx, 1u) == board::Pin{board::Port::A, 9u});
    check("f407: I2C2 на PB10/PB11",
          board::kPins.pin(board::Signal::I2cScl, 2u) == board::Pin{board::Port::B, 10u} &&
          board::kPins.pin(board::Signal::I2cSda, 2u) == board::Pin{board::Port::B, 11u});
    check("f407: AF SPI1 = 5",
          board::kPins.alternate(board::Signal::SpiSck, 1u) == 5u);
    check("f407: нет конфликтов", board::kPins.conflicts() == 0u);

    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
