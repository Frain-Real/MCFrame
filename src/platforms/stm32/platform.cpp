// platforms/stm32/platform.cpp
// ---------------------------------------------------------------------------
// DMA-слой для STM32F103 (DMA1, 7 каналов).
//
// Исправлено относительно прежней версии:
//  * убран DMA2 — у STM32F103RB его физически нет (TOTAL_CHANNELS = 7);
//  * маски ISR/IFCR считаются как (channel-1)*4, а не << channel;
//  * enable_dma_clock() пишет в &RCC->AHBENR, а не по значению регистра;
//  * NVIC настраивается через NVIC->ISER/ICER (CMSIS), а не через
//    несуществующие макросы NVIC_ISER_SETENA_*;
//  * CNDTR проверяется на кратность размеру элемента и на 16-битность;
//  * таблица запросов соответствует RM0008.
// ---------------------------------------------------------------------------
#include "../../os/interfaces/dma_manager.hpp"
#include "../../os/config/dma_config.hpp"
#include "uart_stm32.hpp"
#include "i2c_stm32.hpp"

#include "stm32f10x.h"
#include "core_cm3.h"

#include <cstdint>

namespace embed {
namespace {

// ─────────────────────────── Описание периферии ─────────────────────────────
struct PeripheralRegs {
    config::DmaRequest request;
    uint32_t           tx_reg_addr;   // 0 = передача не поддерживается
    uint32_t           rx_reg_addr;   // 0 = приём не поддерживается
};

// DR — volatile-поле регистровой структуры, поэтому берём АДРЕС поля.
#define REG_ADDR(periph) (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&(periph)->DR)))

static const PeripheralRegs kPeripherals[] = {
    { config::DmaRequest::Usart1Tx, REG_ADDR(USART1), 0u             },
    { config::DmaRequest::Usart1Rx, 0u,               REG_ADDR(USART1) },
    { config::DmaRequest::Usart2Tx, REG_ADDR(USART2), 0u             },
    { config::DmaRequest::Usart2Rx, 0u,               REG_ADDR(USART2) },
    { config::DmaRequest::Usart3Tx, REG_ADDR(USART3), 0u             },
    { config::DmaRequest::Usart3Rx, 0u,               REG_ADDR(USART3) },
    { config::DmaRequest::Spi1Tx,   REG_ADDR(SPI1),   0u             },
    { config::DmaRequest::Spi1Rx,   0u,               REG_ADDR(SPI1)   },
    { config::DmaRequest::Spi2Tx,   REG_ADDR(SPI2),   0u             },
    { config::DmaRequest::Spi2Rx,   0u,               REG_ADDR(SPI2)   },
    { config::DmaRequest::I2c1Tx,   REG_ADDR(I2C1),   0u             },
    { config::DmaRequest::I2c1Rx,   0u,               REG_ADDR(I2C1)   },
    { config::DmaRequest::Adc1,     0u,               REG_ADDR(ADC1)   },
};

/// Поиск периферии по запросу. Строковое сравнение (std::strcmp) убрано:
/// теперь несоответствие имени — ошибка компиляции, а не NotSupported.
static const PeripheralRegs* find_periph(config::DmaRequest request) noexcept {
    for (const auto& p : kPeripherals) {
        if (p.request == request) return &p;
    }
    return nullptr;
}

struct ChannelCallback {
    Callback_h cb;
    void*      arg;
};

ChannelCallback g_callbacks[config::TOTAL_CHANNELS]{};

// ─────────────────────────── Работа с регистрами ────────────────────────────

/// Биты флагов канала в ISR/IFCR идут группами по 4, начиная с канала 1.
static constexpr uint32_t flag_shift(uint8_t ch) noexcept {
    return static_cast<uint32_t>(ch - 1u) * 4u;
}

static DMA_TypeDef* dma_base(uint8_t ctrl) noexcept {
    return (ctrl == 1u) ? DMA1 : DMA2;
}

static DMA_Channel_TypeDef* channel_regs(uint8_t ctrl, uint8_t ch) noexcept {
    // Каналы идут с шагом 0x14 от DMA_Channel1_BASE (база + 0x08).
    const uint32_t base = (ctrl == 1u) ? DMA1_BASE : DMA2_BASE;
    return reinterpret_cast<DMA_Channel_TypeDef*>(
        base + 0x08u + static_cast<uint32_t>(ch - 1u) * 0x14u);
}

static void enable_dma_clock(uint8_t ctrl) noexcept {
    // КЛЮЧЕВОЕ ИСПРАВЛЕНИЕ: RCC_TypeDef уже volatile, нужен АДРЕС регистра.
    // Прежний код делал reinterpret_cast значения AHBENR в указатель.
    if (ctrl == 1u) {
        RCC->AHBENR |= RCC_AHBENR_DMA1EN;
    }
#if defined(STM32F10X_HD) || defined(STM32F10X_CL) || defined(STM32F10X_XL)
    else {
        RCC->AHBENR |= RCC_AHBENR_DMA2EN;
    }
#else
    else {
        (void)ctrl;   // недостижимо: TOTAL_CHANNELS относится только к DMA1
    }
#endif
}

static uint32_t channel_irqn(uint8_t ctrl, uint8_t ch) noexcept {
    if (ctrl == 1u) {
        return static_cast<uint32_t>(DMA1_Channel1_IRQn) + (ch - 1u);
    }
#if defined(STM32F10X_HD) || defined(STM32F10X_CL) || defined(STM32F10X_XL)
    return static_cast<uint32_t>(DMA2_Channel1_IRQn) + (ch - 1u);
#else
    // DMA2 отсутствует (F103RB = medium density), поэтому ветка недостижима:
    // config::TOTAL_CHANNELS описывает только каналы DMA1.
    (void)ch;
    return static_cast<uint32_t>(DMA1_Channel1_IRQn);
#endif
}

static void nvic_enable_irq(uint32_t irqn) noexcept {
    NVIC->ISER[irqn >> 5u] = (1UL << (irqn & 0x1Fu));
}

static void nvic_disable_irq(uint32_t irqn) noexcept {
    NVIC->ICER[irqn >> 5u] = (1UL << (irqn & 0x1Fu));
}

static uint32_t priority_bits(dma::Priority prio) noexcept {
    switch (prio) {
        case dma::Priority::Low:      return 0u;
        case dma::Priority::Medium:   return DMA_CCR1_PL_0;
        case dma::Priority::High:     return DMA_CCR1_PL_1;
        case dma::Priority::VeryHigh: return DMA_CCR1_PL_0 | DMA_CCR1_PL_1;
    }
    return 0u;
}

static uint32_t size_bits(dma::TransferSize size) noexcept {
    switch (size) {
        case dma::TransferSize::Byte:     return 0u;
        case dma::TransferSize::HalfWord: return DMA_CCR1_PSIZE_0 | DMA_CCR1_MSIZE_0;
        case dma::TransferSize::Word:     return DMA_CCR1_PSIZE_1 | DMA_CCR1_MSIZE_1;
    }
    return 0u;
}

static constexpr uint32_t elem_bytes(dma::TransferSize size) noexcept {
    return 1u << static_cast<uint32_t>(size);
}

static void clear_channel_flags(uint8_t ctrl, uint8_t ch) noexcept {
    const uint32_t sh = flag_shift(ch);
    dma_base(ctrl)->IFCR =
        (DMA_ISR_GIF1 | DMA_ISR_TCIF1 | DMA_ISR_HTIF1 | DMA_ISR_TEIF1) << sh;
}

/// Регистры канала нельзя менять во время передачи: сначала снимаем EN.
static void disable_channel(DMA_Channel_TypeDef* ch) noexcept {
    ch->CCR &= ~DMA_CCR1_EN;
    __DSB();
}

static bool valid_id(uint8_t global_id) noexcept {
    return global_id < config::TOTAL_CHANNELS;
}

/// Общая настройка канала для «память <-> периферия».
static bool setup_periph(
    uint8_t           global_id,
    uint32_t          periph_addr,
    const void*       mem_addr,
    Size              bytes,
    dma::TransferSize unit_size,
    dma::Priority     prio,
    dma::BurstMode    burst,
    bool              mem_increment,
    uint32_t          dir_bit
) noexcept {
    uint8_t ctrl = 0u, ch = 0u;
    global_id_to_hw(global_id, ctrl, ch);

    const uint32_t elem = elem_bytes(unit_size);
    if (bytes == 0u || (bytes % elem) != 0u) return false;
    if ((bytes / elem) > 0xFFFFu) return false;    // CNDTR 16-битный
    if (mem_addr == nullptr) return false;

    enable_dma_clock(ctrl);
    auto* regs = channel_regs(ctrl, ch);
    disable_channel(regs);
    clear_channel_flags(ctrl, ch);

    regs->CPAR  = periph_addr;
    regs->CMAR  = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem_addr));
    regs->CNDTR = bytes / elem;

    uint32_t ccr = dir_bit;
    if (mem_increment) ccr |= DMA_CCR1_MINC;
    if (burst == dma::BurstMode::Circular) ccr |= DMA_CCR1_CIRC;
    ccr |= priority_bits(prio);
    ccr |= size_bits(unit_size);
    ccr |= DMA_CCR1_TCIE;

    regs->CCR = ccr;
    return true;
}

} // namespace

// ─────────────────────────── Публичный интерфейс ────────────────────────────

/// Глобальный id -> (контроллер, канал). Для F103RB контроллер всегда DMA1,
/// поэтому выход за границы невозможен.
void global_id_to_hw(uint8_t global_id, uint8_t& ctrl, uint8_t& ch) noexcept {
    ctrl = 1u + (global_id / config::DMA1_CHANNELS);
    ch   = 1u + (global_id % config::DMA1_CHANNELS);
}

uint8_t platform_dma_max_channels() noexcept {
    return config::TOTAL_CHANNELS;
}

Status platform_dma_configure_mem_to_periph(
    uint8_t global_id,
    config::DmaRequest request,
    const dma::BufferConfig& src,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::Trigger trig,
    dma::BurstMode burst
) noexcept {
    if (!valid_id(global_id)) return Status::Unavailable;
    if (!src.buffer || src.bytes == 0u) return Status::InvalidArg;
    (void)trig;   // аппаратный запрос задаётся в CR самой периферии

    const PeripheralRegs* p = find_periph(request);
    if (!p || p->tx_reg_addr == 0u) return Status::NotSupported;

    const bool ok = setup_periph(global_id, p->tx_reg_addr, src.buffer, src.bytes,
                                 unitSize, prio, burst, src.increment,
                                 DMA_CCR1_DIR);
    return ok ? Status::Ok : Status::InvalidArg;
}

Status platform_dma_configure_periph_to_mem(
    uint8_t global_id,
    config::DmaRequest request,
    const dma::BufferConfig& dst,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::Trigger trig,
    dma::BurstMode burst
) noexcept {
    if (!valid_id(global_id)) return Status::Unavailable;
    if (!dst.buffer || dst.bytes == 0u) return Status::InvalidArg;
    (void)trig;

    const PeripheralRegs* p = find_periph(request);
    if (!p || p->rx_reg_addr == 0u) return Status::NotSupported;

    const bool ok = setup_periph(global_id, p->rx_reg_addr, dst.buffer, dst.bytes,
                                 unitSize, prio, burst, dst.increment, 0u);
    return ok ? Status::Ok : Status::InvalidArg;
}

Status platform_dma_configure_mem_to_mem(
    uint8_t global_id,
    const dma::BufferConfig& src,
    const dma::BufferConfig& dst,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::BurstMode burst
) noexcept {
    if (!valid_id(global_id)) return Status::Unavailable;
    if (!src.buffer || !dst.buffer || src.bytes == 0u) return Status::InvalidArg;
    if (burst == dma::BurstMode::Circular) return Status::NotSupported;

    uint8_t ctrl = 0u, ch = 0u;
    global_id_to_hw(global_id, ctrl, ch);

    const uint32_t elem = elem_bytes(unitSize);
    if ((src.bytes % elem) != 0u || (src.bytes / elem) > 0xFFFFu) return Status::InvalidArg;

    enable_dma_clock(ctrl);
    auto* regs = channel_regs(ctrl, ch);
    disable_channel(regs);
    clear_channel_flags(ctrl, ch);

    regs->CPAR  = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(src.buffer));
    regs->CMAR  = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dst.buffer));
    regs->CNDTR = src.bytes / elem;

    uint32_t ccr = DMA_CCR1_MEM2MEM | DMA_CCR1_DIR;
    if (src.increment) ccr |= DMA_CCR1_PINC;
    if (dst.increment) ccr |= DMA_CCR1_MINC;
    ccr |= priority_bits(prio);
    ccr |= size_bits(unitSize);
    ccr |= DMA_CCR1_TCIE;

    regs->CCR = ccr;
    return Status::Ok;
}

Status platform_dma_start(uint8_t global_id) noexcept {
    if (!valid_id(global_id)) return Status::Unavailable;

    uint8_t ctrl = 0u, ch = 0u;
    global_id_to_hw(global_id, ctrl, ch);

    clear_channel_flags(ctrl, ch);

    auto* regs = channel_regs(ctrl, ch);
    if ((regs->CCR & DMA_CCR1_EN) != 0u) return Status::Busy;
    if (regs->CNDTR == 0u) return Status::InvalidArg;

    regs->CCR |= DMA_CCR1_EN;
    nvic_enable_irq(channel_irqn(ctrl, ch));
    return Status::Ok;
}

Status platform_dma_stop(uint8_t global_id) noexcept {
    if (!valid_id(global_id)) return Status::Unavailable;

    uint8_t ctrl = 0u, ch = 0u;
    global_id_to_hw(global_id, ctrl, ch);

    disable_channel(channel_regs(ctrl, ch));
    nvic_disable_irq(channel_irqn(ctrl, ch));
    clear_channel_flags(ctrl, ch);
    return Status::Ok;
}

bool platform_dma_is_complete(uint8_t global_id) noexcept {
    if (!valid_id(global_id)) return false;

    uint8_t ctrl = 0u, ch = 0u;
    global_id_to_hw(global_id, ctrl, ch);

    return (dma_base(ctrl)->ISR & (DMA_ISR_TCIF1 << flag_shift(ch))) != 0u;
}

Size platform_dma_bytes_remaining(uint8_t global_id) noexcept {
    if (!valid_id(global_id)) return 0u;

    uint8_t ctrl = 0u, ch = 0u;
    global_id_to_hw(global_id, ctrl, ch);
    return channel_regs(ctrl, ch)->CNDTR;
}

Status platform_dma_set_callback(uint8_t global_id, Callback_h cb, void* arg) noexcept {
    if (!valid_id(global_id)) return Status::Unavailable;
    g_callbacks[global_id].cb  = cb;
    g_callbacks[global_id].arg = arg;
    return Status::Ok;
}

Status platform_dma_clear_flags(uint8_t global_id) noexcept {
    if (!valid_id(global_id)) return Status::Unavailable;

    uint8_t ctrl = 0u, ch = 0u;
    global_id_to_hw(global_id, ctrl, ch);
    clear_channel_flags(ctrl, ch);
    return Status::Ok;
}

void platform_dma_irq_handler(uint8_t global_id) noexcept {
    if (!valid_id(global_id)) return;

    uint8_t ctrl = 0u, ch = 0u;
    global_id_to_hw(global_id, ctrl, ch);

    auto* dma = dma_base(ctrl);
    const uint32_t sh = flag_shift(ch);

    Status result = Status::Ok;
    bool   fired  = false;

    if ((dma->ISR & (DMA_ISR_TCIF1 << sh)) != 0u) {
        dma->IFCR = DMA_ISR_TCIF1 << sh;
        result = Status::Ok;
        fired  = true;
    }
    if ((dma->ISR & (DMA_ISR_TEIF1 << sh)) != 0u) {
        dma->IFCR = DMA_ISR_TEIF1 << sh;
        result = Status::HardwareError;
        fired  = true;
    }
    if (!fired) return;

    Callback_h cb  = g_callbacks[global_id].cb;
    void*      arg = g_callbacks[global_id].arg;
    if (!cb) return;

    // Контекст живёт на стеке обработчика. Если пользователю нужны данные
    // надолго — он обязан их скопировать.
    struct CbContext {
        uint8_t channel;
        Status  result;
        void*   user;
    } ctx{ global_id, result, arg };

    cb(&ctx);
}

} // namespace embed

// ─────────────── Обработчики прерываний USART и I2C ─────────────────────────
//
// Драйверы находятся в uart_stm32.hpp / i2c_stm32.hpp и не знают друг о
// друге. Обработчики — только маршрутизация: вся логика в автоматах
// драйверов (handleIrq / pump).

extern "C" void USART1_IRQHandler(void) {
    embed::stm32::UartDriver1::instance().onRxIrq();
}

extern "C" void I2C1_EV_IRQHandler(void) {
    embed::stm32::I2cDriver1::instance().onCompleteIrq();
}

extern "C" void I2C1_ER_IRQHandler(void) {
    // В ER-обработчике важен сам факт ошибки: конкретный код (AF/BERR/ARLO)
    // драйвер разбирает сам по SR1, поэтому здесь передаём общий статус.
    embed::stm32::I2cDriver1::instance().onErrorIrq(embed::Status::HardwareError);
}

// ─────────────── Обработчики прерываний каналов DMA1 ────────────────────────
// В startup-файле они объявлены слабыми (Default_Handler), поэтому
// определения здесь их перекрывают.
extern "C" void DMA1_Channel1_IRQHandler(void) { embed::platform_dma_irq_handler(0u); }
extern "C" void DMA1_Channel2_IRQHandler(void) { embed::platform_dma_irq_handler(1u); }
extern "C" void DMA1_Channel3_IRQHandler(void) { embed::platform_dma_irq_handler(2u); }
extern "C" void DMA1_Channel4_IRQHandler(void) { embed::platform_dma_irq_handler(3u); }
extern "C" void DMA1_Channel5_IRQHandler(void) { embed::platform_dma_irq_handler(4u); }
extern "C" void DMA1_Channel6_IRQHandler(void) { embed::platform_dma_irq_handler(5u); }
extern "C" void DMA1_Channel7_IRQHandler(void) { embed::platform_dma_irq_handler(6u); }
