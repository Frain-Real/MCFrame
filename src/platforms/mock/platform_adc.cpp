// platforms/mock/platform_adc.cpp
// ---------------------------------------------------------------------------
// Заглушка АЦП для mock-сборки.
//
// Предназначена для проверки логики драйвера ADC<Kernel, Hal, N> без железа:
// эмулирует последовательность готовых отсчётов по каналам, чтобы
// readFrame()/onConversionComplete() можно было прогонять в тестах.
// ---------------------------------------------------------------------------
#include "../../os/adc/adc.hpp"

#include <cstdint>

namespace {

constexpr uint8_t kSimChannels = 8u;   // сколько каналов эмулируем
uint8_t              g_scan_len    = kSimChannels;
uint8_t              g_next_pos    = 0u;
embed::AdcSample     g_counter     = 0u;
bool                 g_running     = false;
bool                 g_dma_on      = false;

embed::AdcSample*    g_dma_buf     = nullptr;
embed::Size          g_dma_samples = 0u;

} // namespace

namespace embed {

bool platform_adc_init(const void* cfg, Size cfg_size) noexcept {
    (void)cfg;
    (void)cfg_size;
    g_next_pos = 0u;
    g_counter  = 0u;
    g_running  = false;
    return true;
}

bool platform_adc_start() noexcept {
    g_next_pos = 0u;
    g_counter  = 0u;
    g_running  = true;
    return true;
}

bool platform_adc_stop() noexcept {
    g_running = false;
    return true;
}

bool platform_adc_startScan() noexcept {
    g_next_pos = 0u;      // начинаем обход каналов заново
    return g_running;
}

bool platform_adc_pollResult(uint8_t& pos, AdcSample& val) noexcept {
    if (!g_running || g_next_pos >= kSimChannels) return false;
    pos = g_next_pos++;
    val = ++g_counter;    // предсказуемая последовательность для тестов
    return true;
}

void platform_adc_enableIrq(bool enable) noexcept { (void)enable; }

bool platform_adc_configureDma(AdcSample* buf, Size samples, bool circular) noexcept {
    if (buf == nullptr || samples == 0u) return false;
    (void)circular;
    g_dma_buf     = buf;
    g_dma_samples = samples;
    return true;
}

bool platform_adc_startDma() noexcept {
    g_dma_on = (g_dma_buf != nullptr);
    return g_dma_on;
}

void platform_adc_stopDma() noexcept { g_dma_on = false; }

void platform_adc_onIrq() noexcept {}

bool platform_adc_setChannels(const uint8_t* channels, Size count) noexcept {
    if (channels == nullptr || count == 0u) return false;
    g_scan_len = (count > kSimChannels) ? kSimChannels : static_cast<uint8_t>(count);
    return true;
}

bool platform_adc_setSampleTime(uint8_t channel, uint8_t cycles_code) noexcept {
    if (channel >= kSimChannels || cycles_code > 7u) return false;
    return true;
}

} // namespace embed
