// platforms/stm32/stm32_periph.hpp
// ---------------------------------------------------------------------------
// Phase 3: GPIO/SPI/PWM для STM32F1. Регистры берутся через stm32_regs.hpp,
// поэтому те же классы можно собрать с host-моделью (MCFRAME_STM32_REGS_FAKE).
// ---------------------------------------------------------------------------
#pragma once

#include "stm32_regs.hpp"
#include "hal/IGpio.hpp"
#include "hal/ISpi.hpp"
#include "hal/IPwm.hpp"
#include "exti_stm32.hpp"
#include "os/interfaces/dma_manager.hpp"
#include "os/config/dma_config.hpp"
#include "os/config/time_config.hpp"

#include <cstdint>

namespace embed::stm32 {

inline uint32_t gpio_rcc_bit(GPIO_TypeDef* port) noexcept {
    if (port == GPIOA) return RCC_APB2ENR_IOPAEN;
    if (port == GPIOB) return RCC_APB2ENR_IOPBEN;
    if (port == GPIOC) return RCC_APB2ENR_IOPCEN;
    if (port == GPIOD) return RCC_APB2ENR_IOPDEN;
    return 0u;
}

inline void gpio_clock_enable(GPIO_TypeDef* port) noexcept {
    const uint32_t bit = gpio_rcc_bit(port);
    if (bit != 0u) RCC->APB2ENR |= bit;
}

class GpioPin final : public hal::IGpioPin {
public:
    GpioPin(GPIO_TypeDef* port, uint8_t pin) noexcept : port_(port), pin_(pin) {}

    Status configure(const hal::GpioConfig& cfg) noexcept override {
        if (port_ == nullptr || pin_ > 15u) return Status::InvalidArg;
        gpio_clock_enable(port_);

        uint32_t mode = 0u;
        uint32_t cnf = 0u;
        switch (cfg.mode) {
            case hal::GpioMode::Input:
                mode = 0u;
                cnf = (cfg.pull == hal::GpioPull::None) ? 1u : 2u; // floating/pull
                break;
            case hal::GpioMode::Output:
                mode = (cfg.speed == hal::GpioSpeed::Low) ? 2u
                     : (cfg.speed == hal::GpioSpeed::Medium) ? 1u : 3u;
                cnf = 0u; // push-pull
                break;
            case hal::GpioMode::Alternate:
                mode = 3u; // 50 МГц
                cnf = 2u;  // AF push-pull
                break;
            case hal::GpioMode::Analog:
                mode = 0u;
                cnf = 0u;
                break;
        }

        const uint32_t shift = (pin_ < 8u) ? (pin_ * 4u) : ((pin_ - 8u) * 4u);
        volatile uint32_t& cr = (pin_ < 8u) ? port_->CRL : port_->CRH;
        cr = (cr & ~(0xFu << shift)) | ((cnf << 2u) | mode) << shift;

        if (cfg.mode == hal::GpioMode::Input) {
            if (cfg.pull == hal::GpioPull::Up) port_->BSRR = (1u << pin_);
            else if (cfg.pull == hal::GpioPull::Down) port_->BRR = (1u << pin_);
        } else if (cfg.mode == hal::GpioMode::Output) {
            set(cfg.initial);
        }
        return Status::Ok;
    }

    void set(bool level) noexcept override {
        if (level) port_->BSRR = (1u << pin_);
        else       port_->BRR  = (1u << pin_);
    }
    bool get() const noexcept override { return (port_->IDR & (1u << pin_)) != 0u; }
    void toggle() noexcept override { set(!get()); }

    Status attach_irq(hal::GpioEdge edge, hal::GpioIrqCallback cb,
                      void* ctx) noexcept override {
        return exti_attach(port_, pin_, edge, cb, ctx);
    }
    void detach_irq() noexcept override { exti_detach(pin_); }

    GPIO_TypeDef* port() const noexcept { return port_; }
    uint8_t pin() const noexcept { return pin_; }

private:
    GPIO_TypeDef* port_;
    uint8_t       pin_;
};

class Spi final : public hal::ISpi {
public:
    Spi(SPI_TypeDef* spi, uint32_t pclk,
        GpioPin& sck, GpioPin& miso, GpioPin& mosi) noexcept
        : spi_(spi), pclk_(pclk), sck_(sck), miso_(miso), mosi_(mosi) {}

    Status configure(const hal::SpiConfig& cfg) noexcept override {
        if (spi_ == nullptr || cfg.mode > 3u) return Status::InvalidArg;
        if (spi_ == SPI1) RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
        else if (spi_ == SPI2) RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;

        hal::GpioConfig af{};
        af.mode = hal::GpioMode::Alternate;
        if (sck_.configure(af) != Status::Ok) return Status::HardwareError;
        if (mosi_.configure(af) != Status::Ok) return Status::HardwareError;
        hal::GpioConfig in{};
        in.mode = hal::GpioMode::Input;
        in.pull = hal::GpioPull::None;
        if (miso_.configure(in) != Status::Ok) return Status::HardwareError;

        spi_->CR1 = 0u;
        spi_->CR2 = 0u;
        uint32_t cr1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI;
        if (!cfg.msb_first) cr1 |= SPI_CR1_LSBFIRST;
        if ((cfg.mode & 2u) != 0u) cr1 |= SPI_CR1_CPOL;
        if ((cfg.mode & 1u) != 0u) cr1 |= SPI_CR1_CPHA;

        const uint32_t div = baud_div(cfg.hz);
        cr1 |= ((div & 7u) << 3u);
        spi_->CR1 = cr1 | SPI_CR1_SPE;
        cfg_ = cfg;
        idx_ = (spi_ == SPI1) ? 0u : 1u;
        instance_ref(idx_) = this;
        return Status::Ok;
    }

    Status transfer(const void* tx, void* rx, Size n, Timeout timeout) noexcept override {
        if ((tx == nullptr && rx == nullptr) || n == 0u) return Status::InvalidArg;
        if (cfg_.irq) return transfer_irq(tx, rx, n, timeout);
        if (cfg_.dma && tx != nullptr && rx != nullptr) {
            const Status st = transfer_dma(tx, rx, n, timeout);
            if (st != Status::NotSupported) return st;
        }
        const auto* txb = static_cast<const uint8_t*>(tx);
        auto* rxb = static_cast<uint8_t*>(rx);
        for (Size i = 0u; i < n; ++i) {
            const uint8_t out = (txb != nullptr) ? txb[i] : 0xFFu;
            uint32_t spin = 0u;
            while ((spi_->SR & SPI_SR_TXE) == 0u) {
                if (++spin > 1000000u) return Status::Timeout;
            }
            *reinterpret_cast<volatile uint8_t*>(&spi_->DR) = out;
            spin = 0u;
            while ((spi_->SR & SPI_SR_RXNE) == 0u) {
                if (++spin > 1000000u) return Status::Timeout;
            }
            const uint8_t in = *reinterpret_cast<volatile uint8_t*>(&spi_->DR);
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
    uint32_t caps() const noexcept override {
        return hal::SpiCapCs | hal::SpiCapDma | hal::SpiCapIrq;
    }

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

    void on_irq() noexcept {
        if (spi_ == nullptr) return;
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
            const uint8_t out = (txb_ != nullptr) ? txb_[tx_i_] : 0xFFu;
            *reinterpret_cast<volatile uint8_t*>(&spi_->DR) = out;
            ++tx_i_;
            if (tx_i_ >= n_) spi_->CR2 &= ~SPI_CR2_TXEIE;
        }
        if ((sr & SPI_SR_RXNE) != 0u) {
            const uint8_t in = *reinterpret_cast<volatile uint8_t*>(&spi_->DR);
            if (rxb_ != nullptr && rx_i_ < n_) rxb_[rx_i_] = in;
            ++rx_i_;
            if (rx_i_ >= n_) spi_->CR2 &= ~SPI_CR2_RXNEIE;
        }
        if (tx_i_ >= n_ && rx_i_ >= n_) {
            done_ = true;
            spi_->CR2 &= ~(SPI_CR2_TXEIE | SPI_CR2_RXNEIE | SPI_CR2_ERRIE);
        }
    }

    Status transfer_irq(const void* tx, void* rx, Size n, Timeout timeout) noexcept {
        if (spi_ == nullptr) return Status::Unavailable;
        txb_ = static_cast<const uint8_t*>(tx);
        rxb_ = static_cast<uint8_t*>(rx);
        n_ = n;
        tx_i_ = 0u;
        rx_i_ = 0u;
        st_ = Status::Ok;
        done_ = false;
        spi_->CR2 |= SPI_CR2_TXEIE | SPI_CR2_RXNEIE | SPI_CR2_ERRIE;
        if (idx_ == 0u) NVIC_EnableIRQ(SPI1_IRQn);
        else            NVIC_EnableIRQ(SPI2_IRQn);

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

    Status transfer_dma(const void* tx, void* rx, Size n, Timeout timeout) noexcept {
        if (spi_ != SPI1) return Status::NotSupported;
        DmaManager& dm = DmaManager::instance();
        if (!dm.is_initialized() && dm.init() != Status::Ok) return Status::Unavailable;
        DmaChannel tx_ch;
        DmaChannel rx_ch;
        if (dm.acquire(config::DmaRequest::Spi1Tx, tx_ch, true) != Status::Ok) {
            return Status::Unavailable;
        }
        if (dm.acquire(config::DmaRequest::Spi1Rx, rx_ch, false) != Status::Ok) {
            (void)dm.release(tx_ch);
            return Status::Unavailable;
        }

        dma::BufferConfig src{const_cast<void*>(tx), n, true};
        dma::BufferConfig dst{rx, n, true};
        if (tx_ch.configureMemToPeriph(config::DmaRequest::Spi1Tx, src) != Status::Ok ||
            rx_ch.configurePeriphToMem(config::DmaRequest::Spi1Rx, dst) != Status::Ok) {
            (void)dm.release(rx_ch);
            (void)dm.release(tx_ch);
            return Status::HardwareError;
        }
        spi_->CR2 |= SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN;
        if (tx_ch.start() != Status::Ok || rx_ch.start() != Status::Ok) {
            spi_->CR2 &= ~(SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN);
            (void)dm.release(rx_ch);
            (void)dm.release(tx_ch);
            return Status::HardwareError;
        }

        const uint64_t deadline = (timeout != 0u)
            ? (config::OsTimer::NowMs() + timeout) : 0u;
        while (!tx_ch.isComplete() || !rx_ch.isComplete()) {
            if (deadline != 0u && config::OsTimer::NowMs() >= deadline) {
                (void)tx_ch.stop();
                (void)rx_ch.stop();
                spi_->CR2 &= ~(SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN);
                (void)dm.release(rx_ch);
                (void)dm.release(tx_ch);
                return Status::Timeout;
            }
        }
        while ((spi_->SR & SPI_SR_TXE) == 0u) {}
        while ((spi_->SR & SPI_SR_BSY) != 0u) {}
        spi_->CR2 &= ~(SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN);
        (void)dm.release(rx_ch);
        (void)dm.release(tx_ch);
        return Status::Ok;
    }

    uint32_t baud_div(uint32_t hz) const noexcept {
        if (hz == 0u) hz = 1u;
        uint32_t div = 0u;
        uint32_t clock = pclk_ / 2u;
        while (div < 7u && clock > hz) {
            clock /= 2u;
            ++div;
        }
        return div;
    }

    SPI_TypeDef* spi_;
    uint32_t     pclk_;
    GpioPin&     sck_;
    GpioPin&     miso_;
    GpioPin&     mosi_;
    hal::SpiConfig cfg_{};
    bool busy_ = false;
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
        if (tim_ == TIM2) RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
        else if (tim_ == TIM3) RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
        else if (tim_ == TIM4) RCC->APB1ENR |= RCC_APB1ENR_TIM4EN;

        hal::GpioConfig af{};
        af.mode = hal::GpioMode::Alternate;
        if (pin_.configure(af) != Status::Ok) return Status::HardwareError;

        const uint32_t tick_hz = 1000000u;
        const uint32_t psc = (pclk_ / tick_hz == 0u) ? 0u : (pclk_ / tick_hz - 1u);
        uint32_t arr = tick_hz / cfg.frequency_hz;
        if (arr == 0u) arr = 1u;
        if (arr > 65536u) arr = 65536u;

        tim_->PSC = psc;
        tim_->ARR = arr - 1u;
        ccr(channel_) = static_cast<uint16_t>((arr * cfg.duty_percent) / 100u);

        const uint32_t shift = (channel_ - 1u) * 4u;
        if (channel_ <= 2u) {
            tim_->CCMR1 = (tim_->CCMR1 & ~(0x7u << (shift + 4u)))
                        | (0x6u << (shift + 4u))          // PWM mode 1
                        | (1u << (shift + 3u));           // preload
        } else {
            const uint32_t s2 = (channel_ - 3u) * 4u;
            tim_->CCMR2 = (tim_->CCMR2 & ~(0x7u << (s2 + 4u)))
                        | (0x6u << (s2 + 4u))
                        | (1u << (s2 + 3u));
        }
        tim_->CCER |= (1u << ((channel_ - 1u) * 4u));
        tim_->CR1 |= TIM_CR1_ARPE;
        cfg_ = cfg;
        return Status::Ok;
    }

    Status set_duty(uint8_t duty) noexcept override {
        if (duty > 100u) return Status::InvalidArg;
        const uint32_t arr = tim_->ARR + 1u;
        ccr(channel_) = static_cast<uint16_t>((arr * duty) / 100u);
        cfg_.duty_percent = duty;
        return Status::Ok;
    }
    Status start() noexcept override { tim_->CR1 |= TIM_CR1_CEN; return Status::Ok; }
    Status stop() noexcept override { tim_->CR1 &= ~TIM_CR1_CEN; return Status::Ok; }
    uint32_t caps() const noexcept override { return 1u; }

private:
    volatile uint16_t& ccr(uint8_t ch) noexcept {
        switch (ch) {
            case 1u: return tim_->CCR1;
            case 2u: return tim_->CCR2;
            case 3u: return tim_->CCR3;
            default: return tim_->CCR4;
        }
    }

    TIM_TypeDef* tim_;
    uint8_t      channel_;
    GpioPin&     pin_;
    uint32_t     pclk_;
    hal::PwmConfig cfg_{};
};

} // namespace embed::stm32
