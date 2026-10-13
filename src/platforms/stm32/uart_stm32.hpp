// platforms/stm32/uart_stm32.hpp
#pragma once
// ---------------------------------------------------------------------------
// Драйвер USART для STM32F103 с поддержкой трёх стратегий.
//
//   Polling — драйвер опрашивает SR в pollOnIrq() (вызывается AsyncPort
//             в цикле ожидания);
//   Async   — прерывания RXNE/TXE/TC: USART1_IRQHandler -> onRxIrq/onTxIrq;
//   Dma     — DMA1 Channel4 (TX) / Channel5 (RX) для USART1;
//             завершение приходит из обработчика DMA.
//
// Драйвер НЕ зависит от прикладного кода: он лишь завершает объект
// завершения (async::completion), который ему выдал порт.
// ---------------------------------------------------------------------------
#include "../../os/async/async_port.hpp"   // async::Strategy
#include "../../os/interfaces/iuart.hpp"
#include "../../os/interfaces/dma_manager.hpp"
#include "../../os/config/dma_config.hpp"

#include "stm32f10x.h"
#include "core_cm3.h"

namespace embed::stm32 {

/// Номера каналов DMA для каждого USART (по RM0008).
struct UartDmaMap {
    config::DmaRequest tx;
    config::DmaRequest rx;
};

// ─────────────────────────────── USART1 ─────────────────────────────────────
//
// Драйверы USART2/USART3 делаются по этому же образцу: отличаются базой
// регистров, номером IRQ и каналами DMA (см. config/dma_config.hpp).
class UartDriver1 : public IUart<UartDriver1> {
public:
    static constexpr bool           kHasDma   = true;
    static constexpr async::Strategy kStrategy = async::Strategy::Dma;
    static constexpr Size           kMaxChunk = 0u;   // без ограничения блока

    using Base    = IUart<UartDriver1>;
    using Config  = typename Base::Config;
    using Parity  = typename Base::Parity;
    using StopBits = typename Base::StopBits;

    static UartDriver1& instance() noexcept {
        static UartDriver1 d;
        return d;
    }

    // ─────────────────────────── Настройка ───────────────────────────

    Status configure(const Config& cfg) noexcept {
        cfg_ = cfg;

        RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

        // 8 бит данных, 1 стоп-бит, без чётности — базовый режим.
        uint32_t cr1 = USART_CR1_TE | USART_CR1_RE;
        switch (cfg.parity) {
            case Parity::Even: cr1 |= USART_CR1_PCE; break;
            case Parity::Odd:  cr1 |= USART_CR1_PCE | USART_CR1_PS; break;
            case Parity::None: break;
        }
        USART1->CR1 = cr1;
        USART1->CR2 = (cfg.stopBits == StopBits::Two) ? USART_CR2_STOP_1 : 0u;
        // Аппаратное управление потоком живёт в CR3, а не в CR1.
        USART1->CR3 = cfg.flowControl
                    ? (USART_CR3_RTSE | USART_CR3_CTSE)
                    : 0u;

        setBaudRate(cfg.baudRate);

        USART1->CR1 |= USART_CR1_UE;      // включить USART
        return Status::Ok;
    }

    Status open() noexcept {
        USART1->CR1 |= USART_CR1_UE;
        return Status::Ok;
    }

    Status close() noexcept {
        USART1->CR1 &= ~USART_CR1_UE;
        return Status::Ok;
    }

    Status ioctl(unsigned request, void* arg) noexcept {
        (void)request;
        (void)arg;
        return Status::NotSupported;
    }

    /// Смена скорости на лету (пересчёт BRR).
    Status setBaudRate(uint32_t baud) noexcept {
        if (baud == 0u) return Status::InvalidArg;
        // USART1 висит на APB2 (72 МГц при SYSCLK 72 МГц).
        const uint32_t pclk = apb2_clock_hz();
        USART1->BRR = (pclk + baud / 2u) / baud;
        return Status::Ok;
    }

    /// Включить прерывания вместо DMA.
    void useInterrupts(bool enable) noexcept {
        use_dma_ = !enable;
        if (enable) {
            USART1->CR1 |= USART_CR1_RXNEIE;
            nvic_enable(static_cast<uint32_t>(USART1_IRQn));
        } else {
            USART1->CR1 &= ~USART_CR1_RXNEIE;
            nvic_disable(static_cast<uint32_t>(USART1_IRQn));
        }
    }

    // ─────────────────────── Асинхронные операции ───────────────────────

    /// Привязать объект завершения (владелец — порт, см. async_port.hpp).
    Status setCompletion(async::completion& token) noexcept {
        pending_ = &token;
        return Status::Ok;
    }

    async::completion* pendingCompletion() noexcept { return pending_; }

    Status startWrite(const void* data, Size len) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (data == nullptr || len == 0u) return Status::InvalidArg;
        if (tx_busy_) return Status::Busy;

        tx_buf_ = static_cast<const uint8_t*>(data);
        tx_len_ = len;
        tx_pos_ = 0u;
        tx_busy_ = true;

        if (use_dma_ && startTxDma(len) == Status::Ok) return Status::Ok;

        // Программный режим: первый байт пишем сразу, остальные — по TXE.
        USART1->CR1 |= USART_CR1_TXEIE;
        USART1->DR = tx_buf_[0];
        ++tx_pos_;
        return Status::Ok;
    }

    Status startRead(void* buffer, Size capacity) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (buffer == nullptr || capacity == 0u) return Status::InvalidArg;
        if (rx_busy_) return Status::Busy;

        rx_buf_ = static_cast<uint8_t*>(buffer);
        rx_cap_ = capacity;
        rx_pos_ = 0u;
        rx_busy_ = true;

        if (use_dma_ && startRxDma(capacity) == Status::Ok) return Status::Ok;

        // Программный режим: каждый принятый байт — по RXNE.
        USART1->CR1 |= USART_CR1_RXNEIE;
        return Status::Ok;
    }

    Status abort() noexcept {
        USART1->CR1 &= ~(USART_CR1_TXEIE | USART_CR1_RXNEIE);
        stopDma();
        tx_busy_ = false;
        rx_busy_ = false;
        return Status::Ok;
    }

    // ─────────────────── Точки входа прерываний/опроса ───────────────────

    /// Обработчик USART1_IRQHandler. Также используется как опрос
    /// для стратегии Polling.
    void handleIrq() noexcept {
        const uint32_t sr = USART1->SR;

        // Ошибки: переполнение приёмника, кадр, шум.
        if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0u) {
            (void)USART1->DR;                       // сбрасываем ORE чтением
            if (pending_ != nullptr) {
                (void)pending_->post_error(Status::HardwareError);
                pending_ = nullptr;
            }
            rx_busy_ = false;
            return;
        }

        if ((sr & USART_SR_RXNE) != 0u) {
            const uint8_t b = static_cast<uint8_t>(USART1->DR & 0xFFu);
            if (rx_busy_ && rx_pos_ < rx_cap_) {
                rx_buf_[rx_pos_++] = b;
                if (rx_pos_ >= rx_cap_) {
                    finishRx(rx_pos_);
                }
            }
        }

        if ((sr & USART_SR_TXE) != 0u) {
            if (tx_busy_) {
                if (tx_pos_ < tx_len_) {
                    USART1->DR = tx_buf_[tx_pos_++];
                } else {
                    USART1->CR1 &= ~USART_CR1_TXEIE;   // ждём TC
                    USART1->CR1 |= USART_CR1_TCIE;
                }
            }
        }

        if ((sr & USART_SR_TC) != 0u) {
            USART1->CR1 &= ~USART_CR1_TCIE;
            if (tx_busy_) finishTx(tx_len_);
        }
    }

    void onRxIrq() noexcept { handleIrq(); }
    void onTxIrq() noexcept { handleIrq(); }
    void onErrorIrq(Status st) noexcept {
        if (pending_ != nullptr) {
            (void)pending_->post_error(st);
            pending_ = nullptr;
        }
    }
    void pollOnIrq() noexcept { handleIrq(); }

    /// Вызывается из обработчика DMA (см. dma_tx_done/dma_rx_done).
    void onTxDmaComplete(Size n) noexcept { finishTx(n); }
    void onRxDmaComplete(Size n) noexcept { finishRx(n); }

private:
    static void finishTx(Size n) noexcept {
        UartDriver1& d = instance();
        d.tx_busy_ = false;
        if (d.pending_ != nullptr) {
            (void)d.pending_->post_ok(n);
            d.pending_ = nullptr;
        }
    }

    static void finishRx(Size n) noexcept {
        UartDriver1& d = instance();
        USART1->CR1 &= ~USART_CR1_RXNEIE;
        d.rx_busy_ = false;
        if (d.pending_ != nullptr) {
            (void)d.pending_->post_ok(n);
            d.pending_ = nullptr;
        }
    }

    Status startTxDma(Size len) noexcept {
        DmaChannel ch;
        DmaManager& mgr = DmaManager::instance();
        if (!mgr.is_initialized() && mgr.init() != Status::Ok) return Status::Fault;

        if (mgr.acquire(config::DmaRequest::Usart1Tx, ch) != Status::Ok) return Status::Busy;

        const dma::BufferConfig cfg{
            const_cast<uint8_t*>(tx_buf_),
            len,
            true,
        };
        Status st = ch.configureMemToPeriph(config::DmaRequest::Usart1Tx, cfg,
                                           dma::TransferSize::Byte,
                                           dma::Priority::Medium,
                                           dma::Trigger::Hardware,
                                           dma::BurstMode::Single);
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        ch.setCallback(&dmaTxCallback, this);
        USART1->CR3 |= USART_CR3_DMAT;
        st = ch.start();
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        tx_dma_ = ch;
        return Status::Ok;
    }

    Status startRxDma(Size len) noexcept {
        DmaChannel ch;
        DmaManager& mgr = DmaManager::instance();
        if (!mgr.is_initialized() && mgr.init() != Status::Ok) return Status::Fault;

        if (mgr.acquire(config::DmaRequest::Usart1Rx, ch) != Status::Ok) return Status::Busy;

        const dma::BufferConfig cfg{ rx_buf_, len, true };
        Status st = ch.configurePeriphToMem(config::DmaRequest::Usart1Rx, cfg,
                                           dma::TransferSize::Byte,
                                           dma::Priority::Medium,
                                           dma::Trigger::Hardware,
                                           dma::BurstMode::Single);
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        ch.setCallback(&dmaRxCallback, this);
        USART1->CR3 |= USART_CR3_DMAR;
        st = ch.start();
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        rx_dma_ = ch;
        return Status::Ok;
    }

    void stopDma() noexcept {
        DmaManager& mgr = DmaManager::instance();
        if (tx_dma_.valid()) { (void)mgr.release(tx_dma_); tx_dma_ = DmaChannel{}; }
        if (rx_dma_.valid()) { (void)mgr.release(rx_dma_); rx_dma_ = DmaChannel{}; }
        USART1->CR3 &= ~(USART_CR3_DMAT | USART_CR3_DMAR);
    }

    /// Колбэк DMA: структура контекста описана в dma_manager.cpp.
    static void dmaTxCallback(void* ctx) noexcept {
        if (ctx == nullptr) return;
        // Включаем прерывание по завершению сдвига последнего байта:
        // DMA закончился, но байт ещё может оставаться в сдвиговом регистре,
        // поэтому завершать операцию рано.
        USART1->CR1 |= USART_CR1_TCIE;
    }

    static void dmaRxCallback(void* ctx) noexcept {
        auto* self = static_cast<UartDriver1*>(ctx);
        if (self == nullptr) return;
        const Size got = self->rx_cap_ - platform_dma_bytes_remaining(self->rx_dma_.id());
        self->stopDma();
        finishRx(got);
    }

    static uint32_t apb2_clock_hz() noexcept { return 72000000u; }

    static void nvic_enable(uint32_t irqn) noexcept {
        NVIC->ISER[irqn >> 5u] = (1UL << (irqn & 0x1Fu));
    }
    static void nvic_disable(uint32_t irqn) noexcept {
        NVIC->ICER[irqn >> 5u] = (1UL << (irqn & 0x1Fu));
    }

    Config cfg_{};
    async::completion* pending_ = nullptr;

    const uint8_t* tx_buf_ = nullptr;
    Size tx_len_ = 0u;
    Size tx_pos_ = 0u;
    bool tx_busy_ = false;

    uint8_t* rx_buf_ = nullptr;
    Size rx_cap_ = 0u;
    Size rx_pos_ = 0u;
    bool rx_busy_ = false;

    DmaChannel tx_dma_{};
    DmaChannel rx_dma_{};
    bool use_dma_ = true;
};

} // namespace embed::stm32
