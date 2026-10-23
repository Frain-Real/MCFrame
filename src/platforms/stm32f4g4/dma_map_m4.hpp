// platforms/stm32f4g4/dma_map_m4.hpp
// ---------------------------------------------------------------------------
// DMA ДЛЯ F4/G4: МАРШРУТЫ, А НЕ «ТАБЛИЦА ИЗ RM0008».
//
// Прежняя проблема: `os/config/dma_config.hpp` описывал только DMA1
// STM32F103 (7 каналов, фиксированные запросы). F4 живёт на потоках DMA1/DMA2
// с CHSEL, G4 — на каналах DMA1 с DMAMUX. Поэтому маршрут здесь — это
// (контроллер, поток/канал, номер запроса/AF), а не «номер канала 0..6».
//
// Драйверы (SPI/UART/I2C/ADC) НЕ пишут регистры DMA напрямую: они берут
// Route, арендуют его через DmaLease (простая арбитрация «канал занят») и
// вызывают dma_start()/dma_wait(). Так один и тот же драйвер работает на
// обоих семействах, а не «умеет только F1».
//
// ВАЖНО: маршруты — значения по умолчанию из RM0090 (F4) и RM0440 (G4).
// Если плата развела DMA иначе, приложение может передать свой Route в
// драйвер (см. конструктор Spi/I2c) — таблица не является «истиной в
// последней инстанции».
// ---------------------------------------------------------------------------
#pragma once

#include "stm32m4_min.h"

#include <cstdint>

namespace embed::stm32::dma {

/// Реестр занятости маршрутов (inline — один на весь образ).
inline bool g_dma_busy[3][8]{};

enum class Request : uint8_t {
    Spi1Rx, Spi1Tx, Spi2Rx, Spi2Tx,
    Usart1Rx, Usart1Tx, Usart2Rx, Usart2Tx,
    I2c1Rx, I2c1Tx, I2c2Rx, I2c2Tx,
    Adc1,
};

/// Маршрут DMA: контроллер + поток/канал + селектор/запрос.
struct Route {
    uint8_t controller = 0u;   ///< 1 или 2 (DMA1/DMA2)
    uint8_t index      = 0u;   ///< 0..7: поток F4 или канал G4
    uint8_t channel    = 0u;   ///< F4: CHSEL 0..7; G4 не используется
    uint8_t request    = 0u;   ///< G4: номер запроса DMAMUX; F4 не используется

    constexpr bool valid() const noexcept {
        return (controller == 1u || controller == 2u) && index < 8u;
    }
};

constexpr Route route(Request request) noexcept {
#if defined(MCFRAME_SOC_STM32G4)
    switch (request) {
        case Request::Spi1Rx:  return Route{1u, 0u, 0u, 11u};
        case Request::Spi1Tx:  return Route{1u, 1u, 0u, 12u};
        case Request::Spi2Rx:  return Route{1u, 2u, 0u, 13u};
        case Request::Spi2Tx:  return Route{1u, 3u, 0u, 14u};
        case Request::Usart1Rx:return Route{1u, 4u, 0u, 31u};
        case Request::Usart1Tx:return Route{1u, 5u, 0u, 32u};
        case Request::Usart2Rx:return Route{1u, 6u, 0u, 33u};
        case Request::Usart2Tx:return Route{1u, 7u, 0u, 34u};
        case Request::I2c1Rx:  return Route{1u, 0u, 0u, 17u};
        case Request::I2c1Tx:  return Route{1u, 1u, 0u, 18u};
        case Request::I2c2Rx:  return Route{1u, 2u, 0u, 19u};
        case Request::I2c2Tx:  return Route{1u, 3u, 0u, 20u};
        case Request::Adc1:    return Route{1u, 4u, 0u, 1u};
    }
#else
    switch (request) {
        case Request::Spi1Rx:  return Route{2u, 0u, 3u, 0u};   // DMA2 Stream0/Ch3
        case Request::Spi1Tx:  return Route{2u, 3u, 3u, 0u};   // DMA2 Stream3/Ch3
        case Request::Spi2Rx:  return Route{1u, 3u, 0u, 0u};   // DMA1 Stream3/Ch0
        case Request::Spi2Tx:  return Route{1u, 4u, 0u, 0u};   // DMA1 Stream4/Ch0
        case Request::Usart1Rx:return Route{2u, 2u, 4u, 0u};   // DMA2 Stream2/Ch4
        case Request::Usart1Tx:return Route{2u, 7u, 4u, 0u};   // DMA2 Stream7/Ch4
        case Request::Usart2Rx:return Route{1u, 5u, 4u, 0u};   // DMA1 Stream5/Ch4
        case Request::Usart2Tx:return Route{1u, 6u, 4u, 0u};   // DMA1 Stream6/Ch4
        case Request::I2c1Rx:  return Route{1u, 0u, 1u, 0u};   // DMA1 Stream0/Ch1
        case Request::I2c1Tx:  return Route{1u, 6u, 1u, 0u};   // DMA1 Stream6/Ch1
        case Request::I2c2Rx:  return Route{1u, 2u, 7u, 0u};   // DMA1 Stream2/Ch7
        case Request::I2c2Tx:  return Route{1u, 7u, 7u, 0u};   // DMA1 Stream7/Ch7
        case Request::Adc1:    return Route{2u, 0u, 0u, 0u};   // DMA2 Stream0/Ch0
    }
#endif
    return Route{};
}

/// Аренда маршрута: две операции не должны одновременно занять один поток.
///
/// Это не полноценный арбитр F1 (с очередью и приоритетами) — здесь только
/// детектор «канал уже занят». Для SPI/UART/I2C одного драйвера этого
/// достаточно; приложению с несколькими одновременными DMA-потоками нужен
/// собственный диспетчер.
class DmaLease {
public:
    DmaLease() noexcept = default;
    DmaLease(const DmaLease&) = delete;
    DmaLease& operator=(const DmaLease&) = delete;

    ~DmaLease() noexcept { release(); }

    bool acquire(const Route& r) noexcept {
        if (held_ || !r.valid()) return false;
        bool& busy = slot(r);
        if (busy) return false;
        busy = true;
        route_ = r;
        held_ = true;
        return true;
    }

    void release() noexcept {
        if (!held_) return;
        slot(route_) = false;
        held_ = false;
        route_ = Route{};
    }

    bool held() const noexcept { return held_; }
    const Route& route() const noexcept { return route_; }

private:
    static bool& slot(const Route& r) noexcept {
        // controller 1/2, index 0..7. Inline-переменная, а не static внутри
        // функции: иначе каждый TU получил бы СВОЙ реестр занятости, и
        // драйверы из разных единиц трансляции не видели бы друг друга.
        return g_dma_busy[(r.controller <= 2u) ? r.controller : 0u][r.index & 7u];
    }

    Route route_{};
    bool  held_ = false;
};

// ─────────────────────────── Низкий уровень ───────────────────────────

#if defined(MCFRAME_SOC_STM32G4)

inline volatile DMA_Channel_TypeDef* channel_regs(const Route& r) noexcept {
    if (!r.valid() || r.controller != 1u) return nullptr;
    auto* base = reinterpret_cast<volatile uint8_t*>(DMA1);
    return reinterpret_cast<volatile DMA_Channel_TypeDef*>(
        base + 0x08u + 0x14u * r.index);
}

inline uint32_t dma_remaining(const Route& r) noexcept {
    const auto* c = channel_regs(r);
    return (c != nullptr) ? c->CNDTR : 0u;
}

inline void dma_clear_flags(const Route& /*r*/) noexcept {
    DMA1->IFCR = 0xFFFFFFFFu;
}

inline bool dma_start(const Route& r, volatile void* periph, void* mem,
                      uint32_t len, bool to_peripheral) noexcept {
    auto* c = channel_regs(r);
    if (c == nullptr || periph == nullptr || mem == nullptr || len == 0u) return false;
    if (r.request != 0u) DMAMUX1->CCMR[r.index] = r.request;

    c->CCR = 0u;
    c->CPAR = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(periph));
    c->CMAR = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem));
    c->CNDTR = len;
    c->CCR = DMA_CH_CCR_MINC | (to_peripheral ? DMA_CH_CCR_DIR : 0u);
    c->CCR |= DMA_CH_CCR_EN;
    return true;
}

inline void dma_stop(const Route& r) noexcept {
    auto* c = channel_regs(r);
    if (c != nullptr) c->CCR &= ~DMA_CH_CCR_EN;
}

#else

inline volatile DMA_Stream_TypeDef* stream_regs(const Route& r) noexcept {
    if (!r.valid()) return nullptr;
    auto* base = (r.controller == 2u) ? DMA2 : DMA1;
    return reinterpret_cast<volatile DMA_Stream_TypeDef*>(
        reinterpret_cast<uintptr_t>(base) + 0x10u + 0x18u * r.index);
}

inline uint32_t dma_remaining(const Route& r) noexcept {
    const auto* s = stream_regs(r);
    return (s != nullptr) ? s->NDTR : 0u;
}

inline void dma_clear_flags(const Route& r) noexcept {
    auto* base = (r.controller == 2u) ? DMA2 : DMA1;
    base->LIFCR = 0xFFFFFFFFu;
    base->HIFCR = 0xFFFFFFFFu;
}

inline bool dma_start(const Route& r, volatile void* periph, void* mem,
                      uint32_t len, bool to_peripheral) noexcept {
    auto* s = stream_regs(r);
    if (s == nullptr || periph == nullptr || mem == nullptr || len == 0u) return false;

    s->CR = 0u;
    s->PAR = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(periph));
    s->M0AR = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem));
    s->NDTR = len;
    s->FCR = 0u;
    s->CR = (static_cast<uint32_t>(r.channel & 7u) << 25u) |
            DMA_STREAM_CCR_MINC |
            (to_peripheral ? DMA_STREAM_CCR_DIR : 0u);
    s->CR |= DMA_STREAM_CCR_EN;
    return true;
}

inline void dma_stop(const Route& r) noexcept {
    auto* s = stream_regs(r);
    if (s != nullptr) s->CR &= ~DMA_STREAM_CCR_EN;
}

#endif

} // namespace embed::stm32::dma
