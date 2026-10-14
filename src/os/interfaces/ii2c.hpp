// ii2c.hpp
#pragma once

#include "iinterface.hpp"
#include "../async/async_completion.hpp"
#include "../async/async_strategy.hpp"   // async::Strategy, DriverStrategy

namespace embed {

/// 7-битный адрес устройства I2C.
///
/// Объявлен СНАРУЖИ шаблона II2c: иначе у драйверов с разными стратегиями
/// (Polling/Async/Dma) получились бы взаимоисключающие типы адреса, и один
/// и тот же прикладной код нельзя было бы прогнать по всем трём.
struct I2cAddress {
    uint8_t value = 0u;          // 7 бит

    explicit constexpr I2cAddress(uint8_t a = 0u) noexcept : value(a & 0x7Fu) {}

    /// Байт адреса для операции записи (бит R/W = 0).
    constexpr uint8_t writeByte() const noexcept {
        return static_cast<uint8_t>(value << 1);
    }
    /// Байт адреса для операции чтения (бит R/W = 1).
    constexpr uint8_t readByte() const noexcept {
        return static_cast<uint8_t>((value << 1) | 1u);
    }
    /// Адрес допустим (не 0 и не зарезервированный диапазон).
    constexpr bool valid() const noexcept {
        return value != 0u && value < 0x78u;
    }
};

/// Интерфейс I2C (ведущий и ведомый).
///
/// АДРЕСАЦИЯ
/// Адрес устройства передаётся в каждой операции, а не задаётся при
/// конфигурации: на одной шине обычно несколько устройств, и привязывать
/// драйвер к одному адресу неудобно.
///
/// ЗАВЕРШЕНИЕ ОПЕРАЦИИ
/// Как и у UART, определяется стратегией интерфейса:
///   Polling — драйвер опрашивает флаги в pollOnIrq();
///   Async   — onCompleteIrq()/onErrorIrq() из обработчика событий;
///   Dma     — onDmaComplete() из обработчика DMA.
template<typename Impl>
class II2c : public IHwInterface<Impl>
{
public:
    enum class Speed { Standard100k, Fast400k, FastPlus1M };
    enum class MasterMode { Master, Slave };

    using Address = I2cAddress;

    struct Config
    {
        Speed      speed = Speed::Standard100k;
        bool       enableClockStretching = true;
        MasterMode role = MasterMode::Master;
        /// Частота шины APB1 в герцах — нужна для расчёта таймингов.
        /// Для STM32F103: 36 МГц при SYSCLK 72 МГц.
        uint32_t   peripheralClockHz = 36000000u;
        /// Собственный адрес для режима Slave.
        Address    ownAddress{};
    };

    using AsyncMode = typename IHwInterface<Impl>::AsyncMode;

    // ─────────────────────────── Настройка ───────────────────────────

    Status configure(const Config& config) noexcept {
        return self().configure(config);
    }

    // ─────────────── Асинхронный запуск операции драйвером ───────────────

    /// Привязать объект завершения к следующей операции.
    Status setCompletion(async::completion& token) noexcept {
        return self().setCompletion(token);
    }

    /// Записать данные без ожидания; завершение — через привязанный объект.
    Status startWrite(Address addr, const void* data, Size len) noexcept {
        return self().startWrite(addr, data, len);
    }

    /// Прочитать данные без ожидания.
    Status startRead(Address addr, void* buffer, Size len) noexcept {
        return self().startRead(addr, buffer, len);
    }

    /// Запись + повторный START + чтение без ожидания.
    Status startWriteRead(Address addr, const void* tx, Size tx_len,
                          void* rx, Size rx_len) noexcept {
        return self().startWriteRead(addr, tx, tx_len, rx, rx_len);
    }

    /// Прервать операцию и освободить шину (STOP + сброс периферии).
    Status abort() noexcept { return self().abort(); }

    /// Есть ли аппаратный DMA.
    static constexpr bool hasDma() noexcept { return Impl::kHasDma; }

    // ─────────────────────── Операции обмена ───────────────────────

    /// Подключить опрос интерфейса к ожиданию (нужно только для Polling).
    ///
    /// AsyncPort опрашивает интерфейс сам, но методы ниже ждут completion
    /// напрямую: без опроса драйвер в режиме Polling не завершил бы обмен
    /// никогда (в mock это незаметно — он завершает операцию внутри start*).
    static void armPoll(async::completion& token) noexcept {
        if (async::DriverStrategy<Impl>::get() == async::Strategy::Polling) {
            token.set_poll(&poll_once, nullptr);
        }
    }

    /// Снять операцию, если она НЕ завершилась (таймаут): драйвер не должен
    /// остаться с указателем на локальный объект завершения, который вот-вот
    /// будет уничтожен (см. IUart::releaseIfPending).
    void releaseIfPending(async::completion& token) noexcept {
        if (self().pendingCompletion() == &token) (void)self().abort();
    }

    /// Ожидание завершения операции по привязанному объекту.
    /// Вынесено в базу: драйверу остаётся только выставить завершение.
    static Status waitCompletion(async::completion& token,
                                 Timeout timeout) noexcept {
        if (timeout != WaitForever) {
            token.reset(async::WaitPolicy::Timeout, static_cast<uint32_t>(timeout));
        }
        token.begin();
        (void)token.wait();
        return token.status();
    }

    /// Записать данные устройству: START, адрес+W, байты, STOP.
    Status write(Address addr, const void* data, Size len, Size& written,
                 Timeout timeout = WaitForever) noexcept {
        written = 0u;
        if (data == nullptr || len == 0u || !addr.valid()) return Status::InvalidArg;

        async::completion token;
        token.reset(timeout == WaitForever ? async::WaitPolicy::Forever
                                           : async::WaitPolicy::Timeout,
                    static_cast<uint32_t>(timeout));
        armPoll(token);

        const Status st = setCompletion(token);
        if (st != Status::Ok) return st;

        token.begin();
        const Status start = startWrite(addr, data, len);
        if (start != Status::Ok) { (void)token.post_error(start); return start; }

        (void)token.wait();
        written = token.transferred();
        releaseIfPending(token);
        return token.status();
    }

    /// Прочитать данные от устройства: START, адрес+R, байты, STOP.
    Status read(Address addr, void* buffer, Size len, Size& bytesRead,
                Timeout timeout = WaitForever) noexcept {
        bytesRead = 0u;
        if (buffer == nullptr || len == 0u || !addr.valid()) return Status::InvalidArg;

        async::completion token;
        token.reset(timeout == WaitForever ? async::WaitPolicy::Forever
                                           : async::WaitPolicy::Timeout,
                    static_cast<uint32_t>(timeout));
        armPoll(token);

        const Status st = setCompletion(token);
        if (st != Status::Ok) return st;

        token.begin();
        const Status start = startRead(addr, buffer, len);
        if (start != Status::Ok) { (void)token.post_error(start); return start; }

        (void)token.wait();
        bytesRead = token.transferred();
        releaseIfPending(token);
        return token.status();
    }

    /// Запись с последующим повторным START и чтением — типовая операция
    /// «прочитать регистр устройства». STOP между фазами НЕ ставится.
    Status writeRead(Address addr,
                     const void* tx, Size tx_len,
                     void* rx, Size rx_len, Size& bytesRead,
                     Timeout timeout = WaitForever) noexcept {
        bytesRead = 0u;
        if (tx == nullptr || tx_len == 0u || rx == nullptr || rx_len == 0u) {
            return Status::InvalidArg;
        }
        if (!addr.valid()) return Status::InvalidArg;

        async::completion token;
        token.reset(timeout == WaitForever ? async::WaitPolicy::Forever
                                           : async::WaitPolicy::Timeout,
                    static_cast<uint32_t>(timeout));
        armPoll(token);

        const Status st = setCompletion(token);
        if (st != Status::Ok) return st;

        token.begin();
        const Status start = startWriteRead(addr, tx, tx_len, rx, rx_len);
        if (start != Status::Ok) { (void)token.post_error(start); return start; }

        (void)token.wait();
        bytesRead = token.transferred();
        releaseIfPending(token);
        return token.status();
    }

    /// Прочитать один регистр (байт).
    Status readRegister(Address addr, uint8_t reg, uint8_t& value,
                        Timeout timeout = WaitForever) noexcept {
        Size got = 0u;
        const Status st = writeRead(addr, &reg, 1u, &value, 1u, got, timeout);
        if (st != Status::Ok) return st;
        return (got == 1u) ? Status::Ok : Status::Timeout;
    }

    /// Записать один регистр (байт).
    Status writeRegister(Address addr, uint8_t reg, uint8_t value,
                         Timeout timeout = WaitForever) noexcept {
        const uint8_t buf[2] = { reg, value };
        Size written = 0u;
        return write(addr, buf, 2u, written, timeout);
    }

    /// Сканирование шины: found[i] == true, если устройство с адресом i
    /// ответило. Занимает до 127 операций — не вызывать в горячем пути.
    Status scan(bool (&found)[128], Timeout per_addr_timeout = 5u) noexcept {
        for (unsigned i = 0u; i < 128u; ++i) found[i] = false;

        for (uint8_t a = 1u; a < 0x78u; ++a) {
            const Address addr{a};
            const Status st = self().probe(addr, per_addr_timeout);
            if (st == Status::Ok)                  found[a] = true;
            else if (st == Status::HardwareError)  return st;   // шина сломана
        }
        return Status::Ok;
    }

    Status recover() noexcept {
        return self().recover();
    }

    // ─────────────────────── Точки входа прерываний ───────────────────────

    /// Операция завершена (EV-прерывание).
    void onCompleteIrq() noexcept { self().onCompleteIrq(); }
    /// Ошибка (NACK/AF/BERR/ARLO — ER-прерывание).
    void onErrorIrq(Status st) noexcept { self().onErrorIrq(st); }
    /// Обслуживание без прерываний: опрос флагов.
    void pollOnIrq() noexcept { self().pollOnIrq(); }

    /// Текущий привязанный объект завершения (nullptr, если операции нет).
    async::completion* pendingCompletion() noexcept {
        return self().pendingCompletion();
    }

protected:
    II2c() = default;

private:
    Impl&       self()       noexcept { return *static_cast<Impl*>(this); }
    const Impl& self() const noexcept { return *static_cast<const Impl*>(this); }

    /// Опрос интерфейса вместо прерывания — для стратегии Polling.
    static void poll_once(async::completion& /*c*/, void* /*arg*/) noexcept {
        Impl::instance().pollOnIrq();
    }
};

}
