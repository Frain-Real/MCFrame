// platforms/stm32/platform_adc.cpp
// ---------------------------------------------------------------------------
// АЦП для STM32F103 (ADC1, 12 бит, до 16 внешних каналов).
//
// Реализация опирается на существующий DMA-слой (platform_dma_*), а не
// дублирует регистровый код: прежде DMA-логика была бы скопирована, и
// ошибки флагов/масок размножались бы по двум местам.
//
// ADC1 обслуживается каналом DMA1 Channel1 (global id 0) — единственный
// маппинг ADC1 на F103 (см. RM0008, таблица 78).
// ---------------------------------------------------------------------------
#include "../../os/adc/adc.hpp"
#include "../../os/interfaces/dma_manager.hpp"
#include "../../os/config/dma_config.hpp"

#include "stm32f10x.h"
#include "core_cm3.h"

namespace embed {
namespace {

constexpr uint8_t  kAdcDmaGlobalId = 0u;       // DMA1 Channel1
constexpr uint32_t kCalibrationIters = 0x000FFFFFu;

bool     g_running = false;
uint8_t  g_scan_len = 1u;
uint8_t  g_next_pos = 0u;
Size     g_dma_samples = 0u;

} // namespace

bool platform_adc_init(const void* cfg, Size cfg_size) noexcept {
    (void)cfg;
    (void)cfg_size;

    // Длину последовательности и номера каналов задаёт приложение через
    // platform_adc_setChannels() — ядро не знает про SQR-регистры.

    // Тактирование ADC1 (APB2) и GPIO для входов настраивает приложение
    // либо HAL: здесь только сам АЦП.
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    // Выход из спящего режима и включение.
    ADC1->CR2 |= ADC_CR2_ADON;

    // Калибровка (рекомендована RM0008 для повышения точности).
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    for (uint32_t i = 0; i < kCalibrationIters && (ADC1->CR2 & ADC_CR2_RSTCAL); ++i) { }

    ADC1->CR2 |= ADC_CR2_CAL;
    for (uint32_t i = 0; i < kCalibrationIters && (ADC1->CR2 & ADC_CR2_CAL); ++i) { }

    // Внешний триггер не используется: конверсия запускается программно
    // (EXTSEL = SWSTART), выравнивание вправо.
    ADC1->CR2 &= ~(ADC_CR2_EXTSEL | ADC_CR2_EXTTRIG | ADC_CR2_ALIGN);

    g_running  = false;
    g_next_pos = 0u;
    return true;
}

bool platform_adc_start() noexcept {
    ADC1->CR2 |= ADC_CR2_ADON;      // повторная запись = запуск при SWSTART
    g_next_pos = 0u;
    g_running  = true;
    return true;
}

bool platform_adc_stop() noexcept {
    ADC1->CR2 &= ~ADC_CR2_ADON;
    g_running = false;
    return true;
}

void platform_adc_enableIrq(bool enable) noexcept {
    if (enable) {
        ADC1->CR1 |= ADC_CR1_EOCIE;
        NVIC->ISER[static_cast<uint32_t>(ADC1_2_IRQn) >> 5u] =
            (1UL << (static_cast<uint32_t>(ADC1_2_IRQn) & 0x1Fu));
    } else {
        ADC1->CR1 &= ~ADC_CR1_EOCIE;
        NVIC->ICER[static_cast<uint32_t>(ADC1_2_IRQn) >> 5u] =
            (1UL << (static_cast<uint32_t>(ADC1_2_IRQn) & 0x1Fu));
    }
}

bool platform_adc_startScan() noexcept {
    if (!g_running) return false;
    g_next_pos = 0u;

    // Настраиваем длину регулярной последовательности один раз.
    ADC1->SQR1 = static_cast<uint32_t>(g_scan_len - 1u) << 20u;

    ADC1->CR2 |= ADC_CR2_EXTTRIG;   // программный запуск (SWSTART)
    ADC1->CR2 |= ADC_CR2_SWSTART;
    return true;
}

bool platform_adc_pollResult(uint8_t& pos, AdcSample& val) noexcept {
    if (!g_running) return false;
    if (g_next_pos >= g_scan_len) return false;
    if ((ADC1->SR & ADC_SR_EOC) == 0u) return false;

    pos = g_next_pos;
    val = static_cast<AdcSample>(ADC1->DR & 0x0FFFu);   // чтение сбрасывает EOC
    ++g_next_pos;
    return true;
}

bool platform_adc_configureDma(AdcSample* buf, Size samples, bool circular) noexcept {
    if (buf == nullptr || samples == 0u) return false;
    if ((samples % g_scan_len) != 0u) return false;   // кратно числу каналов

    g_dma_samples = samples;

    const dma::BufferConfig cfg{
        buf,
        samples * sizeof(AdcSample),
        /*increment*/ true,
    };

    const Status st = platform_dma_configure_periph_to_mem(
        kAdcDmaGlobalId,
        config::DmaRequest::Adc1,
        cfg,
        dma::TransferSize::HalfWord,
        dma::Priority::High,
        dma::Trigger::Hardware,
        circular ? dma::BurstMode::Circular : dma::BurstMode::Single);

    if (st != Status::Ok) return false;

    // DMA-запрос от ADC1 и режим непрерывного преобразования при circular.
    ADC1->CR2 |= ADC_CR2_DMA;
    if (circular) ADC1->CR2 |= ADC_CR2_CONT;

    // Прерывания по половине и концу передачи.
    const uint8_t ctrl = 1u, ch = 1u;
    (void)ctrl;
    (void)ch;
    platform_dma_set_callback(kAdcDmaGlobalId, nullptr, nullptr);
    return true;
}

bool platform_adc_startDma() noexcept {
    if (g_dma_samples == 0u) return false;
    if (platform_dma_start(kAdcDmaGlobalId) != Status::Ok) return false;
    platform_adc_startScan();
    return true;
}

void platform_adc_stopDma() noexcept {
    (void)platform_dma_stop(kAdcDmaGlobalId);
    ADC1->CR2 &= ~(ADC_CR2_DMA | ADC_CR2_CONT);
}

void platform_adc_onIrq() noexcept {}

} // namespace embed

namespace embed {

/// Задать регулярную последовательность каналов (порядок сканирования).
/// Вызывается приложением/HAL до start().
bool platform_adc_setChannels(const uint8_t* channels, Size count) noexcept {
    if (channels == nullptr || count == 0u || count > 16u) return false;

    // SQR1[23:20] = L (длина - 1); SQR3 — каналы 1..6, SQR2 — 7..12.
    ADC1->SQR1 = static_cast<uint32_t>(count - 1u) << 20u;
    ADC1->SQR2 = 0u;
    ADC1->SQR3 = 0u;

    for (Size i = 0; i < count; ++i) {
        const uint32_t ch = static_cast<uint32_t>(channels[i]) & 0x1Fu;
        if (i < 6u) {
            ADC1->SQR3 |= ch << (i * 5u);
        } else if (i < 12u) {
            ADC1->SQR2 |= ch << ((i - 6u) * 5u);
        } else {
            ADC1->SQR1 |= ch << ((i - 12u) * 5u);
        }
    }
    g_scan_len = static_cast<uint8_t>(count);
    return true;
}

/// Задать время выборки для канала (SMPR2/SMPR1, по 3 бита).
bool platform_adc_setSampleTime(uint8_t channel, uint8_t cycles_code) noexcept {
    if (channel > 17u || cycles_code > 7u) return false;
    if (channel < 10u) {
        ADC1->SMPR2 |= static_cast<uint32_t>(cycles_code) << (channel * 3u);
    } else {
        ADC1->SMPR1 |= static_cast<uint32_t>(cycles_code) << ((channel - 10u) * 3u);
    }
    return true;
}

} // namespace embed
