// platforms/stm32f4g4/stm32_periph_m4.hpp
// ---------------------------------------------------------------------------
// Phase 3: GPIO/SPI/PWM для F407/G431 (Cortex-M4F).
// Пока polling-режим; IRQ/DMA — следующий шаг.
// ---------------------------------------------------------------------------
#pragma once

#include "stm32m4_min.h"
#include "hal/IGpio.hpp"
#include "hal/ISpi.hpp"
#include "hal/IPwm.hpp"
#include "hal/IUart.hpp"
#include "exti_m4.hpp"
#include "os/config/time_config.hpp"

namespace embed::stm32 {

inline void gpio_clock_enable(GPIO_TypeDef* port) noexcept {
    if (port == GPIOA) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if (port == GPIOB) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    else if (port == GPIOC) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
}

class GpioPin final : public hal::IGpioPin {
public:
    GpioPin(GPIO_TypeDef* port, uint8_t pin) noexcept : port_(port), pin_(pin) {}

    Status configure(const hal::GpioConfig& cfg) noexcept override {
        if (port_ == nullptr || pin_ > 15u) return Status::InvalidArg;
        gpio_clock_enable(port_);

        const uint32_t shift = pin_ * 2u;
        uint32_t mode = 0u;
        switch (cfg.mode) {
            case hal::GpioMode::Input:     mode = 0u; break;
            case hal::GpioMode::Output:    mode = 1u; break;
            case hal::GpioMode::Alternate: mode = 2u; break;
            case hal::GpioMode::Analog:    mode = 3u; break;
        }
        port_->MODER = (port_->MODER & ~(3u << shift)) | (mode << shift);

        uint32_t pupd = 0u;
        if (cfg.pull == hal::GpioPull::Up) pupd = 1u;
        else if (cfg.pull == hal::GpioPull::Down) pupd = 2u;
        port_->PUPDR = (port_->PUPDR & ~(3u << shift)) | (pupd << shift);

        if (cfg.mode == hal::GpioMode::Output) {
            port_->OTYPER &= ~(1u << pin_);   // push-pull
            const uint32_t sp = (cfg.speed == hal::GpioSpeed::Low) ? 0u
                              : (cfg.speed == hal::GpioSpeed::Medium) ? 1u
                              : (cfg.speed == hal::GpioSpeed::High) ? 2u : 3u;
            port_->OSPEEDR = (port_->OSPEEDR & ~(3u << shift)) | (sp << shift);
            set(cfg.initial);
        } else if (cfg.mode == hal::GpioMode::Alternate) {
            port_->OTYPER &= ~(1u << pin_);
            port_->OSPEEDR |= (3u << shift);
            const uint32_t idx = pin_ / 8u;
            const uint32_t afs = (pin_ % 8u) * 4u;
            port_->AFR[idx] = (port_->AFR[idx] & ~(0xFu << afs))
                            | ((cfg.alternate & 0xFu) << afs);
        }
        return Status::Ok;
    }

    void set(bool level) noexcept override {
        if (level) port_->BSRR = (1u << pin_);
        else       port_->BSRR = (1u << (pin_ + 16u));
    }
    bool get() const noexcept override { return (port_->IDR & (1u << pin_)) != 0u; }
    void toggle() noexcept override { set(!get()); }

    Status attach_irq(hal::GpioEdge edge, hal::GpioIrqCallback cb,
                      void* ctx) noexcept override {
        return exti_attach(port_, pin_, edge, cb, ctx);
    }
    void detach_irq() noexcept override { exti_detach(pin_); }

private:
    GPIO_TypeDef* port_;
    uint8_t pin_;
};


class Uart1 final : public hal::IUart {
public:
    explicit Uart1(uint32_t pclk) noexcept : pclk_(pclk) {}

    Status configure(const hal::UartConfig& cfg) noexcept override {
        if (cfg.baud_rate == 0u) return Status::InvalidArg;
        RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

        hal::GpioConfig af{};
        af.mode = hal::GpioMode::Alternate;
        af.alternate = 7u;
        GpioPin tx{GPIOA, 9u};
        GpioPin rx{GPIOA, 10u};
        if (tx.configure(af) != Status::Ok) return Status::HardwareError;
        if (rx.configure(af) != Status::Ok) return Status::HardwareError;

        uint32_t cr1 = (1u << 13) | (1u << 3) | (1u << 2); // UE, TE, RE
        if (cfg.parity != hal::UartParity::None) cr1 |= (1u << 10) | (1u << 12);
        if (cfg.parity == hal::UartParity::Odd) cr1 |= (1u << 9);
        USART1->CR1 = cr1;
        USART1->CR2 = (cfg.stop_bits == hal::UartStopBits::Two) ? (2u << 12) : 0u;
        USART1->BRR = (pclk_ + cfg.baud_rate / 2u) / cfg.baud_rate;
        cfg_ = cfg;
        return Status::Ok;
    }
    Status open() noexcept override { USART1->CR1 |= (1u << 13); return Status::Ok; }
    Status close() noexcept override { USART1->CR1 &= ~(1u << 13); return Status::Ok; }

    Status read(void* buffer, Size capacity, Size& bytes_read, Timeout) noexcept override {
        bytes_read = 0u;
        if (buffer == nullptr || capacity == 0u) return Status::InvalidArg;
        uint32_t spin = 0u;
        while ((USART1->SR & (1u << 5)) == 0u) {
            if (++spin > 1000000u) return Status::Timeout;
        }
        *static_cast<uint8_t*>(buffer) = static_cast<uint8_t>(USART1->DR);
        bytes_read = 1u;
        return Status::Ok;
    }

    Status write(const void* data, Size len, Size& written, Timeout) noexcept override {
        written = 0u;
        if (data == nullptr || len == 0u) return Status::InvalidArg;
        const auto* p = static_cast<const uint8_t*>(data);
        for (Size i = 0u; i < len; ++i) {
            uint32_t spin = 0u;
            while ((USART1->SR & (1u << 7)) == 0u) {
                if (++spin > 1000000u) return Status::Timeout;
            }
            USART1->DR = p[i];
            ++written;
        }
        uint32_t spin = 0u;
        while ((USART1->SR & (1u << 6)) == 0u && spin < 1000000u) ++spin;
        return Status::Ok;
    }

private:
    uint32_t pclk_;
    hal::UartConfig cfg_{};
};

class Spi final : public hal::ISpi {
public:
    Spi(SPI_TypeDef* spi, uint32_t pclk,
        GpioPin& sck, GpioPin& miso, GpioPin& mosi) noexcept
        : spi_(spi), pclk_(pclk), sck_(sck), miso_(miso), mosi_(mosi) {}

    Status configure(const hal::SpiConfig& cfg) noexcept override {
        if (spi_ == nullptr || cfg.mode > 3u) return Status::InvalidArg;
        RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;

        hal::GpioConfig af{};
        af.mode = hal::GpioMode::Alternate;
        af.alternate = 5u;
        if (sck_.configure(af) != Status::Ok) return Status::HardwareError;
        if (mosi_.configure(af) != Status::Ok) return Status::HardwareError;
        hal::GpioConfig in{};
        in.mode = hal::GpioMode::Input;
        if (miso_.configure(in) != Status::Ok) return Status::HardwareError;

        uint32_t cr1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI;
        if (!cfg.msb_first) cr1 |= SPI_CR1_LSBFIRST;
        if ((cfg.mode & 2u) != 0u) cr1 |= SPI_CR1_CPOL;
        if ((cfg.mode & 1u) != 0u) cr1 |= SPI_CR1_CPHA;
        cr1 |= (baud_div(cfg.hz) << 3u);
        spi_->CR1 = cr1 | SPI_CR1_SPE;
        cfg_ = cfg;
        idx_ = (spi_ == SPI1) ? 0u : 1u;
        instance_ref(idx_) = this;
        return Status::Ok;
    }

    Status transfer(const void* tx, void* rx, Size n, Timeout timeout) noexcept override {
        if ((tx == nullptr && rx == nullptr) || n == 0u) return Status::InvalidArg;
        if (cfg_.dma) {
            const Status st = transfer_dma(tx, rx, n, timeout);
            if (st != Status::NotSupported) return st;
        }
        if (cfg_.irq) return transfer_irq(tx, rx, n, timeout);
        const auto* txb = static_cast<const uint8_t*>(tx);
        auto* rxb = static_cast<uint8_t*>(rx);
        for (Size i = 0u; i < n; ++i) {
            uint32_t spin = 0u;
            while ((spi_->SR & SPI_SR_TXE) == 0u) {
                if (++spin > 1000000u) return Status::Timeout;
            }
            spi_->DR = (txb != nullptr) ? txb[i] : 0xFFu;
            spin = 0u;
            while ((spi_->SR & SPI_SR_RXNE) == 0u) {
                if (++spin > 1000000u) return Status::Timeout;
            }
            const uint8_t in = static_cast<uint8_t>(spi_->DR);
            if (rxb != nullptr) rxb[i] = in;
        }
        return Status::Ok;
    }

    Status lock(Timeout) noexcept override {
        Lock l;
        if (busy_) return Status::Busy;
        busy_ = true;
        return Status::Ok;
    }
    void unlock() noexcept override { Lock l; busy_ = false; }
    uint32_t caps() const noexcept override { return hal::SpiCapCs | hal::SpiCapIrq | hal::SpiCapDma; }

    static Spi* irq_instance(uint8_t idx) noexcept { return instance_ref(idx); }
    static void dispatch(uint8_t idx) noexcept {
        Spi* s = instance_ref(idx);
        if (s != nullptr) s->on_irq();
    }

private:
    static Spi*& instance_ref(uint8_t idx) noexcept {
        static Spi* p[2] = {nullptr, nullptr};
        return p[idx & 1u];
    }

    static void nvic_enable(uint32_t irq) noexcept {
        *reinterpret_cast<volatile uint32_t*>(0xE000E100u + (irq / 32u) * 4u) =
            (1u << (irq % 32u));
    }

    void on_irq() noexcept {
        const uint32_t sr = spi_->SR;
        if ((sr & SPI_SR_OVR) != 0u) {
            (void)spi_->DR;
            (void)spi_->SR;
            st_ = Status::HardwareError;
            done_ = true;
            spi_->CR2 &= ~(SPI_CR2_TXEIE | SPI_CR2_RXNEIE | SPI_CR2_ERRIE);
            return;
        }
        if ((sr & SPI_SR_TXE) != 0u && tx_i_ < n_) {
            spi_->DR = (txb_ != nullptr) ? txb_[tx_i_] : 0xFFu;
            ++tx_i_;
            if (tx_i_ >= n_) spi_->CR2 &= ~SPI_CR2_TXEIE;
        }
        if ((sr & SPI_SR_RXNE) != 0u) {
            const uint8_t in = static_cast<uint8_t>(spi_->DR);
            if (rxb_ != nullptr && rx_i_ < n_) rxb_[rx_i_] = in;
            ++rx_i_;
            if (rx_i_ >= n_) spi_->CR2 &= ~SPI_CR2_RXNEIE;
        }
        if (tx_i_ >= n_ && rx_i_ >= n_) {
            done_ = true;
            spi_->CR2 &= ~(SPI_CR2_TXEIE | SPI_CR2_RXNEIE | SPI_CR2_ERRIE);
        }
    }

    Status transfer_dma(const void* tx, void* rx, Size n, Timeout timeout) noexcept {
        if (spi_ != SPI1) return Status::NotSupported;
        if (tx == nullptr || rx == nullptr || n == 0u) return Status::NotSupported;
#if defined(MCFRAME_SOC_STM32G4)
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
        DMA1->IFCR = 0xFFu;
        DMAMUX1->CCMR[0] = 10u;   // SPI1_RX
        DMAMUX1->CCMR[1] = 11u;   // SPI1_TX
        DMA1_Channel1->CCR = 0u;
        DMA1_Channel1->CPAR = reinterpret_cast<uint32_t>(&SPI1->DR);
        DMA1_Channel1->CMAR = reinterpret_cast<uint32_t>(const_cast<void*>(rx));
        DMA1_Channel1->CNDTR = n;
        DMA1_Channel1->CCR = (1u << 10);       // MINC, peripheral->memory
        DMA1_Channel2->CCR = 0u;
        DMA1_Channel2->CPAR = reinterpret_cast<uint32_t>(&SPI1->DR);
        DMA1_Channel2->CMAR = reinterpret_cast<uint32_t>(const_cast<void*>(tx));
        DMA1_Channel2->CNDTR = n;
        DMA1_Channel2->CCR = (1u << 10) | (1u << 4); // MINC | DIR
        SPI1->CR2 |= SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
        DMA1_Channel1->CCR |= 1u;
        DMA1_Channel2->CCR |= 1u;

        const uint64_t deadline = (timeout != 0u)
            ? (config::OsTimer::NowMs() + timeout) : 0u;
        while (DMA1_Channel1->CNDTR != 0u || DMA1_Channel2->CNDTR != 0u) {
            if (deadline != 0u && config::OsTimer::NowMs() >= deadline) {
                DMA1_Channel1->CCR &= ~1u;
                DMA1_Channel2->CCR &= ~1u;
                SPI1->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
                return Status::Timeout;
            }
        }
        DMA1_Channel1->CCR &= ~1u;
        DMA1_Channel2->CCR &= ~1u;
        DMA1->IFCR = 0xFFu;
#else
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
        DMA2->LIFCR = (0x3Fu) | (0x3Fu << 22);   // stream0, stream3
        DMA2_Stream0->CR = 0u;
        DMA2_Stream0->PAR = reinterpret_cast<uint32_t>(&SPI1->DR);
        DMA2_Stream0->M0AR = reinterpret_cast<uint32_t>(const_cast<void*>(rx));
        DMA2_Stream0->NDTR = n;
        DMA2_Stream0->FCR = 0u;
        DMA2_Stream0->CR = (3u << 25) | (1u << 10) | 1u; // CHSEL3, MINC, EN

        DMA2_Stream3->CR = 0u;
        DMA2_Stream3->PAR = reinterpret_cast<uint32_t>(&SPI1->DR);
        DMA2_Stream3->M0AR = reinterpret_cast<uint32_t>(const_cast<void*>(tx));
        DMA2_Stream3->NDTR = n;
        DMA2_Stream3->FCR = 0u;
        DMA2_Stream3->CR = (3u << 25) | (1u << 10) | (1u << 4) | 1u; // +DIR
        SPI1->CR2 |= SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;

        const uint64_t deadline = (timeout != 0u)
            ? (config::OsTimer::NowMs() + timeout) : 0u;
        while (DMA2_Stream0->NDTR != 0u || DMA2_Stream3->NDTR != 0u) {
            if (deadline != 0u && config::OsTimer::NowMs() >= deadline) {
                DMA2_Stream0->CR &= ~1u;
                DMA2_Stream3->CR &= ~1u;
                SPI1->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
                return Status::Timeout;
            }
        }
        DMA2_Stream0->CR &= ~1u;
        DMA2_Stream3->CR &= ~1u;
        DMA2->LIFCR = (0x3Fu) | (0x3Fu << 22);
#endif
        SPI1->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
        return Status::Ok;
    }

    Status transfer_irq(const void* tx, void* rx, Size n, Timeout timeout) noexcept {
        txb_ = static_cast<const uint8_t*>(tx);
        rxb_ = static_cast<uint8_t*>(rx);
        n_ = n;
        tx_i_ = 0u;
        rx_i_ = 0u;
        st_ = Status::Ok;
        done_ = false;
        spi_->CR2 |= SPI_CR2_TXEIE | SPI_CR2_RXNEIE | SPI_CR2_ERRIE;
        nvic_enable(35u + idx_);
        const uint64_t deadline = (timeout != 0u)
            ? (config::OsTimer::NowMs() + timeout) : 0u;
        while (!done_) {
            if (deadline != 0u && config::OsTimer::NowMs() >= deadline) {
                spi_->CR2 &= ~(SPI_CR2_TXEIE | SPI_CR2_RXNEIE | SPI_CR2_ERRIE);
                return Status::Timeout;
            }
        }
        return st_;
    }

    uint32_t baud_div(uint32_t hz) const noexcept {
        if (hz == 0u) hz = 1u;
        uint32_t div = 0u;
        uint32_t clock = pclk_ / 2u;
        while (div < 7u && clock > hz) { clock /= 2u; ++div; }
        return div;
    }

    SPI_TypeDef* spi_;
    uint32_t pclk_;
    GpioPin& sck_;
    GpioPin& miso_;
    GpioPin& mosi_;
    bool busy_ = false;
    hal::SpiConfig cfg_{};
    uint8_t idx_ = 0u;
    const uint8_t* txb_ = nullptr;
    uint8_t* rxb_ = nullptr;
    Size n_ = 0u;
    volatile Size tx_i_ = 0u;
    volatile Size rx_i_ = 0u;
    volatile bool done_ = false;
    Status st_ = Status::Ok;
};

class Pwm final : public hal::IPwm {
public:
    Pwm(TIM_TypeDef* tim, uint8_t channel, GpioPin& pin, uint32_t pclk) noexcept
        : tim_(tim), channel_(channel), pin_(pin), pclk_(pclk) {}

    Status configure(const hal::PwmConfig& cfg) noexcept override {
        if (tim_ == nullptr || channel_ < 1u || channel_ > 4u || cfg.frequency_hz == 0u) {
            return Status::InvalidArg;
        }
        RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

        hal::GpioConfig af{};
        af.mode = hal::GpioMode::Alternate;
        af.alternate = 1u;
        if (pin_.configure(af) != Status::Ok) return Status::HardwareError;

        const uint32_t tick_hz = 1000000u;
        const uint32_t psc = (pclk_ / tick_hz == 0u) ? 0u : (pclk_ / tick_hz - 1u);
        uint32_t arr = tick_hz / cfg.frequency_hz;
        if (arr == 0u) arr = 1u;
        if (arr > 65536u) arr = 65536u;
        tim_->PSC = psc;
        tim_->ARR = arr - 1u;
        ccr(channel_) = static_cast<uint32_t>((arr * cfg.duty_percent) / 100u);

        const uint32_t shift = (channel_ - 1u) * 4u;
        if (channel_ <= 2u) {
            tim_->CCMR1 = (tim_->CCMR1 & ~(0x7u << (shift + 4u)))
                        | (0x6u << (shift + 4u)) | (1u << (shift + 3u));
        } else {
            const uint32_t s2 = (channel_ - 3u) * 4u;
            tim_->CCMR2 = (tim_->CCMR2 & ~(0x7u << (s2 + 4u)))
                        | (0x6u << (s2 + 4u)) | (1u << (s2 + 3u));
        }
        tim_->CCER |= (1u << ((channel_ - 1u) * 4u));
        tim_->CR1 |= TIM_CR1_ARPE;
        return Status::Ok;
    }

    Status set_duty(uint8_t duty) noexcept override {
        if (duty > 100u) return Status::InvalidArg;
        const uint32_t arr = tim_->ARR + 1u;
        ccr(channel_) = (arr * duty) / 100u;
        return Status::Ok;
    }
    Status start() noexcept override { tim_->CR1 |= TIM_CR1_CEN; return Status::Ok; }
    Status stop() noexcept override { tim_->CR1 &= ~TIM_CR1_CEN; return Status::Ok; }
    uint32_t caps() const noexcept override { return 1u; }

private:
    volatile uint32_t& ccr(uint8_t ch) noexcept {
        switch (ch) {
            case 1u: return tim_->CCR1;
            case 2u: return tim_->CCR2;
            case 3u: return tim_->CCR3;
            default: return tim_->CCR4;
        }
    }

    TIM_TypeDef* tim_;
    uint8_t channel_;
    GpioPin& pin_;
    uint32_t pclk_;
};

} // namespace embed::stm32
