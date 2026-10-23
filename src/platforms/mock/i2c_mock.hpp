// platforms/mock/i2c_mock.hpp
#pragma once
// ---------------------------------------------------------------------------
// Mock-драйвер I2C. Как и UART, повторяет структуру реального драйвера:
// те же setCompletion/startWrite/startRead/startWriteRead, та же логика
// завершения. «Железо» эмулируется шиной из mock_bus.hpp, поэтому
// проверяются и NACK отсутствующего устройства, и ошибки шины.
// ---------------------------------------------------------------------------
#include "../../os/async/async_port.hpp"   // async::Strategy
#include "../../os/interfaces/ii2c.hpp"
#include "mock_bus.hpp"

namespace embed::mock {

template <async::Strategy Strat = async::Strategy::Polling>
class I2cMock : public II2c<I2cMock<Strat>> {
public:
    static constexpr bool            kHasDma   = (Strat == async::Strategy::Dma);
    static constexpr async::Strategy kStrategy = Strat;
    static constexpr Size            kMaxChunk = 255u;

    using Base    = II2c<I2cMock<Strat>>;
    using Address = I2cAddress;
    using Speed   = typename Base::Speed;
    using MasterMode = typename Base::MasterMode;

    /// Конфигурация = базовая + запрошенная стратегия.
    ///
    /// У mock стратегия задана шаблоном (это «железо»), поэтому поле только
    /// проверяется: запрос другой стратегии — честный NotSupported, а не
    /// молчаливая подмена. На STM32 то же поле РЕАЛЬНО выбирает режим
    /// (i2c_stm32.hpp), поэтому прикладной код одинаков для обеих платформ.
    struct Config : Base::Config {
        async::Strategy strategy = Strat;

        Config() = default;
        Config(const typename Base::Config& base) noexcept : Base::Config(base) {}
    };

    static I2cMock& instance() noexcept {
        static I2cMock d;
        return d;
    }

    /// Стратегия драйвера: читается async::I2cPort и приложением.
    static constexpr async::Strategy strategy() noexcept { return Strat; }

    I2cBus& bus() noexcept { return g_i2c_bus; }

    // ─────────────────────────── Настройка ───────────────────────────

    Status configure(const Config& cfg) noexcept {
        if (cfg.strategy != Strat) return Status::NotSupported;   // не наше «железо»
        if (cfg.peripheralClockHz == 0u) return Status::InvalidArg;
        if (cfg.speed == Speed::FastPlus1M) return Status::NotSupported;
        cfg_ = cfg;
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

    Status startWrite(Address addr, const void* data, Size len) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (data == nullptr || len == 0u || !addr.valid()) return Status::InvalidArg;
        if (len > kMaxChunk) return Status::InvalidArg;

        Size done = 0u;
        const Status st = bus().transfer(addr.value, /*read*/ false,
                                        static_cast<const uint8_t*>(data), len,
                                        nullptr, 0u, done);
        return finish(st, done);
    }

    Status startRead(Address addr, void* buffer, Size len) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (buffer == nullptr || len == 0u || !addr.valid()) return Status::InvalidArg;
        if (len > kMaxChunk) return Status::InvalidArg;

        Size done = 0u;
        const Status st = bus().transfer(addr.value, /*read*/ true,
                                        nullptr, 0u,
                                        static_cast<uint8_t*>(buffer), len, done);
        return finish(st, done);
    }

    Status startWriteRead(Address addr, const void* tx, Size tx_len,
                          void* rx, Size rx_len) noexcept {
        if (pending_ == nullptr) return Status::Unavailable;
        if (tx == nullptr || tx_len == 0u || rx == nullptr || rx_len == 0u) {
            return Status::InvalidArg;
        }
        if (!addr.valid() || tx_len > kMaxChunk || rx_len > kMaxChunk) {
            return Status::InvalidArg;
        }

        // Фаза записи (адрес регистра), затем фаза чтения — как в железе,
        // где между фазами стоит повторный START без STOP.
        Size done = 0u;
        Status st = bus().transfer(addr.value, /*read*/ false,
                                  static_cast<const uint8_t*>(tx), tx_len,
                                  nullptr, 0u, done);
        if (st != Status::Ok) return finish(st, 0u);

        Size rx_done = 0u;
        st = bus().transfer(addr.value, /*read*/ true,
                            nullptr, 0u,
                            static_cast<uint8_t*>(rx), rx_len, rx_done);
        return finish(st, rx_done);
    }

    /// Проверка наличия устройства: пустая операция записи.
    Status probe(Address addr, Timeout /*timeout*/) noexcept {
        if (!addr.valid()) return Status::InvalidArg;

        // Объект завершения обязан пережить вызов: драйвер хранит на него
        // указатель. Локальная переменная на стеке дала бы обращение
        // к уничтоженному объекту (это ловил ASan).
        static async::completion local;
        local.reset(async::WaitPolicy::Forever);
        if (setCompletion(local) != Status::Ok) return Status::Busy;

        Size done = 0u;
        const Status st = bus().transfer(addr.value, /*read*/ false,
                                        nullptr, 0u, nullptr, 0u, done);
        (void)finish(st, 0u);
        return st;
    }

    Status abort() noexcept {
        if (pending_ != nullptr) {
            async::completion* p = pending_;
            pending_ = nullptr;
            (void)p->post_error(Status::Aborted);
        }
        return Status::Ok;
    }

    Status recover() noexcept {
        bus().bus_stuck = false;
        return Status::Ok;
    }

    // ─────────────────── Точки входа опроса/прерываний ───────────────────

    /// В mock-модели обмен завершается внутри start*, поэтому опрос
    /// ничего не делает: завершение уже отправлено.
    void pollOnIrq() noexcept {}
    void onCompleteIrq() noexcept {}
    void onErrorIrq(Status st) noexcept { (void)finish(st, 0u); }

    bool configured() const noexcept { return configured_; }
    bool opened()     const noexcept { return opened_; }

private:
    Status finish(Status st, Size n) noexcept {
        async::completion* p = pending_;
        pending_ = nullptr;
        if (p == nullptr) return st;
        if (st == Status::Ok) (void)p->post_ok(n);
        else                  (void)p->post_error(st);
        return Status::Ok;      // операция запущена и уже завершена
    }

    Config cfg_{};
    async::completion* pending_ = nullptr;
    bool configured_ = false;
    bool opened_ = false;
};

using I2cMockPolling = I2cMock<async::Strategy::Polling>;
using I2cMockAsync   = I2cMock<async::Strategy::Async>;
using I2cMockDma     = I2cMock<async::Strategy::Dma>;

} // namespace embed::mock
