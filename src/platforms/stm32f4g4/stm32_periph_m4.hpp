// platforms/stm32f4g4/stm32_periph_m4.hpp
// ---------------------------------------------------------------------------
// ПЕРИФЕРИЯ STM32F407 / STM32G431 ДЛЯ ПРИКЛАДНОГО СЛОЯ.
//
// Что здесь есть:
//   * GpioPin — MODER/OTYPER/OSPEEDR/PUPDR/AFR + EXTI;
//   * Uart<Instance> — USART1/2/3, настройка и блокирующие read/write с
//     таймаутом;
//   * Spi<Instance> — SPI1/2/3, polling + DMA через dma_map_m4.hpp;
//   * I2c<Instance> — I2C1/2, F4 (CR2/CCR/TRISE) и G4 (TIMINGR/ISR/ICR);
//   * Adc<Instance> — ADC1, одиночное преобразование;
//   * Pwm — TIM2_CH1.
//
// Все драйверы берут ПИНЫ ИЗ КАРТЫ ПЛАТЫ (boards/*/pins.hpp) и не знают
// номеров ног. Раньше «USART1 на PA9/PA10» было зашито в конструкторе, и
// вторая плата с другой разводкой требовала правки драйвера.
//
// Драйверы намеренно polling/blocking: прикладной контракт hal::IUart/
// hal::II2c/hal::IAdc не обещает асинхронности, а блокирующий обмен проще
// проверить и невозможно «потерять» завершение. DMA есть у SPI — там он
// реально нужен для длинных передач.
// ---------------------------------------------------------------------------
#pragma once

#include "stm32m4_min.h"
#include "dma_map_m4.hpp"

#include "boards/pin_map.hpp"

#include "hal/IGpio.hpp"
#include "hal/ISpi.hpp"
#include "hal/IPwm.hpp"
#include "hal/IUart.hpp"
#include "hal/II2c.hpp"
#include "hal/IAdc.hpp"

#include "exti_m4.hpp"
#include "os/config/time_config.hpp"

#include <cstdint>

namespace embed::stm32 {

// ─────────────────────────── Общие помощники ───────────────────────────

inline GPIO_TypeDef* gpio_port(board::Port p) noexcept {
    switch (p) {
        case board::Port::A: return GPIOA;
        case board::Port::B: return GPIOB;
        case board::Port::C: return GPIOC;
        case board::Port::D: return GPIOD;
        case board::Port::E: return GPIOE;
        default:             return nullptr;
    }
}

inline uint32_t gpio_rcc_bit(board::Port p) noexcept {
    switch (p) {
        case board::Port::A: return RCC_AHB1ENR_GPIOAEN;
        case board::Port::B: return RCC_AHB1ENR_GPIOBEN;
        case board::Port::C: return RCC_AHB1ENR_GPIOCEN;
        case board::Port::D: return RCC_AHB1ENR_GPIODEN;
        case board::Port::E: return RCC_AHB1ENR_GPIOEEN;
        default:             return 0u;
    }
}

inline void gpio_clock_enable(GPIO_TypeDef* port) noexcept {
    if (port == GPIOA) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if (port == GPIOB) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    else if (port == GPIOC) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    else if (port == GPIOD) RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    else if (port == GPIOE) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN;
}

inline void gpio_clock_enable(board::Port p) noexcept {
    RCC->AHB1ENR |= gpio_rcc_bit(p);
}

inline uint32_t now_ms() noexcept {
    // До старта системного тика NowMs() == 0; это не ошибка: ожидание
    // дополнительно ограничено счётчиком итераций.
    return static_cast<uint32_t>(config::OsTimer::NowMs());
}

inline uint64_t deadline_for(Timeout timeout) noexcept {
    if (timeout == 0u || timeout == WaitForever) return 0u;
    return static_cast<uint64_t>(now_ms()) + timeout;
}

/// Ограничитель ожидания: время + счётчик, чтобы драйвер не завис навсегда
/// ни при остановленном таймере, ни при неотвечающей периферии.
inline bool wait_expired(uint64_t deadline, uint32_t& spin) noexcept {
    ++spin;
    if (spin > 50000000u) return true;
    return deadline != 0u && static_cast<uint64_t>(now_ms()) >= deadline;
}

// ─────────────────────────────── GPIO ───────────────────────────────

class GpioPin final : public hal::IGpioPin {
public:
    GpioPin(GPIO_TypeDef* port, uint8_t pin) noexcept : port_(port), pin_(pin) {}
    explicit GpioPin(board::Pin pin) noexcept
        : port_(gpio_port(pin.port)), pin_(pin.number) {}

    Status configure(const hal::GpioConfig& cfg) noexcept override {
        if (port_ == nullptr || pin_ > 15u) return Status::InvalidArg;
        gpio_clock_enable(port_);

        const uint32_t shift = static_cast<uint32_t>(pin_) * 2u;
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
            port_->OTYPER = (port_->OTYPER & ~(1u << pin_)) |
                            (cfg.open_drain ? (1u << pin_) : 0u);
            const uint32_t sp = (cfg.speed == hal::GpioSpeed::Low) ? 0u
                              : (cfg.speed == hal::GpioSpeed::Medium) ? 1u
                              : (cfg.speed == hal::GpioSpeed::High) ? 2u : 3u;
            port_->OSPEEDR = (port_->OSPEEDR & ~(3u << shift)) | (sp << shift);
            set(cfg.initial);
        } else if (cfg.mode == hal::GpioMode::Alternate) {
            port_->OTYPER = (port_->OTYPER & ~(1u << pin_)) |
                            (cfg.open_drain ? (1u << pin_) : 0u);
            port_->OSPEEDR |= (3u << shift);
            const uint32_t idx = pin_ / 8u;
            const uint32_t afs = (pin_ % 8u) * 4u;
            port_->AFR[idx] = (port_->AFR[idx] & ~(0xFu << afs))
                            | ((cfg.alternate & 0xFu) << afs);
        }
        return Status::Ok;
    }

    void set(bool level) noexcept override {
        if (port_ == nullptr) return;
        port_->BSRR = level ? (1u << pin_) : (1u << (pin_ + 16u));
    }
    bool get() const noexcept override {
        return port_ != nullptr && (port_->IDR & (1u << pin_)) != 0u;
    }
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

inline Status configure_pin(board::Pin pin, hal::GpioMode mode,
                            hal::GpioPull pull = hal::GpioPull::None,
                            hal::GpioSpeed speed = hal::GpioSpeed::VeryHigh,
                            uint8_t af = 0u, bool open_drain = false) noexcept {
    if (!pin.valid()) return Status::InvalidArg;
    hal::GpioConfig cfg{};
    cfg.mode = mode;
    cfg.pull = pull;
    cfg.speed = speed;
    cfg.alternate = af;
    cfg.open_drain = open_drain;
    GpioPin gpio{pin};
    return gpio.configure(cfg);
}

inline void nvic_irq_enable(uint32_t irq) noexcept {
    *reinterpret_cast<volatile uint32_t*>(0xE000E100u + (irq / 32u) * 4u) =
        (1u << (irq % 32u));
}
inline void nvic_irq_disable(uint32_t irq) noexcept {
    *reinterpret_cast<volatile uint32_t*>(0xE000E180u + (irq / 32u) * 4u) =
        (1u << (irq % 32u));
}

// ─────────────────────────────── UART ───────────────────────────────

template <uint8_t Instance>
struct UartTraits;

template <>
struct UartTraits<1> {
    static USART_TypeDef* reg() noexcept { return USART1; }
    static constexpr bool apb2 = true;
    static constexpr uint32_t rcc_bit = RCC_APB2ENR_USART1EN;
};
template <>
struct UartTraits<2> {
    static USART_TypeDef* reg() noexcept { return USART2; }
    static constexpr bool apb2 = false;
    static constexpr uint32_t rcc_bit = RCC_APB1ENR_USART2EN;
};
template <>
struct UartTraits<3> {
    static USART_TypeDef* reg() noexcept { return USART3; }
    static constexpr bool apb2 = false;
    static constexpr uint32_t rcc_bit = RCC_APB1ENR_USART3EN;
};

/// UART с блокирующим обменом. Пины и AF задаются картой платы.
template <uint8_t Instance>
class Uart final : public hal::IUart {
public:
    Uart(board::Pin tx, board::Pin rx, uint8_t af) noexcept
        : tx_(tx), rx_(rx), af_(af) {}

    static Uart& instance(board::Pin tx, board::Pin rx, uint8_t af) noexcept {
        static Uart d{tx, rx, af};
        return d;
    }

    Status configure(const hal::UartConfig& cfg) noexcept override {
        if (cfg.baud_rate == 0u || cfg.data_bits < 8u || cfg.data_bits > 9u) {
            return Status::InvalidArg;
        }
        if (!tx_.valid() || !rx_.valid()) return Status::InvalidArg;

        if constexpr (UartTraits<Instance>::apb2) {
            RCC->APB2ENR |= UartTraits<Instance>::rcc_bit;
        } else {
            RCC->APB1ENR |= UartTraits<Instance>::rcc_bit;
        }

        if (configure_pin(tx_, hal::GpioMode::Alternate, hal::GpioPull::Up,
                          hal::GpioSpeed::VeryHigh, af_) != Status::Ok) {
            return Status::HardwareError;
        }
        if (configure_pin(rx_, hal::GpioMode::Alternate, hal::GpioPull::Up,
                          hal::GpioSpeed::VeryHigh, af_) != Status::Ok) {
            return Status::HardwareError;
        }

        USART_TypeDef* u = UartTraits<Instance>::reg();
        u->CR1 = 0u;
        u->CR2 = 0u;
        u->CR3 = 0u;

        uint32_t cr1 = USART_CR1_TE | USART_CR1_RE;
        if (cfg.data_bits == 9u) cr1 |= (1u << 12);   // M
        switch (cfg.parity) {
            case hal::UartParity::Even: cr1 |= USART_CR1_PCE; break;
            case hal::UartParity::Odd:  cr1 |= USART_CR1_PCE | USART_CR1_PS; break;
            case hal::UartParity::None: break;
        }
        u->CR1 = cr1;
        u->CR2 = (cfg.stop_bits == hal::UartStopBits::Two) ? (2u << 12u) : 0u;
        u->CR3 = cfg.flow_control ? (USART_CR3_RTSE | USART_CR3_CTSE) : 0u;

        const uint32_t pclk = SystemCoreClock;
        uint32_t brr = (pclk + cfg.baud_rate / 2u) / cfg.baud_rate;
        if (brr == 0u) brr = 1u;
        u->BRR = brr;
        u->CR1 |= USART_CR1_UE;
        cfg_ = cfg;
        configured_ = true;
        return Status::Ok;
    }

    Status open() noexcept override {
        UartTraits<Instance>::reg()->CR1 |= USART_CR1_UE;
        return Status::Ok;
    }
    Status close() noexcept override {
        UartTraits<Instance>::reg()->CR1 &= ~USART_CR1_UE;
        return Status::Ok;
    }

    Status read(void* buffer, Size capacity, Size& bytes_read,
                Timeout timeout) noexcept override {
        bytes_read = 0u;
        if (buffer == nullptr || capacity == 0u || !configured_) {
            return Status::InvalidArg;
        }
        USART_TypeDef* u = UartTraits<Instance>::reg();
        auto* out = static_cast<uint8_t*>(buffer);
        const uint64_t deadline = deadline_for(timeout);

        uint32_t spin = 0u;
        while ((u->SR & USART_SR_RXNE) == 0u) {
            if (wait_expired(deadline, spin)) return Status::Timeout;
        }
        out[bytes_read++] = static_cast<uint8_t>(u->DR);

        // Дренаж уже принятых байтов: shell/протоколу нужна вся пачка, а не
        // один байт на вызов.
        while (bytes_read < capacity) {
            if ((u->SR & USART_SR_RXNE) == 0u) break;
            out[bytes_read++] = static_cast<uint8_t>(u->DR);
        }
        return Status::Ok;
    }

    Status write(const void* data, Size len, Size& written,
                 Timeout timeout) noexcept override {
        written = 0u;
        if (data == nullptr || len == 0u || !configured_) return Status::InvalidArg;
        USART_TypeDef* u = UartTraits<Instance>::reg();
        const auto* in = static_cast<const uint8_t*>(data);
        const uint64_t deadline = deadline_for(timeout);

        for (Size i = 0u; i < len; ++i) {
            uint32_t spin = 0u;
            while ((u->SR & USART_SR_TXE) == 0u) {
                if (wait_expired(deadline, spin)) return Status::Timeout;
            }
            u->DR = in[i];
            ++written;
        }
        uint32_t spin = 0u;
        while ((u->SR & USART_SR_TC) == 0u) {
            if (wait_expired(deadline, spin)) return Status::Timeout;
        }
        return Status::Ok;
    }

    USART_TypeDef* reg() const noexcept { return UartTraits<Instance>::reg(); }

private:
    board::Pin tx_;
    board::Pin rx_;
    uint8_t    af_;
    hal::UartConfig cfg_{};
    bool configured_ = false;
};

using Uart1 = Uart<1>;
using Uart2 = Uart<2>;
using Uart3 = Uart<3>;

// ─────────────────────────────── SPI ───────────────────────────────

template <uint8_t Instance>
struct SpiTraits;

template <>
struct SpiTraits<1> {
    static SPI_TypeDef* reg() noexcept { return SPI1; }
    static constexpr bool apb2 = true;
    static constexpr uint32_t rcc_bit = RCC_APB2ENR_SPI1EN;
};
template <>
struct SpiTraits<2> {
    static SPI_TypeDef* reg() noexcept { return SPI2; }
    static constexpr bool apb2 = false;
    static constexpr uint32_t rcc_bit = RCC_APB1ENR_SPI2EN;
};
template <>
struct SpiTraits<3> {
    static SPI_TypeDef* reg() noexcept { return SPI3; }
    static constexpr bool apb2 = false;
    static constexpr uint32_t rcc_bit = RCC_APB1ENR_SPI3EN;
};

template <uint8_t Instance>
class Spi final : public hal::ISpi {
public:
    Spi(board::Pin sck, board::Pin miso, board::Pin mosi, uint8_t af,
        dma::Route tx_route = dma::Route{}, dma::Route rx_route = dma::Route{}) noexcept
        : sck_(sck), miso_(miso), mosi_(mosi), af_(af),
          tx_route_(tx_route), rx_route_(rx_route) {}

    static Spi& instance(board::Pin sck, board::Pin miso, board::Pin mosi,
                         uint8_t af, dma::Route tx = dma::Route{},
                         dma::Route rx = dma::Route{}) noexcept {
        static Spi d{sck, miso, mosi, af, tx, rx};
        return d;
    }

    Status configure(const hal::SpiConfig& cfg) noexcept override {
        if (cfg.mode > 3u) return Status::InvalidArg;
        if (!sck_.valid() || !mosi_.valid()) return Status::InvalidArg;

        if constexpr (SpiTraits<Instance>::apb2) {
            RCC->APB2ENR |= SpiTraits<Instance>::rcc_bit;
        } else {
            RCC->APB1ENR |= SpiTraits<Instance>::rcc_bit;
        }

        if (configure_pin(sck_, hal::GpioMode::Alternate, hal::GpioPull::None,
                          hal::GpioSpeed::VeryHigh, af_) != Status::Ok) {
            return Status::HardwareError;
        }
        if (configure_pin(mosi_, hal::GpioMode::Alternate, hal::GpioPull::None,
                          hal::GpioSpeed::VeryHigh, af_) != Status::Ok) {
            return Status::HardwareError;
        }
        if (miso_.valid() &&
            configure_pin(miso_, hal::GpioMode::Alternate, hal::GpioPull::None,
                          hal::GpioSpeed::VeryHigh, af_) != Status::Ok) {
            return Status::HardwareError;
        }

        SPI_TypeDef* s = SpiTraits<Instance>::reg();
        s->CR1 = 0u;
        s->CR2 = 0u;

        const uint32_t pclk = SystemCoreClock;
        uint32_t div = 0u;
        uint32_t clock = pclk / 2u;
        while (div < 7u && clock > cfg.hz) { clock /= 2u; ++div; }

        uint32_t cr1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (div << 3u);
        if (!cfg.msb_first) cr1 |= SPI_CR1_LSBFIRST;
        if ((cfg.mode & 2u) != 0u) cr1 |= SPI_CR1_CPOL;
        if ((cfg.mode & 1u) != 0u) cr1 |= SPI_CR1_CPHA;
        s->CR1 = cr1 | SPI_CR1_SPE;
        cfg_ = cfg;
        return Status::Ok;
    }

    Status transfer(const void* tx, void* rx, Size n, Timeout timeout) noexcept override {
        if ((tx == nullptr && rx == nullptr) || n == 0u) return Status::InvalidArg;
        if (cfg_.dma && tx != nullptr && rx != nullptr) {
            const Status st = transfer_dma(tx, rx, n, timeout);
            if (st != Status::NotSupported) return st;
        }
        return transfer_polling(tx, rx, n, timeout);
    }

    Status lock(Timeout) noexcept override {
        Lock l;
        if (busy_) return Status::Busy;
        busy_ = true;
        return Status::Ok;
    }
    void unlock() noexcept override {
        Lock l;
        busy_ = false;
    }
    uint32_t caps() const noexcept override {
        const uint32_t dma = (tx_route_.valid() && rx_route_.valid())
                           ? static_cast<uint32_t>(hal::SpiCapDma) : 0u;
        return static_cast<uint32_t>(hal::SpiCapCs) | dma;
    }

    SPI_TypeDef* reg() const noexcept { return SpiTraits<Instance>::reg(); }

private:
    Status transfer_polling(const void* tx, void* rx, Size n, Timeout timeout) noexcept {
        SPI_TypeDef* s = SpiTraits<Instance>::reg();
        const auto* txb = static_cast<const uint8_t*>(tx);
        auto* rxb = static_cast<uint8_t*>(rx);
        const uint64_t deadline = deadline_for(timeout);

        for (Size i = 0u; i < n; ++i) {
            uint32_t spin = 0u;
            while ((s->SR & SPI_SR_TXE) == 0u) {
                if (wait_expired(deadline, spin)) return Status::Timeout;
            }
            s->DR = (txb != nullptr) ? txb[i] : 0xFFu;

            spin = 0u;
            while ((s->SR & SPI_SR_RXNE) == 0u) {
                if (wait_expired(deadline, spin)) return Status::Timeout;
            }
            const uint8_t in = static_cast<uint8_t>(s->DR);
            if (rxb != nullptr) rxb[i] = in;
        }
        return Status::Ok;
    }

    Status transfer_dma(const void* tx, void* rx, Size n, Timeout timeout) noexcept {
        if (!tx_route_.valid() || !rx_route_.valid()) return Status::NotSupported;
        dma::DmaLease rx_lease;
        dma::DmaLease tx_lease;
        if (!rx_lease.acquire(rx_route_)) return Status::Busy;
        if (!tx_lease.acquire(tx_route_)) return Status::Busy;

        SPI_TypeDef* s = SpiTraits<Instance>::reg();
        dma::dma_clear_flags(tx_route_);
        dma::dma_clear_flags(rx_route_);

        const bool rx_ok = dma::dma_start(rx_route_, &s->DR, rx,
                                          static_cast<uint32_t>(n), false);
        const bool tx_ok = dma::dma_start(tx_route_, &s->DR,
                                          const_cast<void*>(tx),
                                          static_cast<uint32_t>(n), true);
        if (!rx_ok || !tx_ok) {
            dma::dma_stop(rx_route_);
            dma::dma_stop(tx_route_);
            return Status::NotSupported;
        }

        s->CR2 |= (SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
        const uint64_t deadline = deadline_for(timeout);
        Status st = Status::Ok;
        uint32_t spin = 0u;
        while (dma::dma_remaining(rx_route_) != 0u ||
               dma::dma_remaining(tx_route_) != 0u) {
            if (wait_expired(deadline, spin)) {
                st = Status::Timeout;
                break;
            }
        }
        dma::dma_stop(rx_route_);
        dma::dma_stop(tx_route_);
        s->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
        dma::dma_clear_flags(tx_route_);
        dma::dma_clear_flags(rx_route_);
        return st;
    }

    board::Pin sck_;
    board::Pin miso_;
    board::Pin mosi_;
    uint8_t    af_;
    dma::Route tx_route_{};
    dma::Route rx_route_{};
    hal::SpiConfig cfg_{};
    bool busy_ = false;
};

using Spi1 = Spi<1>;
using Spi2 = Spi<2>;
using Spi3 = Spi<3>;

// ─────────────────────────────── I2C ───────────────────────────────

template <uint8_t Instance>
struct I2cTraits;

template <>
struct I2cTraits<1> {
    static I2C_TypeDef* reg() noexcept { return I2C1; }
    static constexpr uint32_t rcc_bit = RCC_APB1ENR_I2C1EN;
};
template <>
struct I2cTraits<2> {
    static I2C_TypeDef* reg() noexcept { return I2C2; }
    static constexpr uint32_t rcc_bit = RCC_APB1ENR_I2C2EN;
};

template <uint8_t Instance>
class I2c final : public hal::II2c {
public:
    I2c(board::Pin scl, board::Pin sda, uint8_t af) noexcept
        : scl_(scl), sda_(sda), af_(af) {}

    static I2c& instance(board::Pin scl, board::Pin sda, uint8_t af) noexcept {
        static I2c d{scl, sda, af};
        return d;
    }

    Status configure(const hal::I2cConfig& cfg) noexcept override {
        if (cfg.hz == 0u || !scl_.valid() || !sda_.valid()) return Status::InvalidArg;
        RCC->APB1ENR |= I2cTraits<Instance>::rcc_bit;

        if (configure_pin(scl_, hal::GpioMode::Alternate, hal::GpioPull::Up,
                          hal::GpioSpeed::VeryHigh, af_, true) != Status::Ok) {
            return Status::HardwareError;
        }
        if (configure_pin(sda_, hal::GpioMode::Alternate, hal::GpioPull::Up,
                          hal::GpioSpeed::VeryHigh, af_, true) != Status::Ok) {
            return Status::HardwareError;
        }

        I2C_TypeDef* i = I2cTraits<Instance>::reg();
#if defined(MCFRAME_SOC_STM32G4)
        i->CR1 = I2C_CR1_PE_G4;
        i->CR1 &= ~I2C_CR1_NOSTRETCH;
        i->TIMINGR = timings(cfg.hz);
#else
        i->CR1 = I2C_CR1_SWRST;
        i->CR1 &= ~I2C_CR1_SWRST;
        i->CR2 = (SystemCoreClock / 1000000u) & I2C_CR2_FREQ;
        uint32_t ccr = 0u;
        if (cfg.hz <= 100000u) {
            ccr = SystemCoreClock / (2u * cfg.hz);
            i->TRISE = (SystemCoreClock / 1000000u) + 1u;
        } else {
            ccr = SystemCoreClock / (3u * cfg.hz);
            i->TRISE = ((SystemCoreClock / 1000000u) * 300u) / 1000u + 1u;
            ccr |= I2C_CCR_FS;
        }
        if (ccr == 0u) return Status::NotSupported;
        i->CCR = ccr;
        i->CR1 = I2C_CR1_PE;
        i->CR1 |= I2C_CR1_ACK;
#endif
        cfg_ = cfg;
        configured_ = true;
        return Status::Ok;
    }

    Status write(uint8_t address, const void* data, Size len,
                 Timeout timeout) noexcept override {
        if (!configured_ || data == nullptr || len == 0u || len > 255u ||
            address > 0x7Fu) {
            return Status::InvalidArg;
        }
#if defined(MCFRAME_SOC_STM32G4)
        return write_g4(address, static_cast<const uint8_t*>(data), len, timeout);
#else
        return write_f4(address, static_cast<const uint8_t*>(data), len, timeout);
#endif
    }

    Status read(uint8_t address, void* buffer, Size capacity, Size& bytes_read,
                Timeout timeout) noexcept override {
        bytes_read = 0u;
        if (!configured_ || buffer == nullptr || capacity == 0u ||
            capacity > 255u || address > 0x7Fu) {
            return Status::InvalidArg;
        }
#if defined(MCFRAME_SOC_STM32G4)
        return read_g4(address, static_cast<uint8_t*>(buffer), capacity,
                       bytes_read, timeout);
#else
        return read_f4(address, static_cast<uint8_t*>(buffer), capacity,
                       bytes_read, timeout);
#endif
    }

    Status lock(Timeout) noexcept override {
        Lock l;
        if (busy_) return Status::Busy;
        busy_ = true;
        return Status::Ok;
    }
    void unlock() noexcept override {
        Lock l;
        busy_ = false;
    }

private:
#if defined(MCFRAME_SOC_STM32G4)
    static uint32_t timings(uint32_t hz) noexcept {
        // Значения для 16 МГц HSI (SystemCoreClock сейчас 16 МГц):
        // PRESC=0, SCLDEL=4, SDADEL=2.
        uint32_t scll = 0x4Fu;
        uint32_t sclh = 0x3Fu;
        if (hz >= 400000u) {
            scll = 0x13u;
            sclh = 0x0Fu;
        }
        return (4u << 20u) | (2u << 16u) | (sclh << 8u) | scll;
    }

    Status write_g4(uint8_t address, const uint8_t* data, Size len,
                    Timeout timeout) noexcept {
        I2C_TypeDef* i = I2cTraits<Instance>::reg();
        if (!wait_busy_clear(i, timeout)) return Status::Busy;
        i->CR2 = (static_cast<uint32_t>(address) << 1u) |
                 ((static_cast<uint32_t>(len) & 0xFFu) << 16u) |
                 I2C_CR2_START | I2C_CR2_AUTOEND;

        const uint64_t deadline = deadline_for(timeout);
        for (Size k = 0u; k < len; ++k) {
            if (!wait_flag(i->ISR, I2C_ISR_TXIS, deadline)) return fail_g4(i);
            i->TXDR = data[k];
        }
        if (!wait_flag(i->ISR, I2C_ISR_TC, deadline)) return fail_g4(i);
        i->ICR = I2C_ICR_STOPCF;
        return Status::Ok;
    }

    Status read_g4(uint8_t address, uint8_t* data, Size len, Size& bytes_read,
                   Timeout timeout) noexcept {
        I2C_TypeDef* i = I2cTraits<Instance>::reg();
        if (!wait_busy_clear(i, timeout)) return Status::Busy;
        i->CR2 = (static_cast<uint32_t>(address) << 1u) | I2C_CR2_RD_WRN |
                 ((static_cast<uint32_t>(len) & 0xFFu) << 16u) |
                 I2C_CR2_START | I2C_CR2_AUTOEND;

        const uint64_t deadline = deadline_for(timeout);
        for (Size k = 0u; k < len; ++k) {
            if (!wait_flag(i->ISR, I2C_ISR_RXNE, deadline)) return fail_g4(i);
            data[k] = static_cast<uint8_t>(i->RXDR);
            ++bytes_read;
        }
        if (!wait_flag(i->ISR, I2C_ISR_STOPF, deadline)) return fail_g4(i);
        i->ICR = I2C_ICR_STOPCF;
        return Status::Ok;
    }

    static bool wait_busy_clear(I2C_TypeDef* i, Timeout timeout) noexcept {
        const uint64_t deadline = deadline_for(timeout);
        uint32_t spin = 0u;
        while ((i->ISR & I2C_ISR_BUSY) != 0u) {
            if (wait_expired(deadline, spin)) return false;
        }
        return true;
    }

    static bool wait_flag(volatile uint32_t& reg, uint32_t mask,
                          uint64_t deadline) noexcept {
        uint32_t spin = 0u;
        while ((reg & mask) == 0u) {
            if (wait_expired(deadline, spin)) return false;
        }
        return true;
    }

    static Status fail_g4(I2C_TypeDef* i) noexcept {
        const uint32_t isr = i->ISR;
        i->CR2 |= I2C_CR2_STOP;
        if ((isr & I2C_ISR_NACKF) != 0u) {
            i->ICR = I2C_ICR_NACKCF;
            return Status::NotFound;
        }
        return Status::Timeout;
    }
#else
    Status write_f4(uint8_t address, const uint8_t* data, Size len,
                    Timeout timeout) noexcept {
        I2C_TypeDef* i = I2cTraits<Instance>::reg();
        const uint64_t deadline = deadline_for(timeout);

        if (!wait_sr1(i, I2C_SR1_TXE, deadline)) return fail_f4(i);
        i->CR1 |= I2C_CR1_START;
        if (!wait_sr1(i, I2C_SR1_SB, deadline)) return fail_f4(i);
        i->DR = static_cast<uint32_t>(address << 1u);
        if (!wait_sr1(i, I2C_SR1_ADDR, deadline)) return fail_f4(i);
        (void)i->SR1;
        (void)i->SR2;

        for (Size k = 0u; k < len; ++k) {
            if (!wait_sr1(i, I2C_SR1_TXE, deadline)) return fail_f4(i);
            i->DR = data[k];
        }
        if (!wait_sr1(i, I2C_SR1_BTF, deadline)) return fail_f4(i);
        i->CR1 |= I2C_CR1_STOP;
        return Status::Ok;
    }

    Status read_f4(uint8_t address, uint8_t* data, Size len, Size& bytes_read,
                   Timeout timeout) noexcept {
        I2C_TypeDef* i = I2cTraits<Instance>::reg();
        const uint64_t deadline = deadline_for(timeout);

        i->CR1 |= I2C_CR1_ACK;
        if (!wait_sr1(i, I2C_SR1_TXE, deadline)) return fail_f4(i);
        i->CR1 |= I2C_CR1_START;
        if (!wait_sr1(i, I2C_SR1_SB, deadline)) return fail_f4(i);
        i->DR = static_cast<uint32_t>((address << 1u) | 1u);
        if (!wait_sr1(i, I2C_SR1_ADDR, deadline)) return fail_f4(i);
        (void)i->SR1;
        (void)i->SR2;

        for (Size k = 0u; k < len; ++k) {
            if (k + 1u == len) {
                i->CR1 &= ~I2C_CR1_ACK;
                i->CR1 |= I2C_CR1_STOP;
            }
            if (!wait_sr1(i, I2C_SR1_RXNE, deadline)) return fail_f4(i);
            data[k] = static_cast<uint8_t>(i->DR);
            ++bytes_read;
        }
        i->CR1 |= I2C_CR1_ACK;
        return Status::Ok;
    }

    static bool wait_sr1(I2C_TypeDef* i, uint32_t mask, uint64_t deadline) noexcept {
        uint32_t spin = 0u;
        for (;;) {
            const uint32_t sr = i->SR1;
            if ((sr & I2C_SR1_AF) != 0u) {
                i->SR1 = static_cast<uint32_t>(~I2C_SR1_AF);
                i->CR1 |= I2C_CR1_STOP;
                return false;
            }
            if ((sr & mask) != 0u) return true;
            if (wait_expired(deadline, spin)) return false;
        }
    }

    static Status fail_f4(I2C_TypeDef* i) noexcept {
        const uint32_t sr = i->SR1;
        i->CR1 |= I2C_CR1_STOP;
        if ((sr & I2C_SR1_AF) != 0u) {
            i->SR1 = static_cast<uint32_t>(~I2C_SR1_AF);
            return Status::NotFound;
        }
        return Status::Timeout;
    }
#endif

    board::Pin scl_;
    board::Pin sda_;
    uint8_t    af_;
    hal::I2cConfig cfg_{};
    bool configured_ = false;
    bool busy_ = false;
};

using I2c1 = I2c<1>;
using I2c2 = I2c<2>;

// ─────────────────────────────── ADC ───────────────────────────────

template <uint8_t Instance>
struct AdcTraits;

template <>
struct AdcTraits<1> {
    static ADC_TypeDef* reg() noexcept { return ADC1; }
};

template <uint8_t Instance>
class Adc final : public hal::IAdc {
public:
    static Adc& instance() noexcept {
        static Adc d;
        return d;
    }

    Status configure(const hal::AdcConfig& cfg) noexcept override {
        if (cfg.resolution_bits != 12u && cfg.resolution_bits != 10u &&
            cfg.resolution_bits != 8u && cfg.resolution_bits != 6u) {
            return Status::InvalidArg;
        }
#if defined(MCFRAME_SOC_STM32G4)
        RCC->AHB2ENR |= RCC_AHB2ENR_ADC12EN;
#else
        RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
#endif
        ADC_TypeDef* a = AdcTraits<Instance>::reg();
        uint32_t res = 0u;
        if (cfg.resolution_bits == 10u) res = 1u;
        else if (cfg.resolution_bits == 8u) res = 2u;
        else if (cfg.resolution_bits == 6u) res = 3u;
        a->CR1 = (a->CR1 & ~ADC_CR1_RES) | (res << 24u);

        // 480 циклов на все каналы: предсказуемый шум и входное сопротивление.
        a->SMPR1 = 0xFFFFFFFFu;
        a->SMPR2 = 0xFFFFFFFFu;
        a->CR2 |= ADC_CR2_ADON;
        cfg_ = cfg;
        configured_ = true;
        return Status::Ok;
    }

    Status read(uint8_t channel, uint16_t& value) noexcept override {
        value = 0u;
        if (!configured_ || channel > 18u) return Status::InvalidArg;
        ADC_TypeDef* a = AdcTraits<Instance>::reg();

        a->SQR1 &= ~ADC_SQR1_L;                 // одна позиция в последовательности
        a->SQR3 = (a->SQR3 & ~ADC_SQR3_SQ1) | channel;
        a->SR = 0u;                             // снять старый EOC
        a->CR2 |= ADC_CR2_SWSTART;

        const uint64_t deadline = deadline_for(10u);
        uint32_t spin = 0u;
        while ((a->SR & ADC_SR_EOC) == 0u) {
            if (wait_expired(deadline, spin)) return Status::Timeout;
        }
        value = static_cast<uint16_t>(a->DR & 0xFFFFu);
        return Status::Ok;
    }

private:
    hal::AdcConfig cfg_{};
    bool configured_ = false;
};

using Adc1 = Adc<1>;

// ─────────────────────────────── PWM ───────────────────────────────

class Pwm final : public hal::IPwm {
public:
    Pwm(TIM_TypeDef* tim, uint8_t channel, board::Pin pin, uint8_t af,
        uint32_t pclk) noexcept
        : tim_(tim), channel_(channel), pin_(pin), af_(af), pclk_(pclk) {}

    Status configure(const hal::PwmConfig& cfg) noexcept override {
        if (tim_ == nullptr || channel_ < 1u || channel_ > 4u ||
            cfg.frequency_hz == 0u || cfg.duty_percent > 100u) {
            return Status::InvalidArg;
        }
        RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;

        if (configure_pin(pin_, hal::GpioMode::Alternate, hal::GpioPull::None,
                          hal::GpioSpeed::High, af_) != Status::Ok) {
            return Status::HardwareError;
        }

        const uint32_t tick_hz = 1000000u;
        const uint32_t psc = (pclk_ / tick_hz == 0u) ? 0u : (pclk_ / tick_hz - 1u);
        uint32_t arr = tick_hz / cfg.frequency_hz;
        if (arr == 0u) arr = 1u;
        if (arr > 65536u) arr = 65536u;
        tim_->PSC = psc;
        tim_->ARR = arr - 1u;
        ccr(channel_) = (arr * cfg.duty_percent) / 100u;

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
    board::Pin pin_;
    uint8_t af_;
    uint32_t pclk_;
};

} // namespace embed::stm32
