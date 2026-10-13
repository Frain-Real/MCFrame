// dma_config.hpp
#pragma once
#include <cstdint>

namespace embed::config {

// DMA STM32F103RB (medium density): ТОЛЬКО DMA1, 7 каналов.
// DMA2 у этого кристалла отсутствует физически, поэтому его каналы
// и запросы (ADC2/ADC3/SPI3/TIM5/TIM8/I2C2) здесь не описаны:
// прежде они приводили к записи в несуществующие регистры и HardFault.
constexpr uint8_t DMA1_CHANNELS = 7;
constexpr uint8_t DMA2_CHANNELS = 0;
constexpr uint8_t TOTAL_CHANNELS = DMA1_CHANNELS + DMA2_CHANNELS;

/// Запросы DMA. Строгое перечисление вместо строк: опечатка в имени
/// периферии теперь ошибка компиляции, а не «NotSupported» в рантайме.
enum class DmaRequest : uint8_t {
    None = 0,

    Adc1,        // DMA1 Ch1

    Spi1Rx,      // DMA1 Ch2
    Spi1Tx,      // DMA1 Ch3

    Spi2Rx,      // DMA1 Ch4
    Spi2Tx,      // DMA1 Ch5

    Usart1Tx,    // DMA1 Ch4  (совпадает со Spi2Rx — взаимоисключающие)
    Usart1Rx,    // DMA1 Ch5

    Usart2Rx,    // DMA1 Ch6
    Usart2Tx,    // DMA1 Ch7

    Usart3Tx,    // DMA1 Ch3
    Usart3Rx,    // DMA1 Ch2

    I2c1Tx,      // DMA1 Ch6
    I2c1Rx,      // DMA1 Ch7

    Count_
};

/// Описание запроса: в каком канале он может обслуживаться.
/// Один запрос может допускать несколько каналов (альтернативные маппинги).
struct DeviceMapping {
    DmaRequest request;
    uint8_t    channels[2];
    uint8_t    channel_count;
};

// Соответствие по RM0008, таблица 78 (DMA1).
inline constexpr DeviceMapping mappings[] = {
    { DmaRequest::Adc1,     {0},    1 },
    { DmaRequest::Spi1Rx,   {1},    1 },
    { DmaRequest::Spi1Tx,   {2},    1 },
    { DmaRequest::Spi2Rx,   {3},    1 },
    { DmaRequest::Spi2Tx,   {4},    1 },
    { DmaRequest::Usart1Tx, {3},    1 },
    { DmaRequest::Usart1Rx, {4},    1 },
    { DmaRequest::Usart2Rx, {5},    1 },
    { DmaRequest::Usart2Tx, {6},    1 },
    { DmaRequest::Usart3Tx, {2},    1 },
    { DmaRequest::Usart3Rx, {1},    1 },
    { DmaRequest::I2c1Tx,   {5},    1 },
    { DmaRequest::I2c1Rx,   {6},    1 },
};

inline constexpr uint8_t mappings_count = sizeof(mappings) / sizeof(mappings[0]);

/// Человекочитаемое имя запроса (для логов/отладки).
inline const char* request_name(DmaRequest r) noexcept {
    switch (r) {
        case DmaRequest::None:     return "None";
        case DmaRequest::Adc1:     return "ADC1";
        case DmaRequest::Spi1Rx:   return "SPI1_RX";
        case DmaRequest::Spi1Tx:   return "SPI1_TX";
        case DmaRequest::Spi2Rx:   return "SPI2_RX";
        case DmaRequest::Spi2Tx:   return "SPI2_TX";
        case DmaRequest::Usart1Tx: return "USART1_TX";
        case DmaRequest::Usart1Rx: return "USART1_RX";
        case DmaRequest::Usart2Rx: return "USART2_RX";
        case DmaRequest::Usart2Tx: return "USART2_TX";
        case DmaRequest::Usart3Tx: return "USART3_TX";
        case DmaRequest::Usart3Rx: return "USART3_RX";
        case DmaRequest::I2c1Tx:   return "I2C1_TX";
        case DmaRequest::I2c1Rx:   return "I2C1_RX";
        case DmaRequest::Count_:   return "?";
    }
    return "?";
}

} // namespace embed::config
