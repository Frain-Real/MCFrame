// tests/test_uart_i2c.cpp
// ---------------------------------------------------------------------------
// Тесты UART и I2C: mock-платформа, все три стратегии, связка с async IO.
//
// ГЛАВНОЕ, ЧТО ПРОВЕРЯЕТСЯ
// Результат операции НЕ зависит от стратегии (Polling/Async/Dma):
// приложение пишет один и тот же линейный код, а меняется лишь то,
// кто завершает операцию.
// ---------------------------------------------------------------------------
#include "os/async/async_hal.hpp"
#include "os/config/time_config.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/core/task/sheduler.hpp"
#include "platforms/mock/i2c_mock.hpp"
#include "platforms/mock/uart_mock.hpp"

#include <cstdio>
#include <cstring>

using namespace embed;
using namespace embed::async;
using namespace embed::mock;

namespace {

int g_failed = 0;
int g_total  = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

template <typename T>
void check_eq(const char* what, T got, T expected) {
    const bool ok = (got == expected);
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %-44s got=%ld expected=%ld\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<long>(got), static_cast<long>(expected));
}

// ── Тик «железа»: имитирует то, что делает SysTick/обработчик IRQ ──
//
// В реальной системе это происходит само. Здесь мы явно продвигаем
// драйвер, чтобы проверять все три стратегии одинаково.
template <typename Drv>
void pump(Drv& drv, unsigned iterations = 8u) {
    for (unsigned i = 0u; i < iterations; ++i) {
        drv.pollOnIrq();
        drv.onRxIrq();
    }
}

// ─────────────────────────────── UART ───────────────────────────────────────

template <async::Strategy Strat, typename Drv>
void test_uart_linear(const char* title) {
    (void)std::printf("uart [%s]:\n", title);

    Drv& drv = Drv::instance();
    g_uart_bus.reset();

    typename Drv::Config cfg{};
    cfg.baudRate = 115200u;
    check("configure() == Ok", drv.configure(cfg) == Status::Ok);
    check("open() == Ok", drv.open() == Status::Ok);

    UartPort<Drv> port;

    // ── Передача: линейный код без колбэков ──
    {
        completion token;
        token.reset(WaitPolicy::Timeout, 50u);
        check("setCompletion() == Ok", drv.setCompletion(token) == Status::Ok);
        token.begin();

        const char* msg = "hello";
        const Status st = drv.startWrite(msg, 5u);
        check("startWrite() == Ok", st == Status::Ok);

        pump(drv);
        (void)token.wait();
        check("передача завершилась Ok", token.status() == Status::Ok);
        check_eq<Size>("передано 5 байт", token.transferred(), 5u);
    }

    // ── Передача через порт (линейный вызов) ──
    {
        g_uart_bus.reset();
        const Status st = port.write("abc", 3u, 50u);
        check("port.write() == Ok", st == Status::Ok);
        check_eq<Size>("в шине 3 байта", g_uart_bus.tx_len, 3u);
        check("содержимое верное", std::memcmp(g_uart_bus.tx, "abc", 3u) == 0);
    }

    // ── writeLine: + CRLF ──
    {
        g_uart_bus.reset();
        const Status st = port.writeLine("OK");
        check("port.writeLine() == Ok", st == Status::Ok);
        check_eq<Size>("OK + CRLF = 4 байта", g_uart_bus.tx_len, 4u);
        check("CRLF на месте",
              g_uart_bus.tx_len == 4u &&
              g_uart_bus.tx[2] == '\r' && g_uart_bus.tx[3] == '\n');
    }

    // ── Приём ──
    {
        g_uart_bus.reset();
        g_uart_bus.feed("PING");

        uint8_t buf[8]{};
        Size got = 0u;
        const Status st = drv.read(buf, sizeof buf, got, 50u);
        check("port.read() == Ok", st == Status::Ok);
        check_eq<Size>("прочитано 4 байта (PING)", got, 4u);
        check("данные верные", std::memcmp(buf, "PING", 4u) == 0);
    }

    // ── Приём точной длины ──
    {
        g_uart_bus.reset();
        g_uart_bus.feed("ABCDEF");

        uint8_t buf[6]{};
        Size got = 0u;
        const Status st = drv.read(buf, 6u, got, 50u);
        check("read() ровно 6 байт == Ok", st == Status::Ok);
        check_eq<Size>("прочитано 6", got, 6u);
        check("данные верные", std::memcmp(buf, "ABCDEF", 6u) == 0);
    }

    // ── readLine: терминатор поглощается, CR отбрасывается ──
    {
        g_uart_bus.reset();
        g_uart_bus.feed("TEMP=25\r\n");

        char line[16]{};
        Size got = 0u;
        const Status st = drv.readLine(line, sizeof line, got, 100u);
        check("readLine() == Ok", st == Status::Ok);
        check_eq<Size>("длина без CRLF", got, 7u);
        check("строка верная", std::memcmp(line, "TEMP=25", 7u) == 0);
    }

    // ── Ошибка канала ──
    {
        g_uart_bus.reset();
        g_uart_bus.fail_next_tx = true;

        completion token;
        token.reset(WaitPolicy::Timeout, 20u);
        (void)drv.setCompletion(token);
        token.begin();
        const Status st = drv.startWrite("x", 1u);
        (void)token.wait();
        check("ошибка канала не проглочена",
              st != Status::Ok || token.status() != Status::Ok);
    }
}

// ──────────────────────────────── I2C ───────────────────────────────────────

using TestAddr = I2cAddress;

template <async::Strategy Strat, typename Drv>
void test_i2c_linear(const char* title) {
    (void)std::printf("i2c [%s]:\n", title);

    Drv& drv = Drv::instance();
    g_i2c_bus.reset();

    typename Drv::Config cfg{};
    cfg.speed = Drv::Speed::Standard100k;
    cfg.peripheralClockHz = 36000000u;
    check("configure() == Ok", drv.configure(cfg) == Status::Ok);
    check("open() == Ok", drv.open() == Status::Ok);

    I2cPort<Drv> port;
    const TestAddr dev{0x50u};      // типовой адрес EEPROM

    // ── Устройства нет: NACK должен быть NotFound, а не «успех» ──
    {
        uint8_t buf[4]{};
        Size got = 0u;
        const Status st = port.read(dev, buf, 1u, got, 20u);
        check("отсутствующее устройство -> NotFound", st == Status::NotFound);
    }

    // ── Регистрируем устройство ──
    check("устройство зарегистрировано", g_i2c_bus.register_device(0x50u));

    // ── Запись ──
    {
        const uint8_t data[2] = { 0x10u, 0xABu };   // регистр 0x10 = 0xAB
        Size written = 0u;
        const Status st = port.write(dev, data, 2u, written, 20u);
        check("write() == Ok", st == Status::Ok);
        check_eq<Size>("записано 2 байта", written, 2u);
        check_eq<unsigned>("регистр устройства обновлён",
                           g_i2c_bus.regs[0x10u], 0xABu);
    }

    // ── Чтение ──
    {
        g_i2c_bus.regs[0x00u] = 0x5Au;
        uint8_t buf[4]{};
        Size got = 0u;
        const Status st = port.read(dev, buf, 1u, got, 20u);
        check("read() == Ok", st == Status::Ok);
        check_eq<Size>("прочитан 1 байт", got, 1u);
        check_eq<unsigned>("значение верное", buf[0], 0x5Au);
    }

    // ── writeRead: типовой доступ к регистру ──
    {
        g_i2c_bus.regs[0x1Fu] = 0xC3u;
        uint8_t buf[1]{};
        Size got = 0u;
        const Status st = port.readRegister(dev, 0x1Fu, buf[0], 20u);
        (void)got;
        check("readRegister() == Ok", st == Status::Ok);
        check_eq<unsigned>("регистр 0x1F == 0xC3", buf[0], 0xC3u);
    }

    // ── writeRegister ──
    {
        const Status st = port.writeRegister(dev, 0x01u, 0x77u, 20u);
        check("writeRegister() == Ok", st == Status::Ok);
        check_eq<unsigned>("регистр 0x01 обновлён", g_i2c_bus.regs[0x01u], 0x77u);
    }

    // ── Много байт за раз ──
    {
        for (unsigned i = 0u; i < 8u; ++i) g_i2c_bus.regs[i] = static_cast<uint8_t>(i + 1u);
        g_i2c_bus.last_reg = 0u;

        uint8_t buf[8]{};
        Size got = 0u;
        const Status st = port.read(dev, buf, 8u, got, 20u);
        check("пакетное чтение == Ok", st == Status::Ok);
        check_eq<Size>("прочитано 8 байт", got, 8u);
        bool seq_ok = true;
        for (unsigned i = 0u; i < 8u; ++i) {
            if (buf[i] != static_cast<uint8_t>(i + 1u)) seq_ok = false;
        }
        check("последовательность верна", seq_ok);
    }

    // ── Ошибка шины ──
    {
        g_i2c_bus.fail_next = true;
        uint8_t buf[1]{};
        Size got = 0u;
        const Status st = port.read(dev, buf, 1u, got, 20u);
        check("ошибка шины -> HardwareError", st == Status::HardwareError);
    }

    // ── Залипшая шина и recover() ──
    {
        g_i2c_bus.bus_stuck = true;
        uint8_t buf[1]{};
        Size got = 0u;
        check("залипшая шина -> HardwareError",
              port.read(dev, buf, 1u, got, 20u) == Status::HardwareError);

        check("recover() == Ok", drv.recover() == Status::Ok);
        check("после recover() шина работает",
              port.read(dev, buf, 1u, got, 20u) == Status::Ok ||
              got > 0u);
    }

    // ── Сканирование шины ──
    {
        g_i2c_bus.reset();
        check("устройство 0x50 на шине", g_i2c_bus.register_device(0x50u));
        check("устройство 0x68 на шине", g_i2c_bus.register_device(0x68u));

        bool found[128]{};
        const Status st = port.scan(found, 5u);
        check("scan() == Ok", st == Status::Ok);
        check("найден 0x50", found[0x50u]);
        check("найден 0x68", found[0x68u]);
        check("0x51 не найден", !found[0x51u]);
    }

    // ── Неверные аргументы ──
    {
        uint8_t buf[1]{};
        Size got = 0u;
        check("невалидный адрес 0 -> InvalidArg",
              port.read(TestAddr{0u}, buf, 1u, got, 10u) == Status::InvalidArg);
        check("nullptr буфер -> InvalidArg",
              port.read(dev, nullptr, 1u, got, 10u) == Status::InvalidArg);
    }
}

// ─────────────────────── Драйверный уровень (без порта) ─────────────────────

void test_i2c_driver_level() {
    (void)std::puts("i2c driver-level (start*/completion):");

    using Drv = I2cMockPolling;
    Drv& drv = Drv::instance();
    g_i2c_bus.reset();
    (void)g_i2c_bus.register_device(0x50u);

    // startWrite + completion
    {
        completion token;
        token.reset(WaitPolicy::Forever);
        check("setCompletion() == Ok", drv.setCompletion(token) == Status::Ok);

        const uint8_t data[2] = { 0x05u, 0x99u };
        const Status st = drv.startWrite(TestAddr{0x50u}, data, 2u);
        check("startWrite() == Ok", st == Status::Ok);
        check("операция завершена сразу (mock)", token.done());
        check("статус Ok", token.status() == Status::Ok);
        check_eq<unsigned>("регистр записан", g_i2c_bus.regs[0x05u], 0x99u);
    }

    // Завершение уже запущенной операции второй раз отклоняется
    {
        completion token;
        token.reset(WaitPolicy::Forever);
        (void)drv.setCompletion(token);
        (void)drv.startWrite(TestAddr{0x50u}, "ab", 2u);
        check("повторный post() отклонён", !token.post_ok(1u));
        check_eq<Size>("переданный объём не испорчен", token.transferred(), 2u);
    }

    // startRead: адрес регистра берётся из последней записи (как в железе),
    // поэтому выставляем его явно.
    {
        g_i2c_bus.regs[0x07u] = 0x42u;
        g_i2c_bus.last_reg = 0x07u;
        completion token;
        token.reset(WaitPolicy::Forever);
        (void)drv.setCompletion(token);
        uint8_t out[1]{};
        check("startRead() == Ok",
              drv.startRead(TestAddr{0x50u}, out, 1u) == Status::Ok);
        check_eq<unsigned>("прочитан регистр 0x07", out[0], 0x42u);
    }

    // startWriteRead
    {
        g_i2c_bus.regs[0x0Au] = 0x11u;
        completion token;
        token.reset(WaitPolicy::Forever);
        (void)drv.setCompletion(token);
        const uint8_t reg = 0x0Au;
        uint8_t out[1]{};
        check("startWriteRead() == Ok",
              drv.startWriteRead(TestAddr{0x50u}, &reg, 1u, out, 1u) == Status::Ok);
        check_eq<unsigned>("регистр 0x0A == 0x11", out[0], 0x11u);
    }

    // probe
    {
        check("probe() существующего устройства == Ok",
              drv.probe(TestAddr{0x50u}, 10u) == Status::Ok);
        check("probe() отсутствующего -> NotFound",
              drv.probe(TestAddr{0x33u}, 10u) == Status::NotFound);
    }

    // Превышение предела одного обмена
    {
        static uint8_t big[300]{};
        completion token;
        token.reset(WaitPolicy::Forever);
        (void)drv.setCompletion(token);
        check("длина > 255 -> InvalidArg",
              drv.startWrite(TestAddr{0x50u}, big, 300u) == Status::InvalidArg);
    }
}

// ─────────────────────── Драйвер UART без порта ────────────────────────────

void test_uart_driver_level() {
    (void)std::puts("uart driver-level:");

    using Drv = UartMockAsync;
    Drv& drv = Drv::instance();
    g_uart_bus.reset();
    g_uart_bus.tx_ticks_per_byte = 2u;    // передача занимает время

    // Операция не завершается мгновенно: завершение придёт по «прерыванию».
    completion token;
    token.reset(WaitPolicy::Timeout, 100u);
    check("setCompletion() == Ok", drv.setCompletion(token) == Status::Ok);
    token.begin();

    const Status st = drv.startWrite("XYZ", 3u);
    check("startWrite() == Ok", st == Status::Ok);
    check("сразу после старта операция не завершена", !token.done());

    // «Прерывания» продвигают передачу.
    unsigned guard = 0u;
    while (!token.done() && guard++ < 64u) {
        drv.onTxIrq();
    }
    check("передача завершилась по прерываниям", token.done());
    check_eq<Size>("передано 3 байта", token.transferred(), 3u);
    check("данные дошли", g_uart_bus.tx_len >= 3u);
    check("содержимое верное", std::memcmp(g_uart_bus.tx, "XYZ", 3u) == 0);

    // Повторный setCompletion во время операции должен быть отклонён.
    {
        g_uart_bus.reset();
        g_uart_bus.tx_ticks_per_byte = 100u;
        completion busy_token;
        busy_token.reset(WaitPolicy::Timeout, 10u);
        (void)drv.setCompletion(busy_token);
        (void)drv.startWrite("A", 1u);

        // setCompletion() заменяет объект (владелец — порт), поэтому
        // отказ проверяем на уровне запуска: драйвер уже занят.
        completion second;
        second.reset(WaitPolicy::Forever);
        (void)drv.setCompletion(second);
        check("передача во время активной -> Busy",
              drv.startWrite("B", 1u) == Status::Busy);

        (void)drv.abort();
        g_uart_bus.tx_ticks_per_byte = 0u;
    }

    // abort завершает АКТИВНУЮ операцию со статусом Aborted.
    // Длительность задаём до старта: иначе передача успела бы завершиться
    // ещё внутри startWrite, и прерывать было бы нечего.
    {
        g_uart_bus.reset();
        g_uart_bus.tx_ticks_per_byte = 50u;

        completion t2;
        t2.reset(WaitPolicy::Timeout, 100u);
        (void)drv.setCompletion(t2);
        check("startWrite() начал длинную передачу",
              drv.startWrite("Q", 1u) == Status::Ok);
        check("операция ещё не завершена", !t2.done());

        check("abort() == Ok", drv.abort() == Status::Ok);
        check("статус Aborted", t2.status() == Status::Aborted);
        g_uart_bus.tx_ticks_per_byte = 0u;
    }

    // Данных в приёмнике нет -> операция ждёт, потом таймаут.
    {
        g_uart_bus.reset();
        g_uart_bus.rx_idle = false;      // линия занята, данных пока нет
        uint8_t buf[4]{};
        Size got = 0u;
        completion t3;
        t3.reset(WaitPolicy::Timeout, 10u);
        (void)drv.setCompletion(t3);
        t3.begin();
        (void)drv.startRead(buf, sizeof buf);
        // Данные не поступают: ждём до таймаута.
        (void)t3.wait();
        check("нет данных -> Timeout", t3.status() == Status::Timeout);
        check_eq<Size>("прочитано 0", got, 0u);
    }
}

// ─────────────── Стратегия: Config + проверка у драйверов (mock) ─────────────
//
// Тот же API, что и у STM32-драйверов (uart_stm32.hpp/i2c_stm32.hpp), где
// Config::strategy РЕАЛЬНО выбирает режим. У mock «железо» фиксировано
// шаблоном, поэтому запрос другого режима обязан давать NotSupported —
// иначе прикладной код молча работал бы не в том режиме, который просил.

void test_strategy_api() {
    (void)std::puts("стратегия: Config и strategy() у mock-драйверов:");

    // ── UART ──
    {
        UartMockPolling::Config cfg{};
        cfg.baudRate = 115200u;
        check("UartMockPolling: strategy() == Polling",
              UartMockPolling::strategy() == async::Strategy::Polling);
        check("UartMockPolling: своя стратегия принята",
              UartMockPolling::instance().configure(cfg) == Status::Ok);

        cfg.strategy = async::Strategy::Dma;
        check("UartMockPolling: чужая стратегия -> NotSupported",
              UartMockPolling::instance().configure(cfg) == Status::NotSupported);

        UartMockAsync::Config acfg{};
        acfg.baudRate = 115200u;
        check("UartMockAsync: strategy() == Async",
              UartMockAsync::strategy() == async::Strategy::Async);
        check("UartMockAsync: configure() == Ok",
              UartMockAsync::instance().configure(acfg) == Status::Ok);

        async::UartPort<UartMockDma> port;
        check("порт видит стратегию драйвера",
              port.strategy() == async::Strategy::Dma);
    }

    // ── I2C ──
    {
        I2cMockDma::Config cfg{};
        cfg.peripheralClockHz = 36000000u;
        check("I2cMockDma: strategy() == Dma",
              I2cMockDma::strategy() == async::Strategy::Dma);
        check("I2cMockDma: configure() == Ok",
              I2cMockDma::instance().configure(cfg) == Status::Ok);

        cfg.strategy = async::Strategy::Polling;
        check("I2cMockDma: чужая стратегия -> NotSupported",
              I2cMockDma::instance().configure(cfg) == Status::NotSupported);

        async::I2cPort<I2cMockAsync> port;
        check("порт I2C видит стратегию драйвера",
              port.strategy() == async::Strategy::Async);
    }
}

// ───────────────────────── Запуск ядра для тестов ─────────────────────────

task     g_tick_tcb;
uint32_t g_tick_stack[64];

void tick_body(void*) noexcept {
    for (;;) {
        (void)scheduler::instance().sleep_for(1);
    }
}

bool start_kernel() {
    if (g_memory_manager.init(2048u, 0u) != Status::Ok) return false;

    config::OsTimer::Config cfg{};
    cfg.default_tick_ms = 1;
    if (config::OsTimer::Init(cfg) != Status::Ok) return false;
    if (config::OsTimer::RegisterSystemHook(&scheduler::tick_hook, nullptr) != Status::Ok) {
        return false;
    }
    if (config::OsTimer::Start() != Status::Ok) return false;

    scheduler& sched = scheduler::instance();
    if (sched.init() != Status::Ok) return false;
    if (g_tick_tcb.init(&tick_body, nullptr, "tick", 64u, 1u) != Status::Ok) return false;
    if (!sched.add_task(&g_tick_tcb, g_tick_stack, 64u)) return false;
    return true;
}

} // namespace

int main() {
    (void)std::puts("=== UART / I2C tests (mock) ===");

    check("старт ядра", start_kernel());

    test_strategy_api();

    // Один и тот же набор проверок для всех трёх стратегий:
    // это и доказывает, что результат не зависит от стратегии.
    test_uart_linear<async::Strategy::Polling, UartMockPolling>("Polling");
    test_uart_linear<async::Strategy::Async,   UartMockAsync>("Async");
    test_uart_linear<async::Strategy::Dma,     UartMockDma>("Dma");

    test_i2c_linear<async::Strategy::Polling, I2cMockPolling>("Polling");
    test_i2c_linear<async::Strategy::Async,   I2cMockAsync>("Async");
    test_i2c_linear<async::Strategy::Dma,     I2cMockDma>("Dma");

    test_uart_driver_level();
    test_i2c_driver_level();

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed != 0) {
        (void)std::printf(", провалено: %d\n", g_failed);
        return 1;
    }
    (void)std::printf(" — UART и I2C работают\n");
    return 0;
}
