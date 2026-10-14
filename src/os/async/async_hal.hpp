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
#include "../interfaces/dma_arbiter.hpp"

#include <type_traits>

namespace embed::async {

// ─────────────────── Арбитр каналов DMA: общие помощники ────────────────────

/// Есть ли у драйвера запросы DMA (kTxRequest/kRxRequest).
///
/// Нужны арбитру каналов: по ним он понимает, какие каналы займёт работа и с
/// кем интерфейс делит канал. У драйвера без DMA (например mock) обёртки
/// вернут NotSupported, а не «сделают вид», что работа поставлена в очередь.
template <typename Driver, typename = void>
struct HasDmaRequests : std::false_type {};

template <typename Driver>
struct HasDmaRequests<Driver, std::void_t<decltype(Driver::kTxRequest)>>
    : std::true_type {};

// ────────────────────────────── UART ────────────────────────────────────────

/// Адаптер UART-драйвера к AsyncPort.
///
/// Driver — класс, унаследованный от IUart<Driver>. Требуется объявить
///   static Driver& instance() noexcept;      // единственный экземпляр
///   static constexpr bool     kHasDma;
/// и одно из двух:
///   static Strategy strategy() noexcept;     // режим можно менять на лету
///   static constexpr Strategy kStrategy;     // режим фиксирован
/// Шаблон проверяет наличие операций на этапе компиляции, поэтому
/// несовпадение сигнатур видно сразу, а не при линковке.
template <typename Driver>
class UartPort : public AsyncPort<UartPort<Driver>> {
    using Base = AsyncPort<UartPort<Driver>>;

public:
    static Driver& driver() noexcept { return Driver::instance(); }

    static Strategy strategy() noexcept { return DriverStrategy<Driver>::get(); }

    static constexpr Size max_chunk() noexcept { return Driver::kMaxChunk; }

    static void cancel_transfer() noexcept { (void)driver().abort(); }

    /// Опрос регистров для стратегии Polling. AsyncPort вызывает это
    /// в цикле ожидания.
    ///
    /// Именно pollOnIrq(): это документированная точка опроса драйвера
    /// (iuart.hpp). Прежний вызов onRxIrq() в mock-драйвере двигал только
    /// приём и не продвигал передачу.
    static void poll_on_irq() noexcept { driver().pollOnIrq(); }

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

    // ── Через арбитр каналов DMA (dma_arbiter.hpp) ──
    //
    // Отличие от write()/read(): работа НЕ отказывает, если канал занят
    // другим интерфейсом (USART1_TX и I2C2_TX — это оба DMA1 Ch4), а встаёт
    // в очередь арбитра и выполняется, когда канал освободится.
    //
    // Требуется Strategy::Dma: арбитр занимает каналы, и «оплачивать» это
    // обменом по прерываниям смысла нет.

    /// Отправить буфер через арбитр каналов. timeout_ms включает ожидание
    /// канала в очереди; 0 — ждать без ограничения.
    Status writeQueued(const void* data, Size len, uint32_t timeout_ms = 0u) noexcept {
        if (data == nullptr || len == 0u) return Status::InvalidArg;
        if constexpr (!HasDmaRequests<Driver>::value) {
            return Status::NotSupported;      // у драйвера нет DMA-запросов
        } else {
            if (strategy() != Strategy::Dma) return Status::NotSupported;

            WriteCtx ctx{ &driver(), data, len };
            Size done = 0u;
            return DmaArbiter::instance().run(
                makeJob(Driver::kTxRequest, config::DmaRequest::None, &ctx,
                        &WriteCtx::start, &WriteCtx::cancel),
                timeout_ms, done);
        }
    }

    /// Принять буфер через арбитр каналов.
    Status readQueued(void* buf, Size capacity, Size& bytes_read,
                      uint32_t timeout_ms = 0u) noexcept {
        bytes_read = 0u;
        if (buf == nullptr || capacity == 0u) return Status::InvalidArg;
        if constexpr (!HasDmaRequests<Driver>::value) {
            return Status::NotSupported;
        } else {
            if (strategy() != Strategy::Dma) return Status::NotSupported;

            ReadCtx ctx{ &driver(), buf, capacity };
            Size done = 0u;
            const Status st = DmaArbiter::instance().run(
                makeJob(Driver::kRxRequest, config::DmaRequest::None, &ctx,
                        &ReadCtx::start, &ReadCtx::cancel),
                timeout_ms, done);
            bytes_read = done;
            return st;
        }
    }

private:
    /// Параметры работы арбитра. Лежат на стеке вызывающего: run() не
    /// возвращается раньше завершения работы, поэтому указатель остаётся
    /// валидным всё время, пока работа в очереди и в обмене.
    struct WriteCtx {
        Driver*     drv;
        const void* data;
        Size        len;

        static Status start(void* ctx, completion& token) noexcept {
            auto* c = static_cast<WriteCtx*>(ctx);
            (void)c->drv->setCompletion(token);
            return c->drv->startWrite(c->data, c->len);
        }
        static void cancel(void* ctx) noexcept {
            (void)static_cast<WriteCtx*>(ctx)->drv->abort();
        }
    };

    struct ReadCtx {
        Driver* drv;
        void*   buf;
        Size    capacity;

        static Status start(void* ctx, completion& token) noexcept {
            auto* c = static_cast<ReadCtx*>(ctx);
            (void)c->drv->setCompletion(token);
            return c->drv->startRead(c->buf, c->capacity);
        }
        static void cancel(void* ctx) noexcept {
            (void)static_cast<ReadCtx*>(ctx)->drv->abort();
        }
    };

    static DmaJob makeJob(config::DmaRequest tx, config::DmaRequest rx, void* ctx,
                          DmaStartFn start, DmaCancelFn cancel) noexcept {
        DmaJob job{};
        job.tx     = tx;
        job.rx     = rx;
        job.owner  = &driver();
        job.start  = start;
        job.cancel = cancel;
        job.ctx    = ctx;
        return job;
    }
};

// ─────────────────────────────── I2C ────────────────────────────────────────

/// Адаптер I2C-драйвера к AsyncPort.
///
/// Требует от драйвера:
///   static Driver& instance() noexcept;
///   static constexpr bool     kHasDma;
/// и одно из двух:
///   static Strategy strategy() noexcept;
///   static constexpr Strategy kStrategy;
template <typename Driver>
class I2cPort : public AsyncPort<I2cPort<Driver>, 100u> {
    using Base = AsyncPort<I2cPort<Driver>, 100u>;
    using Address = typename Driver::Address;

public:
    static Driver& driver() noexcept { return Driver::instance(); }

    static Strategy strategy() noexcept { return DriverStrategy<Driver>::get(); }

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
    //
    // Эти методы НЕ проходят через AsyncPort::transfer() (у них своя
    // сигнатура с адресом), поэтому для стратегии Polling опрос интерфейса
    // подключается вручную: иначе завершение не наступило бы никогда.

    /// Опрос интерфейса в цикле ожидания — только для Polling.
    static void arm_poll(completion& token) noexcept {
        if (strategy() == Strategy::Polling) {
            token.set_poll(&poll_once, nullptr);
        }
    }

    Status write(Address addr, const void* data, Size len, Size& written,
                 uint32_t timeout_ms = 0u) noexcept {
        written = 0u;
        if (data == nullptr || len == 0u) return Status::InvalidArg;

        completion token;
        token.reset(timeout_ms == 0u ? WaitPolicy::Forever : WaitPolicy::Timeout,
                    timeout_ms);
        arm_poll(token);
        if (driver().setCompletion(token) != Status::Ok) return Status::Fault;

        token.begin();
        const Status st = driver().startWrite(addr, data, len);
        if (st != Status::Ok) { (void)token.post_error(st); return st; }

        (void)token.wait();
        written = token.transferred();
        release_if_pending(token);
        return token.status();
    }

    Status read(Address addr, void* buffer, Size len, Size& bytes_read,
                uint32_t timeout_ms = 0u) noexcept {
        bytes_read = 0u;
        if (buffer == nullptr || len == 0u) return Status::InvalidArg;

        completion token;
        token.reset(timeout_ms == 0u ? WaitPolicy::Forever : WaitPolicy::Timeout,
                    timeout_ms);
        arm_poll(token);
        if (driver().setCompletion(token) != Status::Ok) return Status::Fault;

        token.begin();
        const Status st = driver().startRead(addr, buffer, len);
        if (st != Status::Ok) { (void)token.post_error(st); return st; }

        (void)token.wait();
        bytes_read = token.transferred();
        release_if_pending(token);
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
        arm_poll(token_arena_);
        if (driver().setCompletion(token_arena_) != Status::Ok) return Status::Fault;

        token_arena_.begin();
        const Status st = driver().startWriteRead(addr, &reg, 1u, &value, 1u);
        if (st != Status::Ok) { (void)token_arena_.post_error(st); return st; }

        (void)token_arena_.wait();
        release_if_pending(token_arena_);
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

    // ── Через арбитр каналов DMA (dma_arbiter.hpp) ──
    //
    // Работа не отказывает, если канал занят другим интерфейсом (I2C1_TX делит
    // DMA1 Ch6 с USART2_RX, I2C1_RX делит Ch7 с USART2_TX), а встаёт в очередь
    // арбитра. Требуется Strategy::Dma.

    /// Записать данные через арбитр каналов.
    Status writeQueued(Address addr, const void* data, Size len,
                       uint32_t timeout_ms = 0u) noexcept {
        if (data == nullptr || len == 0u || !addr.valid()) return Status::InvalidArg;
        if constexpr (!HasDmaRequests<Driver>::value) {
            return Status::NotSupported;
        } else {
            if (strategy() != Strategy::Dma) return Status::NotSupported;

            WriteCtx ctx{ &driver(), addr, data, len };
            Size done = 0u;
            return DmaArbiter::instance().run(
                makeJob(Driver::kTxRequest, config::DmaRequest::None, &ctx,
                        &WriteCtx::start, &WriteCtx::cancel),
                timeout_ms, done);
        }
    }

    /// Записать регистр через арбитр каналов (2 байта: адрес регистра, значение).
    Status writeRegisterQueued(Address addr, uint8_t reg, uint8_t value,
                               uint32_t timeout_ms = 0u) noexcept {
        if constexpr (!HasDmaRequests<Driver>::value) {
            return Status::NotSupported;
        } else {
            // Буфер — внутри контекста работы: он живёт до её завершения.
            RegisterWriteCtx ctx{ &driver(), addr, reg, value };
            Size done = 0u;
            return DmaArbiter::instance().run(
                makeJob(Driver::kTxRequest, config::DmaRequest::None, &ctx,
                        &RegisterWriteCtx::start, &RegisterWriteCtx::cancel),
                timeout_ms, done);
        }
    }

    /// Прочитать данные через арбитр каналов.
    Status readQueued(Address addr, void* buffer, Size len, Size& bytes_read,
                      uint32_t timeout_ms = 0u) noexcept {
        bytes_read = 0u;
        if (buffer == nullptr || len == 0u || !addr.valid()) return Status::InvalidArg;
        if constexpr (!HasDmaRequests<Driver>::value) {
            return Status::NotSupported;
        } else {
            if (strategy() != Strategy::Dma) return Status::NotSupported;

            ReadCtx ctx{ &driver(), addr, buffer, len };
            Size done = 0u;
            const Status st = DmaArbiter::instance().run(
                makeJob(Driver::kRxRequest, config::DmaRequest::None, &ctx,
                        &ReadCtx::start, &ReadCtx::cancel),
                timeout_ms, done);
            bytes_read = done;
            return st;
        }
    }

    /// Прочитать регистр через арбитр каналов.
    ///
    /// Работа занимает ОБА канала (запись адреса регистра — TX, чтение — RX)
    /// и захватывает их разом: иначе две такие работы могли бы встать
    /// взаимной блокировкой, поделив Ch6 и Ch7.
    Status readRegisterQueued(Address addr, uint8_t reg, uint8_t& value,
                              uint32_t timeout_ms = 0u) noexcept {
        if (!addr.valid()) return Status::InvalidArg;
        if constexpr (!HasDmaRequests<Driver>::value) {
            return Status::NotSupported;
        } else {
            if (strategy() != Strategy::Dma) return Status::NotSupported;

            RegisterReadCtx ctx{ &driver(), addr, reg, value };
            Size done = 0u;
            return DmaArbiter::instance().run(
                makeJob(Driver::kTxRequest, Driver::kRxRequest, &ctx,
                        &RegisterReadCtx::start, &RegisterReadCtx::cancel),
                timeout_ms, done);
        }
    }

private:
    static inline Address current_address_{};

    /// Опрос интерфейса вместо прерывания — для стратегии Polling.
    static void poll_once(completion& /*c*/, void* /*arg*/) noexcept {
        driver().pollOnIrq();
    }

    /// Снять незавершённую операцию: драйвер не должен остаться с указателем
    /// на объект завершения, который вот-вот будет уничтожен (у write/read он
    /// лежит на стеке вызова, у readRegister — членом порта).
    static void release_if_pending(completion& token) noexcept {
        if (driver().pendingCompletion() == &token) (void)driver().abort();
    }

    // ── Параметры работ арбитра ──
    //
    // Лежат на стеке вызывающего: run() возвращается только после завершения
    // работы, поэтому указатели (в т.ч. на буфер) остаются валидными.

    struct WriteCtx {
        Driver*     drv;
        Address     addr;
        const void* data;
        Size        len;

        static Status start(void* ctx, completion& token) noexcept {
            auto* c = static_cast<WriteCtx*>(ctx);
            (void)c->drv->setCompletion(token);
            return c->drv->startWrite(c->addr, c->data, c->len);
        }
        static void cancel(void* ctx) noexcept {
            (void)static_cast<WriteCtx*>(ctx)->drv->abort();
        }
    };

    struct RegisterWriteCtx {
        Driver* drv;
        Address addr;
        uint8_t buf[2];      ///< адрес регистра + значение

        RegisterWriteCtx(Driver* d, Address a, uint8_t reg, uint8_t value) noexcept
            : drv(d), addr(a), buf{ reg, value } {}

        static Status start(void* ctx, completion& token) noexcept {
            auto* c = static_cast<RegisterWriteCtx*>(ctx);
            (void)c->drv->setCompletion(token);
            return c->drv->startWrite(c->addr, c->buf, 2u);
        }
        static void cancel(void* ctx) noexcept {
            (void)static_cast<RegisterWriteCtx*>(ctx)->drv->abort();
        }
    };

    struct ReadCtx {
        Driver* drv;
        Address addr;
        void*   buf;
        Size    len;

        static Status start(void* ctx, completion& token) noexcept {
            auto* c = static_cast<ReadCtx*>(ctx);
            (void)c->drv->setCompletion(token);
            return c->drv->startRead(c->addr, c->buf, c->len);
        }
        static void cancel(void* ctx) noexcept {
            (void)static_cast<ReadCtx*>(ctx)->drv->abort();
        }
    };

    struct RegisterReadCtx {
        Driver* drv;
        Address addr;
        uint8_t reg;
        uint8_t& value;

        static Status start(void* ctx, completion& token) noexcept {
            auto* c = static_cast<RegisterReadCtx*>(ctx);
            (void)c->drv->setCompletion(token);
            return c->drv->startWriteRead(c->addr, &c->reg, 1u, &c->value, 1u);
        }
        static void cancel(void* ctx) noexcept {
            (void)static_cast<RegisterReadCtx*>(ctx)->drv->abort();
        }
    };

    static DmaJob makeJob(config::DmaRequest tx, config::DmaRequest rx, void* ctx,
                          DmaStartFn start, DmaCancelFn cancel) noexcept {
        DmaJob job{};
        job.tx     = tx;
        job.rx     = rx;
        job.owner  = &driver();
        job.start  = start;
        job.cancel = cancel;
        job.ctx    = ctx;
        return job;
    }

    /// Владелец объекта завершения для операций, которые не проходят через
    /// базовый AsyncPort (у них своя сигнатура с адресом).
    completion token_arena_{};
};

} // namespace embed::async
