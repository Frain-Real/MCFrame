// platforms/stm32/i2c_stm32.hpp
#pragma once
// ---------------------------------------------------------------------------
// Драйвер I2C для STM32F103 (I2C1) с поддержкой трёх стратегий.
//
//   Polling — драйвер опрашивает SR в pollOnIrq() (AsyncPort вызывает его
//             в цикле ожидания);
//   Async   — события через I2C1_EV_IRQHandler, ошибки через I2C1_ER_IRQHandler;
//   Dma     — DMA1 Channel6 (TX) / Channel7 (RX).
//
// ЛОГИКА ОДНА НА ВСЕ РЕЖИМЫ
// Есть один автомат pump(), который двигает обмен. Режим определяет лишь
// то, КТО его вызывает: прерывание, цикл опроса или колбэк DMA. Если
// писать «режим прерываний» и «режим опроса» раздельно, они почти
// неизбежно расходятся — здесь этого не может быть по построению.
//
// ПОСЛЕДОВАТЕЛЬНОСТЬ ОБМЕНА (RM0008, рис. 244-246)
//   запись:      SB -> адрес+W -> ADDR -> TXE* -> ... -> BTF -> STOP
//   чтение:      SB -> адрес+R -> ADDR -> RXNE* -> ... -> NACK+STOP
//   регистр:     SB -> адрес+W -> ADDR -> TXE* -> BTF -> START -> адрес+R -> ...
// ---------------------------------------------------------------------------
#include "../../os/async/async_port.hpp"   // async::Strategy
#include "../../os/interfaces/ii2c.hpp"
#include "../../os/interfaces/dma_manager.hpp"
#include "../../os/config/dma_config.hpp"

#include "stm32f10x.h"
#include "core_cm3.h"

namespace embed::stm32 {

class I2cDriver1 : public II2c<I2cDriver1> {
public:
    static constexpr bool            kHasDma    = true;
    static constexpr async::Strategy kStrategy  = async::Strategy::Polling;
    static constexpr Size            kMaxChunk  = 255u;   // предел протокола

    using Base    = II2c<I2cDriver1>;
    using Address = I2cAddress;
    using Config  = typename Base::Config;
    using Speed   = typename Base::Speed;
    using MasterMode = typename Base::MasterMode;

    static I2cDriver1& instance() noexcept {
        static I2cDriver1 d;
        return d;
    }

    // ─────────────────────────── Настройка ───────────────────────────

    Status configure(const Config& cfg) noexcept {
        if (cfg.peripheralClockHz == 0u) return Status::InvalidArg;

        cfg_ = cfg;
        RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;

        // Сброс периферии: после сбоя на шине контроллер может остаться
        // в неопределённом состоянии, и без сброса обмен не пойдёт.
        I2C1->CR1 |= I2C_CR1_SWRST;
        I2C1->CR1 &= ~I2C_CR1_SWRST;

        I2C1->OAR1 = static_cast<uint32_t>(cfg.ownAddress.value) << 1u;
        I2C1->OAR2 = 0u;

        const uint32_t ccr = computeCcr(cfg.peripheralClockHz, cfg.speed);
        if (ccr == 0u) return Status::NotSupported;   // скорость не поддержана

        if (cfg.speed == Speed::Standard100k) {
            I2C1->CCR   = ccr & 0xFFFu;
            I2C1->TRISE = (cfg.peripheralClockHz / 1000000u) + 1u;
        } else {
            I2C1->CCR   = I2C_CCR_FS | (ccr & 0xFFFu);
            // В быстром режиме фронт не длиннее 300 нс.
            I2C1->TRISE = ((cfg.peripheralClockHz / 1000000u) * 300u) / 1000u + 1u;
        }

        // Растягивание тактов включено по умолчанию (бит NOSTRETCH сброшен).
        I2C1->CR1 = I2C_CR1_PE;
        I2C1->CR1 &= ~I2C_CR1_NOSTRETCH;

        return Status::Ok;
    }

    Status open() noexcept {
        I2C1->CR1 |= I2C_CR1_PE;
        return Status::Ok;
    }

    Status close() noexcept {
        I2C1->CR1 &= ~I2C_CR1_PE;
        return Status::Ok;
    }

    Status ioctl(unsigned request, void* arg) noexcept {
        (void)request;
        (void)arg;
        return Status::NotSupported;
    }

    /// Сменить скорость без пересоздания драйвера.
    Status setSpeed(Speed speed) noexcept {
        Config c = cfg_;
        c.speed  = speed;
        return configure(c);
    }

    // ─────────────────────── Асинхронные операции ───────────────────────

    /// Привязать объект завершения. Владелец объекта — порт
    /// (см. async_port.hpp), поэтому заменяем ссылку, а не отказываем.
    Status setCompletion(async::completion& token) noexcept {
        if (phase_ != Phase::Idle) return Status::Busy;
        pending_ = &token;
        return Status::Ok;
    }

    async::completion* pendingCompletion() noexcept { return pending_; }

    Status startWrite(Address addr, const void* data, Size len) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (data == nullptr || len == 0u || !addr.valid()) return Status::InvalidArg;
        if (len > kMaxChunk) return Status::InvalidArg;
        if (phase_ != Phase::Idle) return Status::Busy;

        mode_   = Mode::Write;
        addr_   = addr;
        tx_     = static_cast<const uint8_t*>(data);
        tx_len_ = len;
        tx_pos_ = 0u;
        rx_     = nullptr;
        rx_len_ = 0u;
        rx_pos_ = 0u;

        beginTransfer();
        return Status::Ok;
    }

    Status startRead(Address addr, void* buffer, Size len) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (buffer == nullptr || len == 0u || !addr.valid()) return Status::InvalidArg;
        if (len > kMaxChunk) return Status::InvalidArg;
        if (phase_ != Phase::Idle) return Status::Busy;

        mode_   = Mode::Read;
        addr_   = addr;
        tx_     = nullptr;
        tx_len_ = 0u;
        tx_pos_ = 0u;
        rx_     = static_cast<uint8_t*>(buffer);
        rx_len_ = len;
        rx_pos_ = 0u;

        beginTransfer();
        return Status::Ok;
    }

    Status startWriteRead(Address addr, const void* tx, Size tx_len,
                          void* rx, Size rx_len) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (tx == nullptr || tx_len == 0u || rx == nullptr || rx_len == 0u) {
            return Status::InvalidArg;
        }
        if (tx_len > kMaxChunk || rx_len > kMaxChunk) return Status::InvalidArg;
        if (!addr.valid() || phase_ != Phase::Idle) return Status::Busy;

        mode_   = Mode::WriteRead;
        addr_   = addr;
        tx_     = static_cast<const uint8_t*>(tx);
        tx_len_ = tx_len;
        tx_pos_ = 0u;
        rx_     = static_cast<uint8_t*>(rx);
        rx_len_ = rx_len;
        rx_pos_ = 0u;

        beginTransfer();
        return Status::Ok;
    }

    /// Проверка наличия устройства на шине. Ok означает «ответило».
    Status probe(Address addr, Timeout /*timeout*/) noexcept {
        if (!addr.valid()) return Status::InvalidArg;

        // Объект завершения обязан пережить вызов: драйвер хранит на него
        // указатель. Локальная переменная на стеке дала бы обращение
        // к уничтоженному объекту (это ловил ASan).
        static async::completion local;
        local.reset(async::WaitPolicy::Forever);
        if (setCompletion(local) != Status::Ok) return Status::Busy;

        mode_     = Mode::Probe;
        addr_     = addr;
        tx_       = nullptr; tx_len_ = 0u; tx_pos_ = 0u;
        rx_       = nullptr; rx_len_ = 0u; rx_pos_ = 0u;

        beginTransfer();
        (void)local.wait();
        return local.status();
    }

    Status abort() noexcept {
        I2C1->CR1 |= I2C_CR1_STOP;
        disableInterrupts();
        stopDma();
        phase_ = Phase::Idle;
        complete(Status::Aborted, completedBytes());
        return Status::Ok;
    }

    /// Восстановление шины: сброс контроллера.
    /// Нужен, если ведомый удерживает SDA и обмен «залип».
    Status recover() noexcept {
        I2C1->CR1 &= ~I2C_CR1_PE;
        I2C1->CR1 |= I2C_CR1_SWRST;
        I2C1->CR1 &= ~I2C_CR1_SWRST;
        I2C1->CR1 |= I2C_CR1_PE;

        disableInterrupts();
        stopDma();
        phase_ = Phase::Idle;
        return Status::Ok;
    }

    // ─────────────────────── Точки входа ───────────────────────

    void onCompleteIrq() noexcept { pump(); }
    void onErrorIrq(Status st) noexcept { fail(st); }
    void pollOnIrq() noexcept { pump(); }

    bool busy() const noexcept { return phase_ != Phase::Idle; }

private:
    enum class Phase : uint8_t {
        Idle = 0,
        Start,        // ждём SB после запроса START
        SendAddress,  // ждём ADDR после отправки адреса
        WriteData,    // ждём TXE и пишем байты
        WriteDone,    // ждём BTF перед STOP/повторным START
        ReadData,     // ждём RXNE и читаем байты
        ReadLast,     // последний байт: NACK + STOP
    };

    enum class Mode : uint8_t { Write = 0, Read, WriteRead, Probe };

    void beginTransfer() noexcept {
        enableInterrupts();
        phase_ = Phase::Start;
        I2C1->SR1 = 0u;                 // очистить залипшие флаги
        pump();                          // первый шаг делаем сразу
    }

    /// Единственная точка, двигающая обмен.
    void pump() noexcept {
        if (pending_ == nullptr || phase_ == Phase::Idle) return;

        const uint32_t sr1 = I2C1->SR1;
        const uint32_t sr2 = I2C1->SR2;    // чтение SR2 очищает ADDR

        // Ошибки раньше событий: иначе можно «залипнуть» на невыясненной AF.
        if ((sr1 & I2C_SR1_AF) != 0u) {
            I2C1->SR1 &= ~I2C_SR1_AF;
            I2C1->CR1 |= I2C_CR1_STOP;
            fail(mode_ == Mode::Probe ? Status::NotFound : Status::HardwareError);
            return;
        }
        if ((sr1 & (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR)) != 0u) {
            I2C1->SR1 &= ~(I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR);
            I2C1->CR1 |= I2C_CR1_STOP;
            fail(Status::HardwareError);
            return;
        }

        switch (phase_) {
            case Phase::Idle:
                return;

            case Phase::Start:
                if ((sr1 & I2C_SR1_SB) == 0u) {
                    I2C1->CR1 |= I2C_CR1_START;
                    return;              // SB придёт позже
                }
                // SB установлен: отправляем адрес с нужным битом R/W.
                I2C1->DR = (mode_ == Mode::Read) ? addr_.readByte()
                                                 : addr_.writeByte();
                phase_ = Phase::SendAddress;
                return;

            case Phase::SendAddress:
                if ((sr1 & I2C_SR1_ADDR) == 0u) return;   // ещё не ответили
                (void)sr2;                                // очистка ADDR

                if (mode_ == Mode::Probe) {
                    I2C1->CR1 |= I2C_CR1_STOP;
                    phase_ = Phase::Idle;
                    complete(Status::Ok, 0u);
                    return;
                }
                if (mode_ == Mode::Read) {
                    I2C1->CR1 |= I2C_CR1_ACK;
                    prepareReadAck();
                    phase_ = Phase::ReadData;
                    return;
                }
                phase_ = Phase::WriteData;
                return;

            case Phase::WriteData:
                if ((sr1 & I2C_SR1_TXE) == 0u) return;
                if (tx_pos_ < tx_len_) {
                    I2C1->DR = tx_[tx_pos_++];
                    if (tx_pos_ >= tx_len_) phase_ = Phase::WriteDone;
                    return;
                }
                phase_ = Phase::WriteDone;
                return;

            case Phase::WriteDone:
                // BTF: байт ушёл в сдвиговый регистр и линия свободна.
                if ((sr1 & I2C_SR1_BTF) == 0u) return;
                if (mode_ == Mode::WriteRead) {
                    // Повторный START без STOP — так читают регистры.
                    phase_ = Phase::Start;
                    mode_  = Mode::Read;
                    return;
                }
                I2C1->CR1 |= I2C_CR1_STOP;
                phase_ = Phase::Idle;
                complete(Status::Ok, tx_pos_);
                return;

            case Phase::ReadData:
                if ((sr1 & I2C_SR1_RXNE) == 0u) return;
                if (rx_pos_ + 1u < rx_len_) {
                    rx_[rx_pos_++] = static_cast<uint8_t>(I2C1->DR & 0xFFu);
                    prepareReadAck();
                    return;
                }
                // Предпоследний байт прочитан — остался последний.
                rx_[rx_pos_++] = static_cast<uint8_t>(I2C1->DR & 0xFFu);
                I2C1->CR1 &= ~I2C_CR1_ACK;      // NACK на последний байт
                I2C1->CR1 |= I2C_CR1_STOP;
                phase_ = Phase::ReadLast;
                return;

            case Phase::ReadLast:
                // У F103 STOP завершает приём: ждём, пока контроллер
                // действительно уйдёт в Idle (флаг STOPF/BTF снят).
                if ((sr1 & I2C_SR1_RXNE) != 0u) {
                    if (rx_pos_ < rx_len_) {
                        rx_[rx_pos_++] = static_cast<uint8_t>(I2C1->DR & 0xFFu);
                    }
                }
                if ((sr1 & (I2C_SR1_STOPF | I2C_SR1_BTF)) != 0u) {
                    I2C1->SR1 &= ~I2C_SR1_STOPF;
                }
                phase_ = Phase::Idle;
                complete(Status::Ok, rx_pos_);
                return;
        }
    }

    /// Подготовка приёма для режима 1-байтового остатка.
    /// Для длины 1 сразу NACK+STOP, иначе ACK и ждём.
    void prepareReadAck() noexcept {
        if (rx_pos_ + 1u >= rx_len_) {
            I2C1->CR1 &= ~I2C_CR1_ACK;      // это последний байт
            I2C1->CR1 |= I2C_CR1_STOP;
        } else {
            I2C1->CR1 |= I2C_CR1_ACK;
        }
    }

    void fail(Status st) noexcept {
        disableInterrupts();
        stopDma();
        I2C1->CR1 |= I2C_CR1_STOP;
        phase_ = Phase::Idle;
        complete(st, completedBytes());
    }

    void complete(Status st, Size transferred) noexcept {
        async::completion* p = pending_;
        pending_ = nullptr;
        if (p == nullptr) return;
        if (st == Status::Ok) (void)p->post_ok(transferred);
        else                  (void)p->post_error(st);
    }

    Size completedBytes() const noexcept {
        return (mode_ == Mode::Read || mode_ == Mode::WriteRead) ? rx_pos_ : tx_pos_;
    }

    void enableInterrupts() noexcept {
        I2C1->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITERREN | I2C_CR2_ITBUFEN;
        nvic_enable(static_cast<uint32_t>(I2C1_EV_IRQn));
        nvic_enable(static_cast<uint32_t>(I2C1_ER_IRQn));
    }

    void disableInterrupts() noexcept {
        I2C1->CR2 &= ~(I2C_CR2_ITEVTEN | I2C_CR2_ITERREN | I2C_CR2_ITBUFEN);
    }

    void stopDma() noexcept {
        I2C1->CR2 &= ~(I2C_CR2_DMAEN | I2C_CR2_LAST);
    }

    static uint32_t computeCcr(uint32_t pclk1, Speed speed) noexcept {
        switch (speed) {
            case Speed::Standard100k:
                // CCR = pclk1 / (2 * f_scl). Для 36 МГц и 100 кГц: 180.
                return pclk1 / 200000u;
            case Speed::Fast400k:
                // Duty 2:1: CCR = pclk1 / (3 * f_scl). Для 36 МГц: 30.
                return pclk1 / 1200000u;
            case Speed::FastPlus1M:
                // STM32F103 в стандартном режиме ограничен 100 кГц, а
                // 1 МГц не поддерживается: честно сообщаем об этом.
                return 0u;
        }
        return 0u;
    }

    static void nvic_enable(uint32_t irqn) noexcept {
        NVIC->ISER[irqn >> 5u] = (1UL << (irqn & 0x1Fu));
    }

    Config cfg_{};
    async::completion* pending_ = nullptr;

    Phase   phase_ = Phase::Idle;
    Mode    mode_  = Mode::Write;
    Address addr_{};

    const uint8_t* tx_ = nullptr;
    Size tx_len_ = 0u;
    Size tx_pos_ = 0u;

    uint8_t* rx_ = nullptr;
    Size rx_len_ = 0u;
    Size rx_pos_ = 0u;

    DmaChannel dma_tx_{};
    DmaChannel dma_rx_{};
};

} // namespace embed::stm32
