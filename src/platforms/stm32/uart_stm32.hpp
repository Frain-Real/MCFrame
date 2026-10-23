// platforms/stm32/uart_stm32.hpp
#pragma once
// ---------------------------------------------------------------------------
// Драйвер USART1/2/3 (STM32F103) с ТРЕМЯ РЕАЛЬНЫМИ стратегиями.
//
//   Polling — автомат двигает pollOnIrq(), который async::AsyncPort вызывает
//             в цикле ожидания; RXNEIE/TXEIE/TCIE/IDLEIE и NVIC ВЫКЛЮЧЕНЫ,
//             DMA не участвует;
//   Async   — прерывания USARTx (RXNE/TXE/TC/IDLE) через USARTx_IRQHandler;
//   Dma     — DMA1: каналы USART1 Ch4/5, USART2 Ch7/6, USART3 Ch2/1.
//             а только ждёт завершения. Приём завершается по IDLE (пачка
//             закончилась) или по заполнению буфера, передача — по TC, то
//             есть когда последний байт вышел из сдвигового регистра.
//
// АВТОМАТ ОДИН НА ВСЕ РЕЖИМЫ: handleIrq() читает SR и двигает обмен.
// Стратегия определяет лишь то, КТО его вызывает и кто завершает операцию.
// Раздельные «режим прерываний» и «режим DMA» почти неизбежно расходятся —
// здесь этого не может быть по построению.
//
// ВЫБОР СТРАТЕГИИ
//   * Config::strategy            — при configure();
//   * usePolling()/useInterrupts()/useDma() — смена на лету (нужен простой);
//   * strategy()                  — что выбрано сейчас (это же читает порт).
//
// Проверка: tests/host/test_stm32_hal.cpp (USART1) и
// tests/host/test_uart_multi.cpp (USART2/3) — те же три стратегии на модели
// регистров. На модели прогоняются
// те же автоматы, что уходят в прошивку.
// ---------------------------------------------------------------------------
#include "../../os/async/async_port.hpp"   // async::Strategy
#include "../../os/interfaces/iuart.hpp"
#include "../../os/interfaces/dma_manager.hpp"
#include "../../os/config/dma_config.hpp"

#include "stm32_regs.hpp"

namespace embed::stm32 {

/// Номера каналов DMA для каждого USART (по RM0008).
struct UartDmaMap {
    config::DmaRequest tx;
    config::DmaRequest rx;
};

// ─────────────────────────── Общие traits USART ─────────────────────────────
//
// Драйвер ОДИН на все три USART: отличается только база регистров, шина
// тактирования (APB2/APB1), номер IRQ, частота и каналы DMA. Всё остальное —
// один и тот же автомат, поэтому USART2/3 получают ровно те же стратегии
// Polling/Async/Dma и те же гарантии, что USART1.
struct Uart1Traits {
    static auto* reg() noexcept { return USART1; }
    static constexpr uint32_t irq = static_cast<uint32_t>(USART1_IRQn);
    static constexpr bool     apb2 = true;
    static constexpr uint32_t rcc_bit = RCC_APB2ENR_USART1EN;
    static constexpr uint32_t pclk = 72000000u;
    static constexpr config::DmaRequest kTxRequest = config::DmaRequest::Usart1Tx;
    static constexpr config::DmaRequest kRxRequest = config::DmaRequest::Usart1Rx;
};
struct Uart2Traits {
    static auto* reg() noexcept { return USART2; }
    static constexpr uint32_t irq = static_cast<uint32_t>(USART2_IRQn);
    static constexpr bool     apb2 = false;
    static constexpr uint32_t rcc_bit = RCC_APB1ENR_USART2EN;
    static constexpr uint32_t pclk = 36000000u;
    static constexpr config::DmaRequest kTxRequest = config::DmaRequest::Usart2Tx;
    static constexpr config::DmaRequest kRxRequest = config::DmaRequest::Usart2Rx;
};
struct Uart3Traits {
    static auto* reg() noexcept { return USART3; }
    static constexpr uint32_t irq = static_cast<uint32_t>(USART3_IRQn);
    static constexpr bool     apb2 = false;
    static constexpr uint32_t rcc_bit = RCC_APB1ENR_USART3EN;
    static constexpr uint32_t pclk = 36000000u;
    static constexpr config::DmaRequest kTxRequest = config::DmaRequest::Usart3Tx;
    static constexpr config::DmaRequest kRxRequest = config::DmaRequest::Usart3Rx;
};

template <typename Traits>
class UartDriver : public IUart<UartDriver<Traits>> {
public:
    static constexpr bool            kHasDma   = true;
    static constexpr Size            kMaxChunk = 0u;   // без ограничения блока
    /// Стратегия по умолчанию: прерывания. DMA включается явно — он требует
    /// свободного канала и настроенного контроллера.
    static constexpr async::Strategy kDefaultStrategy = async::Strategy::Async;

    /// Запросы DMA этого интерфейса (RM0008). Нужны в том числе арбитру
    /// каналов (dma_arbiter.hpp): он по ним понимает, какие каналы займёт
    /// работа и с кем этот интерфейс делит канал.
    static constexpr config::DmaRequest kTxRequest = Traits::kTxRequest;
    static constexpr config::DmaRequest kRxRequest = Traits::kRxRequest;

    using Base     = IUart<UartDriver<Traits>>;
    using Parity   = typename Base::Parity;
    using StopBits = typename Base::StopBits;

    /// Конфигурация = базовая + выбор стратегии.
    ///
    /// Конструктор от Base::Config нужен, чтобы вызов configure() через
    /// интерфейс (IUart<UartDriver1>&) не срезал поле strategy: базовый
    /// Config превращается в этот с умолчаниями.
    struct Config : Base::Config {
        async::Strategy strategy = kDefaultStrategy;

        Config() = default;
        Config(const typename Base::Config& base) noexcept : Base::Config(base) {}
    };

    static UartDriver& instance() noexcept {
        static UartDriver d;
        return d;
    }

    /// Текущая стратегия (её читает async::UartPort, чтобы понять,
    /// нужно ли опрашивать интерфейс в цикле ожидания).
    static async::Strategy strategy() noexcept { return instance().strategy_; }

    // ─────────────────────────── Настройка ───────────────────────────

    Status configure(const Config& cfg) noexcept {
        if (cfg.baudRate == 0u) return Status::InvalidArg;
        // Смена настроек (в т.ч. стратегии) на середине операции недопустима.
        if (tx_busy_ || rx_busy_) return Status::Busy;

        cfg_ = cfg;

        if constexpr (Traits::apb2) {
            RCC->APB2ENR |= Traits::rcc_bit;
        } else {
            RCC->APB1ENR |= Traits::rcc_bit;
        }

        // 8 бит данных, 1 стоп-бит, без чётности — базовый режим.
        uint32_t cr1 = USART_CR1_TE | USART_CR1_RE;
        switch (cfg.parity) {
            case Parity::Even: cr1 |= USART_CR1_PCE; break;
            case Parity::Odd:  cr1 |= USART_CR1_PCE | USART_CR1_PS; break;
            case Parity::None: break;
        }
        U()->CR1 = cr1;
        U()->CR2 = (cfg.stopBits == StopBits::Two) ? USART_CR2_STOP_1 : 0u;
        // Аппаратное управление потоком живёт в CR3, а не в CR1.
        U()->CR3 = cfg.flowControl
                    ? (USART_CR3_RTSE | USART_CR3_CTSE)
                    : 0u;

        const Status br = setBaudRate(cfg.baudRate);
        if (br != Status::Ok) return br;

        U()->CR1 |= USART_CR1_UE;      // включить USART

        // Стратегия применяется последней: она включает/снимает прерывания,
        // а значит должна видеть уже настроенный периферийный блок.
        return selectStrategy(cfg.strategy);
    }

    Status open() noexcept {
        U()->CR1 |= USART_CR1_UE;
        return Status::Ok;
    }

    Status close() noexcept {
        (void)abort();                    // снять операции и освободить DMA
        U()->CR1 &= ~USART_CR1_UE;
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
        // USART1 — APB2 (72 МГц), USART2/3 — APB1 (36 МГц) при SYSCLK 72 МГц.
        const uint32_t pclk = Traits::pclk;
        U()->BRR = (pclk + baud / 2u) / baud;
        return Status::Ok;
    }

    // ─────────────────────── Выбор стратегии ───────────────────────

    /// Переключить стратегию. Требуется простой: способ завершения операции
    /// нельзя менять на середине передачи — иначе байты «потеряются» между
    /// двумя механизмами.
    Status selectStrategy(async::Strategy s) noexcept {
        if (s == async::Strategy::Dma && !kHasDma) return Status::NotSupported;
        if (tx_busy_ || rx_busy_) return Status::Busy;

        stopDma();                        // каналы освобождаются всегда

        U()->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_TXEIE |
                         USART_CR1_TCIE   | USART_CR1_IDLEIE);

        // NVIC нужен прерываниям и DMA: у DMA-передачи завершение приходит
        // по TC из USART1_IRQHandler, а не из обработчика канала DMA.
        // В Polling он ВЫКЛЮЧЕН — это и есть отличие режима опроса.
        if (s == async::Strategy::Polling) {
            nvic_disable(static_cast<uint32_t>(Traits::irq));
        } else {
            nvic_enable(static_cast<uint32_t>(Traits::irq));
        }

        strategy_ = s;
        return Status::Ok;
    }

    /// Опрос регистров: ни прерываний, ни DMA.
    Status usePolling() noexcept { return selectStrategy(async::Strategy::Polling); }
    /// Прерывания RXNE/TXE/TC/IDLE.
    Status useInterrupts() noexcept { return selectStrategy(async::Strategy::Async); }
    /// DMA1 Ch4 (TX) / Ch5 (RX).
    Status useDma() noexcept { return selectStrategy(async::Strategy::Dma); }

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

        tx_buf_        = static_cast<const uint8_t*>(data);
        tx_len_        = len;
        tx_pos_        = 0u;
        tx_all_queued_ = false;
        tx_busy_       = true;

        switch (strategy_) {
            case async::Strategy::Dma: {
                // Канал занят или нет контроллера — честно сообщаем Busy,
                // а не делаем вид, что передача пошла.
                const Status st = startTxDma(len);
                if (st != Status::Ok) {
                    tx_busy_ = false;
                    return st;
                }
                return Status::Ok;
            }
            case async::Strategy::Async:
                // Байты отдаёт прерывание TXE: DR пуст уже сейчас, поэтому
                // первое прерывание придёт сразу после включения TXEIE.
                U()->CR1 |= USART_CR1_TXEIE;
                return Status::Ok;

            case async::Strategy::Polling:
            default:
                // Ничего не включаем: байты отдаст pollOnIrq().
                return Status::Ok;
        }
    }

    Status startRead(void* buffer, Size capacity) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (buffer == nullptr || capacity == 0u) return Status::InvalidArg;
        if (rx_busy_) return Status::Busy;

        rx_buf_ = static_cast<uint8_t*>(buffer);
        rx_cap_ = capacity;
        rx_pos_ = 0u;
        rx_busy_ = true;

        // Залипший IDLE от прошлой операции поднял бы прерывание сразу и
        // «звонил» бы до первого байта (обработчику нечего завершать).
        // Снимаем до включения IDLEIE — тогда чтение DR ничего не украдёт.
        clearIdleFlag();

        switch (strategy_) {
            case async::Strategy::Dma: {
                const Status st = startRxDma(capacity);
                if (st != Status::Ok) {
                    rx_busy_ = false;
                    return st;
                }
                return Status::Ok;
            }
            case async::Strategy::Async:
                // RXNE — байты, IDLE — конец пачки (иначе read() с буфером
                // больше пакета ждал бы заполнения буфера или таймаута).
                U()->CR1 |= USART_CR1_RXNEIE | USART_CR1_IDLEIE;
                return Status::Ok;

            case async::Strategy::Polling:
            default:
                return Status::Ok;
        }
    }

    Status abort() noexcept {
        U()->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_TXEIE |
                         USART_CR1_TCIE | USART_CR1_IDLEIE);
        stopDma();
        tx_busy_       = false;
        rx_busy_       = false;
        tx_all_queued_ = false;

        // Прерванная операция обязана завершиться, иначе задача, ждущая
        // completion, проснётся только по таймауту.
        if (pending_ != nullptr) {
            async::completion* p = pending_;
            pending_ = nullptr;
            (void)p->post_error(Status::Aborted);
        }
        return Status::Ok;
    }

    // ─────────────────── Точки входа прерываний/опроса ───────────────────

    /// Обработчик USART1_IRQHandler. Та же функция обслуживает и стратегию
    /// Polling (её вызывает pollOnIrq() из цикла ожидания порта).
    void onIrq() noexcept { handleIrq(); }
    void onRxIrq() noexcept { handleIrq(); }
    void onTxIrq() noexcept { handleIrq(); }
    void pollOnIrq() noexcept { handleIrq(); }

    void onErrorIrq(Status st) noexcept { failOperation(st); }

    /// Вызывается из обработчика DMA (см. dmaTxCallback/dmaRxCallback).
    void onTxDmaComplete(Size n) noexcept {
        (void)n;
        tx_all_queued_ = true;
        U()->CR1 |= USART_CR1_TCIE;
        handleIrq();
    }
    void onRxDmaComplete(Size n) noexcept { finishRx(n); }

    /// Все байты отданы в DR (драйвером или DMA) — осталось дождаться TC.
    bool txQueued() const noexcept { return tx_all_queued_; }
    /// Приём обслуживает DMA (драйвер не читает DR).
    bool rxByDma() const noexcept { return rx_dma_active_; }

    static auto* U() noexcept { return Traits::reg(); }

private:
    /// Сколько байт обработчик вычитывает из DR за один вызов.
    static constexpr unsigned kRxBurst = 64u;

    /// Единственная точка, двигающая обмен.
    void handleIrq() noexcept {
        const uint32_t sr = U()->SR;

        // Ошибки раньше событий: пока ORE не снят, приёмный регистр
        // заблокирован, и «нормальные» ветки всё равно ничего не сделают.
        if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0u) {
            (void)U()->SR;
            (void)U()->DR;                 // ORE снимается чтением DR
            failOperation(Status::HardwareError);
            return;
        }

        // ── Приём: RXNE обслуживает драйвер, только если нет DMA-RX ──
        // Вычитываем ВСЁ, что уже пришло: если один вызов обработчика застал
        // несколько байт (ядро было занято), оставлять их в DR нельзя —
        // следующий байт даст ORE. Ограничение сверху защищает от
        // зацикливания, если RXNE почему-то не снимается.
        if (!rx_dma_active_) {
            for (unsigned guard = 0u; guard < kRxBurst; ++guard) {
                if ((U()->SR & USART_SR_RXNE) == 0u) break;

                const uint8_t b = static_cast<uint8_t>(U()->DR & 0xFFu);
                if (rx_busy_) {
                    if (rx_pos_ < rx_cap_) rx_buf_[rx_pos_++] = b;
                    if (rx_pos_ >= rx_cap_) {
                        finishRx(rx_pos_);
                        return;
                    }
                }
            }
        }

        // ── Конец пачки: линия замолчала (IDLE) ──
        // Проверяется ПОСЛЕ вычитывания DR: если пришёл и последний байт,
        // и IDLE, операция должна завершиться уже с ним в буфере.
        // Так завершается приём и на железе, и в mock-модели (bus().rx_idle):
        // read(buf, 64) на 4 принятых байтах возвращает 4, а не ждёт 64.
        if ((sr & USART_SR_IDLE) != 0u) {
            (void)U()->SR;
            // IDLE снимается чтением SR и DR, но при DMA-приёме DR читает
            // DMA: чтение здесь «украло» бы у него уже принятый байт.
            // Операцию всё равно завершаем — дальше IDLEIE будет снят.
            if (!rx_dma_active_) (void)U()->DR;
            if (rx_busy_) {
                const Size got = rxReceived();
                if (got > 0u) {
                    finishRx(got);
                    return;
                }
            }
        }

        // ── Передача: байты в DR кладёт драйвер, только если нет DMA-TX ──
        if (!tx_dma_active_ && tx_busy_ && !tx_all_queued_ &&
            tx_pos_ < tx_len_ && (sr & USART_SR_TXE) != 0u) {
            U()->DR = tx_buf_[tx_pos_++];
            if (tx_pos_ >= tx_len_) {
                tx_all_queued_ = true;
                if (strategy_ == async::Strategy::Async) {
                    // Все байты отданы: TXE больше не нужен, ждём TC.
                    U()->CR1 &= ~USART_CR1_TXEIE;
                    U()->CR1 |=  USART_CR1_TCIE;
                }
            }
        }

        // TC читается ЗАНОВО: запись в DR выше сбрасывает TC, а sr снят до
        // неё. Со старым значением передача «завершилась» бы в тот момент,
        // когда последний байт ещё не вышел из сдвигового регистра.
        const uint32_t sr_now = U()->SR;
        if (tx_busy_ && tx_all_queued_ && (sr_now & USART_SR_TC) != 0u) {
            finishTx(tx_len_);
        }
    }

    /// Снять флаг IDLE: по RM0008 он снимается чтением SR, затем DR.
    /// DR читается только если флаг действительно установлен.
    void clearIdleFlag() noexcept {
        if ((U()->SR & USART_SR_IDLE) == 0u) return;
        (void)U()->SR;
        (void)U()->DR;
    }

    /// Сколько байт принято: у DMA это capacity - CNDTR, у драйвера — rx_pos_.
    Size rxReceived() const noexcept {
        if (!rx_dma_active_ || !rx_dma_.valid()) return rx_pos_;
        const Size left = platform_dma_bytes_remaining(rx_dma_.id());
        return (rx_cap_ > left) ? (rx_cap_ - left) : 0u;
    }

    void finishTx(Size n) noexcept {
        stopTxDma();
        U()->CR1 &= ~(USART_CR1_TXEIE | USART_CR1_TCIE);
        tx_busy_       = false;
        tx_all_queued_ = false;
        post(Status::Ok, n);
    }

    void finishRx(Size n) noexcept {
        stopRxDma();
        U()->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_IDLEIE);
        rx_busy_ = false;
        post(Status::Ok, n);
    }

    /// Ошибка приёма/передачи: снимаем операцию и сообщаем статус.
    void failOperation(Status st) noexcept {
        U()->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_TXEIE |
                         USART_CR1_TCIE   | USART_CR1_IDLEIE);
        stopDma();
        tx_busy_       = false;
        rx_busy_       = false;
        tx_all_queued_ = false;
        post(st, 0u);
    }

    void post(Status st, Size n) noexcept {
        async::completion* p = pending_;
        pending_ = nullptr;
        if (p == nullptr) return;
        if (st == Status::Ok) (void)p->post_ok(n);
        else                  (void)p->post_error(st);
    }

    // ─────────────────────────────── DMA ───────────────────────────────

    Status startTxDma(Size len) noexcept {
        DmaManager& mgr = DmaManager::instance();
        if (!mgr.is_initialized() && mgr.init() != Status::Ok) return Status::Fault;

        DmaChannel ch;
        if (mgr.acquire(kTxRequest, ch) != Status::Ok) {
            return Status::Busy;
        }

        const dma::BufferConfig buf{
            const_cast<uint8_t*>(tx_buf_),
            len,
            true,
        };
        Status st = ch.configureMemToPeriph(kTxRequest, buf,
                                            dma::TransferSize::Byte,
                                            dma::Priority::Medium,
                                            dma::Trigger::Hardware,
                                            dma::BurstMode::Single);
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        st = ch.setCallback(&dmaTxCallback, this);
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        tx_dma_        = ch;
        tx_dma_active_ = true;

        U()->CR1 &= ~USART_CR1_TCIE;   // TC прошлой передачи не наш
        U()->CR3 |= USART_CR3_DMAT;

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

        const dma::BufferConfig buf{ rx_buf_, len, true };
        Status st = ch.configurePeriphToMem(kRxRequest, buf,
                                            dma::TransferSize::Byte,
                                            dma::Priority::Medium,
                                            dma::Trigger::Hardware,
                                            dma::BurstMode::Single);
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        st = ch.setCallback(&dmaRxCallback, this);
        if (st != Status::Ok) { (void)mgr.release(ch); return st; }

        rx_dma_        = ch;
        rx_dma_active_ = true;

        U()->CR3 |= USART_CR3_DMAR;
        // IDLE нужен и на DMA-приёме: буфер обычно больше пакета, и без
        // признака «линия замолчала» операция ждала бы заполнения буфера.
        U()->CR1 |= USART_CR1_IDLEIE;

        st = ch.start();
        if (st != Status::Ok) { stopRxDma(); return st; }
        return Status::Ok;
    }

    void stopTxDma() noexcept {
        U()->CR3 &= ~USART_CR3_DMAT;
        U()->CR1 &= ~USART_CR1_TCIE;
        tx_dma_active_ = false;
        if (tx_dma_.valid()) {
            // Сначала гасим канал (EN=0), потом отпускаем СВОЮ ссылку:
            // канал может остаться за арбитром, и включённым его оставлять
            // нельзя — иначе он среагирует на запрос другой периферии.
            (void)tx_dma_.stop();
            (void)DmaManager::instance().release(tx_dma_);
            tx_dma_ = DmaChannel{};
        }
    }

    void stopRxDma() noexcept {
        U()->CR3 &= ~USART_CR3_DMAR;
        rx_dma_active_ = false;
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
    static void dmaTxCallback(void* ctx) noexcept {
        const auto* c = static_cast<const DmaCallbackContext*>(ctx);
        if (c == nullptr) return;
        auto* self = static_cast<UartDriver<Traits>*>(c->user);
        if (self == nullptr) return;
        if (c->result != Status::Ok) {
            self->failOperation(Status::HardwareError);
            return;
        }
        // DMA закончился, но последний байт ещё может оставаться в сдвиговом
        // регистре, поэтому завершать операцию рано: ждём прерывание TC.
        self->tx_all_queued_ = true;
        U()->CR1 |= USART_CR1_TCIE;
        self->handleIrq();
    }

    static void dmaRxCallback(void* ctx) noexcept {
        const auto* c = static_cast<const DmaCallbackContext*>(ctx);
        if (c == nullptr) return;
        auto* self = static_cast<UartDriver<Traits>*>(c->user);
        if (self == nullptr) return;
        if (c->result != Status::Ok) {
            self->finishRxError(Status::HardwareError);
            return;
        }
        // Буфер заполнен полностью: остаток CNDTR равен нулю.
        self->finishRx(self->rxReceived());
    }

    void finishRxError(Status st) noexcept {
        stopRxDma();
        U()->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_IDLEIE);
        rx_busy_ = false;
        post(st, 0u);
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
    bool tx_dma_active_ = false;
    bool rx_dma_active_ = false;
    /// Все байты отданы в DR: осталось дождаться TC.
    bool tx_all_queued_ = false;
};

using UartDriver1 = UartDriver<Uart1Traits>;
using UartDriver2 = UartDriver<Uart2Traits>;
using UartDriver3 = UartDriver<Uart3Traits>;

} // namespace embed::stm32
