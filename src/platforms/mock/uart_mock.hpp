// platforms/mock/uart_mock.hpp
#pragma once
// ---------------------------------------------------------------------------
// Mock-драйвер UART: та же логика, что у STM32, но «железо» эмулируется
// (см. mock_bus.hpp). Позволяет прогонять прикладной код и проверять
// связку с async IO без платы.
//
// Все три стратегии реализованы честно:
//   Polling — завершает pollOnIrq();
//   Async   — завершают onRxIrq()/onTxIrq() (тест/эмулятор их вызывает);
//   Dma     — завершает onDmaComplete().
// Приложение при этом пишет один и тот же линейный код.
// ---------------------------------------------------------------------------
#include "../../os/async/async_port.hpp"   // async::Strategy
#include "../../os/interfaces/iuart.hpp"
#include "mock_bus.hpp"

namespace embed::mock {

/// Драйвер mock-UART.
///
/// Strategy выбирается шаблоном, чтобы один и тот же тест прогонялся
/// по всем трём путям и доказывал, что результат от режима не зависит.
template <async::Strategy Strat = async::Strategy::Polling>
class UartMock : public IUart<UartMock<Strat>> {
public:
    static constexpr bool            kHasDma   = (Strat == async::Strategy::Dma);
    static constexpr async::Strategy kStrategy = Strat;
    static constexpr Size            kMaxChunk = 64u;   // как у реального FIFO

    using Base    = IUart<UartMock<Strat>>;
    using Parity  = typename Base::Parity;
    using StopBits = typename Base::StopBits;

    /// Конфигурация = базовая + запрошенная стратегия.
    ///
    /// У mock стратегия задана шаблоном (это «железо»), поэтому поле только
    /// проверяется: запрос другой стратегии — честный NotSupported, а не
    /// молчаливая подмена. На STM32 это же поле РЕАЛЬНО выбирает режим
    /// (uart_stm32.hpp), поэтому прикладной код одинаков для обеих платформ.
    struct Config : Base::Config {
        async::Strategy strategy = Strat;

        Config() = default;
    };

    static UartMock& instance() noexcept {
        static UartMock d;
        return d;
    }

    /// Стратегия драйвера: читается async::UartPort и приложением.
    static constexpr async::Strategy strategy() noexcept { return Strat; }

    UartBus& bus() noexcept { return g_uart_bus; }

    // ─────────────────────────── Настройка ───────────────────────────

    Status configure(const Config& cfg) noexcept {
        if (cfg.strategy != Strat) return Status::NotSupported;   // не наше «железо»
        cfg_ = cfg;
        if (cfg.baudRate == 0u) return Status::InvalidArg;
        configured_ = true;
        return Status::Ok;
    }

    Status open() noexcept  { opened_ = true;  return Status::Ok; }
    Status close() noexcept { opened_ = false; return Status::Ok; }

    Status ioctl(unsigned /*request*/, void* /*arg*/) noexcept {
        return Status::NotSupported;
    }

    // ─────────────────────── Асинхронные операции ───────────────────────

    /// Привязать объект завершения.
    ///
    /// Заменяет предыдущий, а не отказывает: порт владеет объектом всё
    /// время операции, поэтому «старый» указатель всегда либо уже
    /// отработал, либо относится к прерванной операции. Отказ здесь
    /// оставлял бы в драйвере висячий указатель на чужой объект.
    Status setCompletion(async::completion& token) noexcept {
        pending_ = &token;
        return Status::Ok;
    }

    async::completion* pendingCompletion() noexcept { return pending_; }

    Status startWrite(const void* data, Size len) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (data == nullptr || len == 0u) return Status::InvalidArg;
        if (tx_busy_) return Status::Busy;

        tx_     = static_cast<const uint8_t*>(data);
        tx_len_ = len;
        tx_pos_ = 0u;
        tx_busy_ = true;

        // Мгновенная передача: завершаем сразу, как это делает DMA с
        // нулевой задержкой или FIFO, вместивший весь буфер.
        if (bus().tx_ticks_per_byte == 0u) {
            if (bus().fail_next_tx) {
                bus().fail_next_tx = false;
                tx_busy_ = false;
                return finish(Status::HardwareError, 0u);
            }
            for (Size i = 0u; i < len; ++i) bus().pushTx(tx_[i]);
            tx_pos_ = len;
            tx_busy_ = false;
            return finish(Status::Ok, len);
        }
        return Status::Ok;      // завершение придёт по опросу/прерыванию
    }

    Status startRead(void* buffer, Size capacity) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (buffer == nullptr || capacity == 0u) return Status::InvalidArg;
        if (rx_busy_) return Status::Busy;

        rx_     = static_cast<uint8_t*>(buffer);
        rx_cap_ = capacity;
        rx_pos_ = 0u;
        rx_busy_ = true;

        if (bus().fail_next_rx) {
            bus().fail_next_rx = false;
            rx_busy_ = false;
            return finish(Status::HardwareError, 0u);
        }

        // Забираем столько, сколько есть (неблокирующее чтение).
        pumpRead();
        if (!rx_busy_) return Status::Ok;
        return Status::Ok;      // ждём данные
    }

    Status abort() noexcept {
        tx_busy_ = false;
        rx_busy_ = false;
        if (pending_ != nullptr) {
            async::completion* p = pending_;
            pending_ = nullptr;
            (void)p->post_error(Status::Aborted);
        }
        return Status::Ok;
    }

    // ─────────────────── Точки входа опроса/прерываний ───────────────────

    /// Обслуживание: опрос флагов (стратегия Polling).
    void pollOnIrq() noexcept {
        (void)bus().tick();
        pumpRead();
        pumpWrite();
    }

    void onRxIrq() noexcept { pumpRead(); }
    void onTxIrq() noexcept { pumpWrite(); }

    void onErrorIrq(Status st) noexcept {
        tx_busy_ = rx_busy_ = false;
        (void)finish(st, 0u);
    }

    /// Колбэк «DMA»: завершение приходит извне, как в реальном драйвере.
    void onDmaComplete(Size transferred) noexcept {
        (void)finish(Status::Ok, transferred);
    }

    /// Сколько байт «в сдвиговом регистре» — используется тестами,
    /// чтобы проверить завершение передачи.
    void onShiftingEmpty() noexcept {
        if (tx_busy_) {
            tx_busy_ = false;
            (void)finish(Status::Ok, tx_pos_);
        }
    }

    bool configured() const noexcept { return configured_; }
    bool opened()     const noexcept { return opened_; }

private:
    /// Дочитываем всё, что есть в приёмнике.
    ///
    /// Завершение наступает в двух случаях:
    ///   * буфер заполнен полностью;
    ///   * пришли данные и линия замолчала (bus().rx_idle) — так работает
    ///     обычный UART и так же ведёт себя HAL по прерыванию IDLE.
    void pumpRead() noexcept {
        if (!rx_busy_ || pending_ == nullptr) return;

        uint8_t b = 0u;
        while (rx_pos_ < rx_cap_ && bus().popByte(b)) {
            rx_[rx_pos_++] = b;
        }

        const bool full = (rx_pos_ >= rx_cap_);
        const bool idle_done = bus().rx_idle && (rx_pos_ > 0u);
        if (!full && !idle_done) return;      // данных ещё нет — ждём

        rx_busy_ = false;
        (void)finish(Status::Ok, rx_pos_);
    }

    /// Продвигаем передачу: каждый вызов «железа» отправляет байт.
    void pumpWrite() noexcept {
        if (!tx_busy_ || pending_ == nullptr) return;
        if (bus().tx_ticks_per_byte == 0u) return;   // уже завершено

        bus().tick();
        bus().pushTx(tx_[tx_pos_]);
        ++tx_pos_;
        if (tx_pos_ >= tx_len_) {
            tx_busy_ = false;
            (void)finish(Status::Ok, tx_pos_);
        }
    }

    Status finish(Status st, Size n) noexcept {
        async::completion* p = pending_;
        pending_ = nullptr;
        if (p == nullptr) return st;
        if (st == Status::Ok) (void)p->post_ok(n);
        else                  (void)p->post_error(st);
        return Status::Ok;
    }

    Config cfg_{};
    async::completion* pending_ = nullptr;

    const uint8_t* tx_ = nullptr;
    Size tx_len_ = 0u;
    Size tx_pos_ = 0u;
    bool tx_busy_ = false;

    uint8_t* rx_ = nullptr;
    Size rx_cap_ = 0u;
    Size rx_pos_ = 0u;
    bool rx_busy_ = false;

    bool configured_ = false;
    bool opened_ = false;
};

/// Готовые псевдонимы для тестов и приложений на mock-платформе.
using UartMockPolling = UartMock<async::Strategy::Polling>;
using UartMockAsync   = UartMock<async::Strategy::Async>;
using UartMockDma     = UartMock<async::Strategy::Dma>;

} // namespace embed::mock
