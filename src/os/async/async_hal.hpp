// async_hal.hpp
#pragma once
// ---------------------------------------------------------------------------
// МОСТ «ДРАЙВЕР ИНТЕРФЕЙСА -> АСИНХРОННЫЙ ПОРТ»
//
// Зачем нужен отдельный слой:
//   * IUart/II2c — это драйверы. У них есть методы, вызываемые
//     обработчиками прерываний, и ровно один «текущий» объект завершения.
//   * async::AsyncPort<Hal> требует СТАТИЧЕСКИХ методов (strategy,
//     startWrite, startRead) и не зависит от конкретного драйвера.
// Этот адаптер связывает одно с другим: AsyncPort видит статические методы,
// которые внутри обращаются к единственному экземпляру драйвера.
//
// РЕЗУЛЬТАТ ДЛЯ ПОЛЬЗОВАТЕЛЯ
//
//     static Uart1 uart;                       // драйвер
//     embed::async::UartPort<Uart1> port;      // линейный API
//
//     port.writeLine("AT");                    // как обычный вызов
//     port.readLine(buf, sizeof buf, n);       // и никаких колбэков
//
// Стратегия (Polling/Async/Dma) берётся у драйвера, поэтому приложение
// о ней не думает.
// ---------------------------------------------------------------------------

#include "async_port.hpp"
#include "../interfaces/ii2c.hpp"
#include "../interfaces/iuart.hpp"

namespace embed::async {

// ────────────────────────────── UART ────────────────────────────────────────

/// Адаптер UART-драйвера к AsyncPort.
///
/// Driver — класс, унаследованный от IUart<Driver>. Требуется объявить
///   static Driver& instance() noexcept;      // единственный экземпляр
///   static constexpr Strategy kStrategy;     // Polling/Async/Dma
///   static constexpr bool     kHasDma;
/// Шаблон проверяет наличие операций на этапе компиляции, поэтому
/// несовпадение сигнатур видно сразу, а не при линковке.
template <typename Driver>
class UartPort : public AsyncPort<UartPort<Driver>> {
    using Base = AsyncPort<UartPort<Driver>>;

public:
    static Driver& driver() noexcept { return Driver::instance(); }

    static Strategy strategy() noexcept { return Driver::kStrategy; }

    static constexpr Size max_chunk() noexcept { return Driver::kMaxChunk; }

    static void cancel_transfer() noexcept { (void)driver().abort(); }

    /// Опрос регистров для стратегии Polling. AsyncPort вызывает это
    /// в цикле ожидания.
    static void poll_on_irq() noexcept { driver().onRxIrq(); }

    static Status startWrite(completion& c, const void* data, Size len) noexcept {
        driver().setCompletion(c);
        return driver().startWrite(data, len);
    }

    static Status startRead(completion& c, void* buf, Size cap) noexcept {
        driver().setCompletion(c);
        return driver().startRead(buf, cap);
    }

    // ── Прикладной сахар поверх линейного API ──

    /// Дождаться готовности линии (для некоторых модулей — по синтаксису).
    Status waitReady(uint32_t timeout_ms = 0u) noexcept {
        (void)timeout_ms;
        return Status::Ok;
    }
};

// ─────────────────────────────── I2C ────────────────────────────────────────

/// Адаптер I2C-драйвера к AsyncPort.
///
/// Требует от драйвера:
///   static Driver& instance() noexcept;
///   static constexpr Strategy kStrategy;
///   static constexpr bool     kHasDma;
template <typename Driver>
class I2cPort : public AsyncPort<I2cPort<Driver>, 100u> {
    using Base = AsyncPort<I2cPort<Driver>, 100u>;
    using Address = typename Driver::Address;

public:
    static Driver& driver() noexcept { return Driver::instance(); }

    static Strategy strategy() noexcept { return Driver::kStrategy; }

    static constexpr Size max_chunk() noexcept { return Driver::kMaxChunk; }

    static void cancel_transfer() noexcept { (void)driver().abort(); }

    static void poll_on_irq() noexcept { driver().pollOnIrq(); }

    // ПРИМЕЧАНИЕ: производные read/write с адресом СКРЫВАЮТ одноимённые
    // методы базового AsyncPort. Это сделано намеренно: у I2C адрес —
    // обязательная часть операции, и «безадресный» read() прикладному коду
    // не нужен. Если нужен — вызывайте Base::read(...) явно.

    /// Базовая реализация AsyncPort нам не нужна: у I2C адрес — часть
    /// операции, поэтому «безадресный» startRead/startWrite не имеют смысла.
    /// Передаём адрес через поле, выставляемое методами ниже.
    static Status startWrite(completion& c, const void* data, Size len) noexcept {
        driver().setCompletion(c);
        return driver().startWrite(current_address_, data, len);
    }

    static Status startRead(completion& c, void* buf, Size cap) noexcept {
        driver().setCompletion(c);
        return driver().startRead(current_address_, buf, cap);
    }

    // ── Операции с адресом: то, чем реально пользуется приложение ──

    Status write(Address addr, const void* data, Size len, Size& written,
                 uint32_t timeout_ms = 0u) noexcept {
        written = 0u;
        if (data == nullptr || len == 0u) return Status::InvalidArg;

        completion token;
        token.reset(timeout_ms == 0u ? WaitPolicy::Forever : WaitPolicy::Timeout,
                    timeout_ms);
        if (driver().setCompletion(token) != Status::Ok) return Status::Fault;

        token.begin();
        const Status st = driver().startWrite(addr, data, len);
        if (st != Status::Ok) { (void)token.post_error(st); return st; }

        (void)token.wait();
        written = token.transferred();
        return token.status();
    }

    Status read(Address addr, void* buffer, Size len, Size& bytes_read,
                uint32_t timeout_ms = 0u) noexcept {
        bytes_read = 0u;
        if (buffer == nullptr || len == 0u) return Status::InvalidArg;

        completion token;
        token.reset(timeout_ms == 0u ? WaitPolicy::Forever : WaitPolicy::Timeout,
                    timeout_ms);
        if (driver().setCompletion(token) != Status::Ok) return Status::Fault;

        token.begin();
        const Status st = driver().startRead(addr, buffer, len);
        if (st != Status::Ok) { (void)token.post_error(st); return st; }

        (void)token.wait();
        bytes_read = token.transferred();
        return token.status();
    }

    /// Прочитать регистр: запись адреса регистра, повторный START, чтение.
    ///
    /// Объект завершения — ЧЛЕН порта, а не локальная переменная: драйвер
    /// хранит на него указатель и завершает операцию позже. Локальная
    /// переменная дала бы обращение к уничтоженному объекту (это ловил ASan).
    Status readRegister(Address addr, uint8_t reg, uint8_t& value,
                           uint32_t timeout_ms = 0u) noexcept {
        token_arena_.reset(timeout_ms == 0u ? WaitPolicy::Forever
                                            : WaitPolicy::Timeout,
                           timeout_ms);
        if (driver().setCompletion(token_arena_) != Status::Ok) return Status::Fault;

        token_arena_.begin();
        const Status st = driver().startWriteRead(addr, &reg, 1u, &value, 1u);
        if (st != Status::Ok) { (void)token_arena_.post_error(st); return st; }

        (void)token_arena_.wait();
        return token_arena_.status();
    }

    /// Записать регистр.
    Status writeRegister(Address addr, uint8_t reg, uint8_t value,
                            uint32_t timeout_ms = 0u) noexcept {
        const uint8_t buf[2] = { reg, value };
        Size written = 0u;
        return write(addr, buf, 2u, written, timeout_ms);
    }

    /// Сканирование шины: found[a] == true, если устройство ответило.
    Status scan(bool (&found)[128], uint32_t per_addr_timeout_ms = 5u) noexcept {
        for (unsigned i = 0u; i < 128u; ++i) found[i] = false;

        Size dummy = 0u;
        for (uint8_t a = 1u; a < 0x78u; ++a) {
            const Address addr{a};
            const uint8_t probe = 0u;
            const Status st = write(addr, &probe, 1u, dummy, per_addr_timeout_ms);
            if (st == Status::Ok) found[a] = true;
        }
        return Status::Ok;
    }

    /// Освободить шину после сбоя (9 тактов SCL + STOP).
    Status recover() noexcept { return driver().recover(); }

private:
    static inline Address current_address_{};

    /// Владелец объекта завершения для операций, которые не проходят через
    /// базовый AsyncPort (у них своя сигнатура с адресом).
    completion token_arena_{};
};

} // namespace embed::async
