// platforms/stm32/i2c_stm32.hpp
#pragma once
// ---------------------------------------------------------------------------
// Драйвер I2C1 (STM32F103) с ТРЕМЯ РЕАЛЬНЫМИ стратегиями.
//
//   Polling — автомат двигает pollOnIrq(); ITEVTEN/ITERREN/ITBUFEN и NVIC
//             ВЫКЛЮЧЕНЫ, DMA не участвует;
//   Async   — события через I2C1_EV_IRQHandler, ошибки через
//             I2C1_ER_IRQHandler (ITEVTEN/ITERREN/ITBUFEN включены);
//   Dma     — данные возит DMA1 Channel6 (TX) / Channel7 (RX); прерывания
//             остаются только на события шины (SB/ADDR/BTF/STOPF) и ошибки:
//             ITBUFEN снят, иначе CPU и DMA читали бы DR одновременно.
//
// ЛОГИКА ОДНА НА ВСЕ РЕЖИМЫ
// Есть один автомат pump(), который двигает обмен. Режим определяет лишь
// то, КТО его вызывает и кто переносит байты: прерывание, цикл опроса, DMA
// или колбэк DMA. Если писать «режим прерываний» и «режим DMA» раздельно,
// они почти неизбежно расходятся — здесь этого не может быть по построению.
//
// ПОСЛЕДОВАТЕЛЬНОСТЬ ОБМЕНА (RM0008, рис. 244-246)
//   запись:      SB -> адрес+W -> ADDR -> TXE* -> ... -> BTF -> STOP
//   чтение:      SB -> адрес+R -> ADDR -> RXNE* -> ... -> NACK+STOP
//   регистр:     SB -> адрес+W -> ADDR -> TXE* -> BTF -> START -> адрес+R -> ...
//
// ОСОБЕННОСТЬ ПРИЁМА ЧЕРЕЗ DMA
// DMA переносит N-1 байт, последний байт читает драйвер: NACK на последний
// байт и STOP нужно выставить ДО того, как он принят, а признак «остался
// последний» у DMA-канала берётся не из счётчика, а из состояния автомата.
// Так последовательность ACK/NACK/STOP одна и та же во всех трёх режимах.
//
// Проверка: tests/host/test_stm32_hal.cpp — все три стратегии прогоняются на
// ПК против программной модели регистров (см. stm32_regs.hpp).
// ---------------------------------------------------------------------------
#include "../../os/async/async_port.hpp"   // async::Strategy
#include "../../os/interfaces/ii2c.hpp"
#include "../../os/interfaces/dma_manager.hpp"
#include "../../os/config/dma_config.hpp"

#include "stm32_regs.hpp"

namespace embed::stm32 {

class I2cDriver1 : public II2c<I2cDriver1> {
public:
    static constexpr bool            kHasDma    = true;
    static constexpr Size            kMaxChunk  = 255u;   // предел протокола
    /// Стратегия по умолчанию: прерывания. DMA включается явно — он требует
    /// свободных каналов DMA1 Ch6/Ch7.
    static constexpr async::Strategy kDefaultStrategy = async::Strategy::Async;

    /// Запросы DMA этого интерфейса (RM0008). Нужны в том числе арбитру
    /// каналов (dma_arbiter.hpp). Обе операции writeRead нужны сразу оба:
    /// каналы занимаются разом, хотя используются по фазам.
    static constexpr config::DmaRequest kTxRequest = config::DmaRequest::I2c1Tx;
    static constexpr config::DmaRequest kRxRequest = config::DmaRequest::I2c1Rx;

    using Base       = II2c<I2cDriver1>;
    using Address    = I2cAddress;
    using Speed      = Base::Speed;
    using MasterMode = Base::MasterMode;

    /// Конфигурация = базовая + выбор стратегии.
    ///
    /// Конструктор от Base::Config нужен, чтобы вызов configure() через
    /// интерфейс (II2c<I2cDriver1>&) не срезал поле strategy.
    struct Config : Base::Config {
        async::Strategy strategy = kDefaultStrategy;

        Config() = default;
        Config(const Base::Config& base) noexcept : Base::Config(base) {}
    };

    static I2cDriver1& instance() noexcept {
        static I2cDriver1 d;
        return d;
    }

    /// Текущая стратегия (её читает async::I2cPort).
    static async::Strategy strategy() noexcept { return instance().strategy_; }

    // ─────────────────────────── Настройка ───────────────────────────

    Status configure(const Config& cfg) noexcept {
        if (cfg.peripheralClockHz == 0u) return Status::InvalidArg;
        if (busy()) return Status::Busy;

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

        return selectStrategy(cfg.strategy);
    }

    Status open() noexcept {
        I2C1->CR1 |= I2C_CR1_PE;
        return Status::Ok;
    }

    Status close() noexcept {
        (void)abort();
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

    // ─────────────────────── Выбор стратегии ───────────────────────

    /// Переключить стратегию. Требуется простой: на середине обмена менять
    /// способ переноса байт нельзя.
    Status selectStrategy(async::Strategy s) noexcept {
        if (s == async::Strategy::Dma && !kHasDma) return Status::NotSupported;
        if (busy()) return Status::Busy;

        disableInterrupts();
        stopDma();                       // DMAEN снят, каналы освобождены

        // NVIC нужен прерываниям и DMA: события шины (SB/ADDR/BTF)
        // обслуживаются обработчиком и в DMA-режиме.
        if (s == async::Strategy::Polling) {
            nvic_disable(static_cast<uint32_t>(I2C1_EV_IRQn));
            nvic_disable(static_cast<uint32_t>(I2C1_ER_IRQn));
        } else {
            nvic_enable(static_cast<uint32_t>(I2C1_EV_IRQn));
            nvic_enable(static_cast<uint32_t>(I2C1_ER_IRQn));
        }

        strategy_ = s;
        return Status::Ok;
    }

    /// Опрос флагов: ни прерываний, ни DMA.
    Status usePolling() noexcept { return selectStrategy(async::Strategy::Polling); }
    /// События и ошибки по прерываниям.
    Status useInterrupts() noexcept { return selectStrategy(async::Strategy::Async); }
    /// Данные через DMA1 Ch6 (TX) / Ch7 (RX), события — по прерываниям.
    Status useDma() noexcept { return selectStrategy(async::Strategy::Dma); }

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
        // В режиме опроса обмен двигает не прерывание, а опрос: без этого
        // probe() ждал бы завершения, которого никто не выставит.
        if (strategy_ == async::Strategy::Polling) {
            local.set_poll(&probePoll, nullptr);
        }
        if (setCompletion(local) != Status::Ok) return Status::Busy;

        mode_     = Mode::Probe;
        addr_     = addr;
        tx_       = nullptr; tx_len_ = 0u; tx_pos_ = 0u;
        rx_       = nullptr; rx_len_ = 0u; rx_pos_ = 0u;

        beginTransfer();
        (void)local.wait();
        // Завершение не пришло (таймаут) — снимаем операцию: иначе автомат
        // остался бы на середине обмена, и следующий вызов получил бы Busy.
        if (pending_ == &local) (void)abort();
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
    void pollOnIrq() noexcept { pump(); }

    /// Обработчик ER-прерывания.
    ///
    /// Конкретный код (AF/BERR/ARLO/OVR) драйвер разбирает САМ по SR1 —
    /// ровно так же, как это делает опрос и EV-обработчик. Иначе NACK
    /// отсутствующего устройства приходил бы как HardwareError, тогда как
    /// mock-шина (и прикладной код) ждут NotFound.
    void onErrorIrq(Status st) noexcept {
        if (pending_ == nullptr) return;

        const uint32_t sr1 = I2C1->SR1;
        const bool known = (sr1 & (I2C_SR1_AF | I2C_SR1_BERR |
                                   I2C_SR1_ARLO | I2C_SR1_OVR)) != 0u;
        if (!known) {
            // Флага нет — прерывание пришло «не по делу»: сообщаем как есть,
            // иначе операция осталась бы висеть.
            fail(st);
            return;
        }
        pump();
    }

    /// Колбэк DMA: данные перенесены, автомат продолжает работу.
    void onDmaComplete(Status st = Status::Ok) noexcept {
        if (st != Status::Ok) { fail(st); return; }
        dma_done_ = true;
        pump();
    }

    bool busy() const noexcept { return phase_ != Phase::Idle; }
    /// Данные переносит DMA (в этом режиме драйвер не трогает DR).
    bool dmaUsed() const noexcept { return strategy_ == async::Strategy::Dma; }

private:
    enum class Phase : uint8_t {
        Idle = 0,
        Start,        // ждём SB после запроса START
        SendAddress,  // ждём ADDR после отправки адреса
        WriteData,    // ждём TXE и пишем байты (драйвер)
        WriteDataDma, // байты пишет DMA, ждём его завершения
        WriteDone,    // ждём BTF перед STOP/повторным START
        ReadData,     // ждём RXNE и читаем байты (драйвер)
        ReadDataDma,  // байты читает DMA, ждём его завершения
        ReadLast,     // последний байт: NACK + STOP
    };

    enum class Mode : uint8_t { Write = 0, Read, WriteRead, Probe };

    void beginTransfer() noexcept {
        dma_done_ = false;
        phase_ = Phase::Start;
        I2C1->SR1 = 0u;                 // очистить залипшие флаги
        enableInterrupts();             // набор битов зависит от стратегии
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
            // AF на байте АДРЕСА означает «устройства нет» — это NotFound,
            // и так же ведёт себя mock-шина. AF на байте данных — сбой
            // обмена, а не отсутствие устройства.
            const bool no_device = (mode_ == Mode::Probe) ||
                                   (phase_ == Phase::SendAddress);
            fail(no_device ? Status::NotFound : Status::HardwareError);
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
                    complete(Status::Ok, 0u);
                    return;
                }
                if (mode_ == Mode::Read) {
                    I2C1->CR1 |= I2C_CR1_ACK;
                    if (dmaUsed() && rx_len_ > 1u) {
                        // DMA берёт N-1 байт, последний читает драйвер.
                        if (startRxDma(rx_len_ - 1u) != Status::Ok) {
                            fail(Status::HardwareError);
                            return;
                        }
                        I2C1->CR2 |= I2C_CR2_DMAEN;
                        phase_ = Phase::ReadDataDma;
                        return;
                    }
                    enableBufferInterrupt(true);   // ждём RXNE по прерыванию
                    prepareReadAck();
                    phase_ = Phase::ReadData;
                    return;
                }
                if (dmaUsed()) {
                    if (startTxDma(tx_len_) != Status::Ok) {
                        fail(Status::HardwareError);
                        return;
                    }
                    I2C1->CR2 |= I2C_CR2_DMAEN;
                    phase_ = Phase::WriteDataDma;
                    return;
                }
                enableBufferInterrupt(true);       // ждём TXE по прерыванию
                phase_ = Phase::WriteData;
                return;

            case Phase::WriteData:
                if ((sr1 & I2C_SR1_TXE) == 0u) return;
                if (tx_pos_ < tx_len_) {
                    I2C1->DR = tx_[tx_pos_++];
                    if (tx_pos_ >= tx_len_) {
                        phase_ = Phase::WriteDone;
                        // DR пуст, поэтому TXE остаётся установленным:
                        // буферное прерывание снимаем, иначе оно «звенит»
                        // до самого STOP. Дальше нужен только BTF.
                        enableBufferInterrupt(false);
                    }
                    return;
                }
                phase_ = Phase::WriteDone;
                return;

            case Phase::WriteDataDma:
                // Байты отдал DMA. Прежде чем ставить STOP, нужно дождаться,
                // пока последний байт уйдёт из DR и сдвигового регистра (BTF),
                // и только потом снимать DMAEN — иначе последний байт теряется.
                if (!dma_done_) return;
                dma_done_ = false;
                I2C1->CR2 &= ~I2C_CR2_DMAEN;
                stopTxDma();
                tx_pos_ = tx_len_;
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

            case Phase::ReadDataDma:
                if (!dma_done_) return;
                dma_done_ = false;
                I2C1->CR2 &= ~I2C_CR2_DMAEN;
                stopRxDma();
                rx_pos_ = rx_len_ - 1u;         // DMA принял N-1 байт
                // Последний байт — программно и ровно так же, как без DMA:
                // NACK + STOP выставляются ДО его приёма (RM0008, рис. 246).
                I2C1->CR1 &= ~I2C_CR1_ACK;
                I2C1->CR1 |= I2C_CR1_STOP;
                enableBufferInterrupt(true);    // RXNE последнего байта
                phase_ = Phase::ReadLast;
                return;

            case Phase::ReadLast:
                if ((sr1 & I2C_SR1_RXNE) != 0u) {
                    if (rx_pos_ < rx_len_) {
                        rx_[rx_pos_++] = static_cast<uint8_t>(I2C1->DR & 0xFFu);
                    }
                }
                if ((sr1 & I2C_SR1_STOPF) != 0u) {
                    // STOPF на F103 снимается чтением SR1 с последующей
                    // записью в CR1; иначе он «звенит» в EV-прерывании.
                    (void)I2C1->SR1;
                    I2C1->CR1 = I2C1->CR1;
                }
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
        disableInterrupts();
        stopDma();
        phase_ = Phase::Idle;
        dma_done_ = false;

        async::completion* p = pending_;
        pending_ = nullptr;
        if (p == nullptr) return;
        if (st == Status::Ok) (void)p->post_ok(transferred);
        else                  (void)p->post_error(st);
    }

    Size completedBytes() const noexcept {
        return (mode_ == Mode::Read || mode_ == Mode::WriteRead) ? rx_pos_ : tx_pos_;
    }

    /// Включить прерывания так, как требует стратегия.
    ///
    ///   Polling — не включаем ничего: флаги читает pollOnIrq();
    ///   Async   — события, ошибки и буфер (TXE/RXNE);
    ///   Dma     — события и ошибки, но НЕ буфер: байты возит DMA, а
    ///             прерывание TXE/RXNE читало бы DR параллельно с ним
    ///             и «съедало» байты.
    void enableInterrupts() noexcept {
        switch (strategy_) {
            case async::Strategy::Polling:
                I2C1->CR2 &= ~(I2C_CR2_ITEVTEN | I2C_CR2_ITERREN | I2C_CR2_ITBUFEN);
                return;

            case async::Strategy::Dma:
                I2C1->CR2 &= ~I2C_CR2_ITBUFEN;
                I2C1->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITERREN;
                return;

            case async::Strategy::Async:
            default:
                I2C1->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITERREN | I2C_CR2_ITBUFEN;
                return;
        }
    }

    /// Буферное прерывание (TXE/RXNE) отдельно от событийного: в DMA-режиме
    /// оно включается только на «хвосте», когда последний байт читает драйвер.
    void enableBufferInterrupt(bool on) noexcept {
        if (strategy_ == async::Strategy::Polling) return;   // опрос и так работает
        if (on) I2C1->CR2 |= I2C_CR2_ITBUFEN;
        else    I2C1->CR2 &= ~I2C_CR2_ITBUFEN;
    }

    void disableInterrupts() noexcept {
        I2C1->CR2 &= ~(I2C_CR2_ITEVTEN | I2C_CR2_ITERREN | I2C_CR2_ITBUFEN);
    }

    // ─────────────────────────────── DMA ───────────────────────────────

    Status startTxDma(Size len) noexcept {
        DmaManager& mgr = DmaManager::instance();
        if (!mgr.is_initialized() && mgr.init() != Status::Ok) return Status::Fault;

        DmaChannel ch;
        if (mgr.acquire(kTxRequest, ch) != Status::Ok) {
            return Status::Busy;
        }

        const dma::BufferConfig buf{ const_cast<uint8_t*>(tx_), len, true };
        Status st = ch.configureMemToPeriph(kTxRequest, buf,
                                            dma::TransferSize::Byte,
                                            dma::Priority::High,
                                            dma::Trigger::Hardware,
                                            dma::BurstMode::Single);
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        st = ch.setCallback(&dmaTxCallback, this);
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        tx_dma_ = ch;
        st = ch.start();
        if (st != Status::Ok) { stopTxDma(); return st; }
        return Status::Ok;
    }

    Status startRxDma(Size len) noexcept {
        DmaManager& mgr = DmaManager::instance();
        if (!mgr.is_initialized() && mgr.init() != Status::Ok) return Status::Fault;

        DmaChannel ch;
        if (mgr.acquire(kRxRequest, ch) != Status::Ok) {
            return Status::Busy;
        }

        const dma::BufferConfig buf{ rx_, len, true };
        Status st = ch.configurePeriphToMem(kRxRequest, buf,
                                            dma::TransferSize::Byte,
                                            dma::Priority::High,
                                            dma::Trigger::Hardware,
                                            dma::BurstMode::Single);
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        st = ch.setCallback(&dmaRxCallback, this);
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        rx_dma_ = ch;
        st = ch.start();
        if (st != Status::Ok) { stopRxDma(); return st; }
        return Status::Ok;
    }

    void stopTxDma() noexcept {
        I2C1->CR2 &= ~I2C_CR2_DMAEN;
        if (tx_dma_.valid()) {
            // Гасим канал (EN=0) до освобождения ссылки: канал может
            // остаться за арбитром, и включённым его оставлять нельзя.
            (void)tx_dma_.stop();
            (void)DmaManager::instance().release(tx_dma_);
            tx_dma_ = DmaChannel{};
        }
    }

    void stopRxDma() noexcept {
        I2C1->CR2 &= ~(I2C_CR2_DMAEN | I2C_CR2_LAST);
        if (rx_dma_.valid()) {
            (void)rx_dma_.stop();
            (void)DmaManager::instance().release(rx_dma_);
            rx_dma_ = DmaChannel{};
        }
    }

    void stopDma() noexcept {
        stopTxDma();
        stopRxDma();
    }

    /// Колбэк DMA. Контекст — DmaCallbackContext (см. dma_manager.hpp):
    /// указатель на драйвер лежит в поле user, а не в самом аргументе.
    static void dmaTxCallback(void* ctx) noexcept { forwardDma(ctx); }
    static void dmaRxCallback(void* ctx) noexcept { forwardDma(ctx); }

    /// Опрос интерфейса для probe() в режиме Polling.
    static void probePoll(async::completion& /*c*/, void* /*arg*/) noexcept {
        instance().pollOnIrq();
    }

    static void forwardDma(void* ctx) noexcept {
        const auto* c = static_cast<const DmaCallbackContext*>(ctx);
        if (c == nullptr) return;
        auto* self = static_cast<I2cDriver1*>(c->user);
        if (self == nullptr) return;
        self->onDmaComplete(c->result);
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
    static void nvic_disable(uint32_t irqn) noexcept {
        NVIC->ICER[irqn >> 5u] = (1UL << (irqn & 0x1Fu));
    }

    Config cfg_{};
    async::Strategy strategy_ = kDefaultStrategy;
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

    DmaChannel tx_dma_{};
    DmaChannel rx_dma_{};
    /// DMA сообщил о завершении переноса данных.
    bool dma_done_ = false;
};

} // namespace embed::stm32
