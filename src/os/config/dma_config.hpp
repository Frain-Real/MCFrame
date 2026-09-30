#pragma once
#include <cstdint>

namespace embed::config {

// Глобальная нумерация каналов:
// ID 0-6:   DMA1 Channel 1-7
// ID 7-11:  DMA2 Channel 1-5

constexpr uint8_t DMA1_CHANNELS = 7;
constexpr uint8_t DMA2_CHANNELS = 5;
constexpr uint8_t TOTAL_CHANNELS = DMA1_CHANNELS + DMA2_CHANNELS;

struct DeviceMapping {
    const char* device;
    bool        is_tx;
    uint8_t     channels[5];
    uint8_t     channel_count;
};

// Конфигурация для STM32F103
inline constexpr DeviceMapping mappings[] = {
    // === DMA1 (ID 0-6) ===
    
    // ADC1 — только DMA1 Channel 1 (ID 0)
    { "ADC1",   false, {0}, 1 },
    
    // SPI1 — DMA1 Channel 2/3 (ID 1/2)
    { "SPI1",   false, {1}, 1 },  // RX
    { "SPI1",   true,  {2}, 1 },  // TX
    
    // SPI2 — DMA1 Channel 4/5 (ID 3/4)
    { "SPI2",   false, {3}, 1 },  // RX
    { "SPI2",   true,  {4}, 1 },  // TX
    
    // USART1 — DMA1 Channel 4/5 (ID 3/4)
    { "USART1", true,  {3}, 1 },  // TX
    { "USART1", false, {4}, 1 },  // RX
    
    // USART2 — DMA1 Channel 6/7 (ID 5/6)
    { "USART2", false, {5}, 1 },  // RX
    { "USART2", true,  {6}, 1 },  // TX
    
    // USART3 — DMA1 Channel 2/3 (ID 1/2)
    { "USART3", true,  {1}, 1 },  // TX
    { "USART3", false, {2}, 1 },  // RX
    
    // I2C1 — DMA1 Channel 6/7 (ID 5/6)
    { "I2C1",   true,  {5}, 1 },  // TX
    { "I2C1",   false, {6}, 1 },  // RX
    
    // I2C2 — DMA1 Channel 4/5 (ID 3/4)
    { "I2C2",   true,  {3}, 1 },  // TX
    { "I2C2",   false, {4}, 1 },  // RX
    
    // TIM2 — может использовать несколько каналов
    { "TIM2",   true,  {1, 2, 5, 6}, 4 },
    
    // === DMA2 (ID 7-11) ===
    
    // ADC2 — DMA2 Channel 1 (ID 7)
    { "ADC2",   false, {7}, 1 },
    
    // ADC3 — DMA2 Channel 2 (ID 8)
    { "ADC3",   false, {8}, 1 },
    
    // TIM5 — DMA2 Channel 1-5 (ID 7-11)
    { "TIM5",   true,  {7, 8, 9, 10, 11}, 5 },
    
    // TIM8 — DMA2 Channel 2-5 (ID 8-11)
    { "TIM8",   true,  {8, 9, 10, 11}, 4 },
    
    // SPI3 — DMA2 Channel 1/2 (ID 7/8)
    { "SPI3",   false, {7}, 1 },  // RX
    { "SPI3",   true,  {8}, 1 },  // TX
};

inline constexpr uint8_t mappings_count = sizeof(mappings) / sizeof(mappings[0]);

} // namespace os::dma::config