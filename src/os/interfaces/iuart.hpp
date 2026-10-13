// iuart.hpp
#pragma once
#include "iinterface.hpp"
#include "../async/async_completion.hpp"

namespace embed {

/// Интерфейс UART.
///
/// ДВА УРОВНЯ ИСПОЛЬЗОВАНИЯ
///
/// 1. Наследование (реализация драйвера). Наследник предоставляет
///    примитивы: configure/open/close/read/write/ioctl и хуки прерываний
///    onRxIrq()/onTxIrq()/onError().
///
/// 2. Прикладной код. Пишет ЛИНЕЙНО через async::AsyncPort<Hal>
///    (см. os/async/async_port.hpp) либо блокирующими методами ниже,
///    если асинхронный модуль не нужен.
///
/// Кто завершает операцию, решает стратегия интерфейса:
///   Polling — драйвер сам, в poll_on_irq();
///   Async   — обработчик прерывания вызывает onRxIrq()/onTxIrq();
///   Dma     — обработчик DMA вызывает onDmaComplete().
template<typename Impl>
class IUart : public IHwInterface<Impl>
{
public:
    enum class Parity { None, Odd, Even };

    enum class StopBits { One, OneAndHalf, Two };

    struct Config
    {
        unsigned baudRate = 115200u;
        unsigned dataBits = 8u;
        Parity   parity   = Parity::None;
        StopBits stopBits = StopBits::One;
        /// Включить аппаратное управление потоком (RTS/CTS), если есть.
        bool     flowControl = false;
    };

    using AsyncMode = typename IHwInterface<Impl>::AsyncMode;

    // ─────────────────────────── Настройка ───────────────────────────

    Status configure(const Config& config) noexcept {
        return self().configure(config);
    }

    // ─────────────── Асинхронный запуск операции драйвером ───────────────
    //
    // Так работает связка с async::AsyncPort:
    //   1. порт вызывает setCompletion(&token) и startWrite()/startRead();
    //   2. драйвер запускает передачу и НЕ ждёт её;
    //   3. завершение приходит из прерывания или DMA, драйвер вызывает
    //      token->post_ok(n) / token->post_error(st);
    //   4. порт в это время спит и просыпается по завершению.
    //
    // Именно поэтому приложению не нужны колбэки: пробуждение берёт на
    // себя объект завершения.

    /// Привязать объект завершения к следующей операции.
    Status setCompletion(async::completion& token) noexcept {
        return self().setCompletion(token);
    }

    /// Запустить передачу и вернуть управление СРАЗУ.
    /// Завершение — через привязанный setCompletion() объект.
    Status startWrite(const void* data, Size len) noexcept {
        return self().startWrite(data, len);
    }

    /// Запустить приём и вернуть управление СРАЗУ.
    Status startRead(void* buffer, Size capacity) noexcept {
        return self().startRead(buffer, capacity);
    }

    /// Прервать текущую операцию (например по таймауту).
    Status abort() noexcept { return self().abort(); }

    /// Есть ли аппаратный DMA для этого интерфейса.
    static constexpr bool hasDma() noexcept { return Impl::kHasDma; }

    // ─────────────────────── Линейные операции ───────────────────────
    //
    // Обёртки над start*/completion: одно и то же ядро, поэтому логика
    // таймаутов и разбора ошибок не дублируется.

    /// Отправить ровно len байт, дождавшись завершения.
    Status write(const void* data, Size len, Size& written,
                 Timeout timeout = WaitForever) noexcept {
        written = 0u;
        if (data == nullptr || len == 0u) return Status::InvalidArg;

        async::completion token;
        token.reset(timeout == WaitForever ? async::WaitPolicy::Forever
                                           : async::WaitPolicy::Timeout,
                    static_cast<uint32_t>(timeout));

        const Status st = setCompletion(token);
        if (st != Status::Ok) return st;

        token.begin();
        const Status start = startWrite(data, len);
        if (start != Status::Ok) {
            (void)token.post_error(start);
            return start;
        }
        (void)token.wait();
        written = token.transferred();
        return token.status();
    }

    /// Прочитать до capacity байт, дождавшись хотя бы одного.
    Status read(void* buffer, Size capacity, Size& bytesRead,
                Timeout timeout = WaitForever) noexcept {
        bytesRead = 0u;
        if (buffer == nullptr || capacity == 0u) return Status::InvalidArg;

        async::completion token;
        token.reset(timeout == WaitForever ? async::WaitPolicy::Forever
                                           : async::WaitPolicy::Timeout,
                    static_cast<uint32_t>(timeout));

        const Status st = setCompletion(token);
        if (st != Status::Ok) return st;

        token.begin();
        const Status start = startRead(buffer, capacity);
        if (start != Status::Ok) {
            (void)token.post_error(start);
            return start;
        }
        (void)token.wait();
        bytesRead = token.transferred();
        return token.status();
    }

    /// Отправить строку с CRLF — удобно для AT-команд и логов.
    Status writeLine(const char* text) noexcept {
        if (text == nullptr) return Status::InvalidArg;
        Size n = 0u;
        while (text[n] != '\0') ++n;

        Size written = 0u;
        Status st = write(text, n, written);
        if (st != Status::Ok) return st;
        st = write("\r\n", 2u, written);
        return st;
    }

    /// Читать до терминатора (терминатор поглощается, в буфер не входит).
    Status readLine(void* buffer, Size capacity, Size& bytesRead,
                    uint32_t timeout = 1000u) noexcept {
        if (buffer == nullptr || capacity == 0u) return Status::InvalidArg;

        auto* out = static_cast<uint8_t*>(buffer);
        bytesRead = 0u;

        const uint64_t start = IHwInterface<Impl>::timerNow();
        for (;;) {
            if (bytesRead >= capacity) return Status::NoMemory;

            uint32_t left = timeout;
            if (timeout != 0u && IHwInterface<Impl>::timerAvailable()) {
                const uint32_t spent =
                    static_cast<uint32_t>(IHwInterface<Impl>::timerNow() - start);
                if (spent >= timeout) return Status::Timeout;
                left = timeout - spent;
            }

            Size got = 0u;
            const Status st = read(out + bytesRead, capacity - bytesRead, got, left);
            if (st == Status::Timeout) return Status::Timeout;
            if (st != Status::Ok) return st;

            for (Size i = 0u; i < got; ++i) {
                if (static_cast<char>(out[bytesRead]) == '\n') {
                    // Отбрасываем CR перед LF — обычное завершение строки.
                    if (bytesRead > 0u && static_cast<char>(out[bytesRead - 1u]) == '\r') {
                        --bytesRead;
                    }
                    return Status::Ok;
                }
                ++bytesRead;
            }

            if (got == 0u) IHwInterface<Impl>::yieldCpu();
        }
    }

    // ─────────────────────── Точки входа прерываний ───────────────────────
    //
    // Драйвер вызывает их из своих обработчиков. Приложение их не трогает:
    // именно поэтому колбэки писать не нужно.

    /// Готовность к приёму (RXNE / DMA HT/TC на приём).
    void onRxIrq() noexcept { self().onRxIrq(); }
    /// Передача завершена (TXE/TC / DMA TC на передачу).
    void onTxIrq() noexcept { self().onTxIrq(); }
    /// Ошибка канала (ORE/FE/NE, DMA TE, NACK на I2C).
    void onErrorIrq(Status st) noexcept { self().onErrorIrq(st); }

    /// Текущий привязанный объект завершения (nullptr, если операции нет).
    async::completion* pendingCompletion() noexcept {
        return self().pendingCompletion();
    }

    /// Обслуживание без прерываний: опрос регистров.
    /// Вызывается асинхронным модулем в цикле ожидания.
    void pollOnIrq() noexcept { self().pollOnIrq(); }

protected:
    IUart() = default;

private:
    Impl&       self()       noexcept { return *static_cast<Impl*>(this); }
    const Impl& self() const noexcept { return *static_cast<const Impl*>(this); }
};

}
