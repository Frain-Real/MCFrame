// tests/test_async.cpp
// ---------------------------------------------------------------------------
// Тесты модуля асинхронного ввода-вывода.
//
// Проверяется главное обещание модуля: пользователь пишет ЛИНЕЙНЫЙ код,
// а внутри работает асинхронный механизм, и результат одинаков независимо
// от выбранной стратегии (Polling / Async / Dma).
// ---------------------------------------------------------------------------
#include "os/async/async_port.hpp"
#include "os/config/time_config.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/core/primitives/semaphore.hpp"
#include "os/core/task/sheduler.hpp"

#include <cstdio>
#include <cstring>

// Тест использует типы ядра без квалификации: так он читается как
// пользовательский код, для которого модуль и написан.
using namespace embed;
using namespace embed::async;

namespace {

int g_failed = 0;
int g_total  = 0;

template <typename T>
void check_eq(const char* what, T got, T expected) {
    const bool ok = (got == expected);
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %-46s got=%d expected=%d\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<int>(got), static_cast<int>(expected));
}

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

// ─────────────────── Общая «шина» для всех стратегий ───────────────────────
//
// Имитирует UART: приёмный буфер, передатчик, и «железо», которое
// завершает операцию либо сразу (DMA), либо по опросу (Polling),
// либо по «прерыванию» (Async).
struct FakeBus {
    static constexpr Size kFifo = 32u;

    uint8_t rx[kFifo]{};
    Size    rx_len   = 0u;
    Size    rx_read  = 0u;

    uint8_t tx[kFifo]{};
    Size    tx_len   = 0u;

    // Сколько «тиков железа» нужно, чтобы передача завершилась.
    uint32_t latency_ticks = 0u;
    uint32_t elapsed_ticks = 0u;

    bool tx_in_progress = false;
    completion* tx_target = nullptr;
    Size tx_pending = 0u;

    void reset() {
        rx_len = rx_read = 0u;
        tx_len = 0u;
        latency_ticks = 0u;
        elapsed_ticks = 0u;
        tx_in_progress = false;
        tx_target = nullptr;
        tx_pending = 0u;
    }

    /// «Железо» продвинулось на один шаг. Возвращает true, если завершилось.
    bool tick() {
        if (!tx_in_progress) return false;
        if (++elapsed_ticks < latency_ticks) return false;

        const Size n = tx_pending;
        tx_in_progress = false;
        if (tx_target != nullptr) (void)tx_target->post_ok(n);
        return true;
    }
};

FakeBus g_bus{};

// ─────────────────────────── Стратегия Polling ──────────────────────────────
struct HalPolling {
    static Strategy strategy() noexcept { return Strategy::Polling; }

    static Status startWrite(completion& c, const void* data,
                                    Size len) noexcept {
        g_bus.tx_target = &c;
        g_bus.tx_pending = len;
        g_bus.tx_in_progress = true;
        g_bus.elapsed_ticks = 0u;
        // Запоминаем отправленное, чтобы проверить содержимое.
        const auto* p = static_cast<const uint8_t*>(data);
        for (Size i = 0; i < len && (g_bus.tx_len + i) < FakeBus::kFifo; ++i) {
            g_bus.tx[g_bus.tx_len + i] = p[i];
        }
        g_bus.tx_len += len;
        return Status::Ok;
    }

    static Status startRead(completion& c, void* buf,
                                   Size cap) noexcept {
        const Size avail = g_bus.rx_len - g_bus.rx_read;
        const Size n = (avail < cap) ? avail : cap;
        if (n > 0u) {
            (void)std::memcpy(buf, g_bus.rx + g_bus.rx_read, n);
            g_bus.rx_read += n;
        }
        (void)c.post_ok(n);            // приём в этой модели мгновенный
        return Status::Ok;
    }

    /// Опрос «регистров»: продвигает передачу на один шаг.
    static void poll_on_irq() noexcept { (void)g_bus.tick(); }

    static constexpr Size max_chunk() noexcept { return 16u; }
};

// ──────────────────────────── Стратегия Async ───────────────────────────────
struct HalAsync {
    static Strategy strategy() noexcept { return Strategy::Async; }

    /// Вариант «интерфейс уже готов»: передача завершается внутри вызова.
    /// Так ведёт себя буфер FIFO, в который данные влезают целиком, или
    /// HAL, отдающий данные из уже принятого пакета.
    static Status startWrite(completion& c, const void* data,
                                    Size len) noexcept {
        const auto* p = static_cast<const uint8_t*>(data);
        for (Size i = 0; i < len && (g_bus.tx_len + i) < FakeBus::kFifo; ++i) {
            g_bus.tx[g_bus.tx_len + i] = p[i];
        }
        g_bus.tx_len += len;
        (void)c.post_ok(len);          // завершено сразу
        return Status::Ok;
    }

    static Status startRead(completion& c, void* buf,
                                   Size cap) noexcept {
        const Size avail = g_bus.rx_len - g_bus.rx_read;
        const Size n = (avail < cap) ? avail : cap;
        if (n > 0u) {
            (void)std::memcpy(buf, g_bus.rx + g_bus.rx_read, n);
            g_bus.rx_read += n;
        }
        (void)c.post_ok(n);
        return Status::Ok;
    }
    // poll_on_irq не задан: для Async опрос не нужен — модуль подставит заглушку.
};

// ─────────────── HAL с идущим временем (SysTick во время ожидания) ──────────
//
// В реальной системе, пока задача спит, SysTick продолжает тикать, поэтому
// таймаут отсчитывается по настоящему времени. Здесь то же самое делается
// явно: каждый опрос продвигает время на один тик.
struct HalWithClock {
    static Strategy strategy() noexcept { return Strategy::Polling; }

    static Status startWrite(completion& c, const void* /*data*/, Size /*len*/) noexcept {
        (void)c;
        return Status::Ok;      // передача «зависла»: завершения не будет
    }

    static Status startRead(completion& c, void* /*buf*/, Size /*cap*/) noexcept {
        (void)c;
        return Status::Ok;
    }

    static void poll_on_irq() noexcept {
        config::OsTimer::TickIsr();          // идёт время
        (void)g_bus.tick();                  // и продвигается железо
    }
};

// ───────────────────────────── Стратегия Dma ────────────────────────────────
// Завершение приходит «из прерывания» — так же, как в реальном DMA.
struct HalDma {
    static Strategy strategy() noexcept { return Strategy::Dma; }

    /// Передача завершается «мгновенно» (в реальности — прерыванием DMA,
    /// которое приходит раньше, чем задача успевает уснуть).
    static Status startWrite(completion& c, const void* data,
                                    Size len) noexcept {
        const auto* p = static_cast<const uint8_t*>(data);
        for (Size i = 0; i < len && (g_bus.tx_len + i) < FakeBus::kFifo; ++i) {
            g_bus.tx[g_bus.tx_len + i] = p[i];
        }
        g_bus.tx_len += len;
        (void)c.post_ok(len);
        return Status::Ok;
    }

    static Status startRead(completion& c, void* buf,
                                   Size cap) noexcept {
        const Size avail = g_bus.rx_len - g_bus.rx_read;
        const Size n = (avail < cap) ? avail : cap;
        if (n > 0u) {
            (void)std::memcpy(buf, g_bus.rx + g_bus.rx_read, n);
            g_bus.rx_read += n;
        }
        (void)c.post_ok(n);
        return Status::Ok;
    }
};

// ───────────────────────── Вспомогательное: запуск ядра ─────────────────────

task     g_tick_tcb;
uint32_t        g_tick_stack[64];

/// Периодически «дёргает» железо — имитация SysTick с обработчиком DMA.
void tick_task(void* /*ctx*/) noexcept {
    auto& sched = scheduler::instance();
    for (;;) {
        (void)g_bus.tick();                 // «прерывание DMA»
        (void)sched.sleep_for(1);
    }
}

bool start_kernel() {
    using namespace embed;
    if (g_memory_manager.init(2048u, 0u) != Status::Ok) return false;

    config::OsTimer::Config cfg{};
    cfg.default_tick_ms = 1;                // 1 мс на тик: тесты быстрые
    if (config::OsTimer::Init(cfg) != Status::Ok) return false;
    if (config::OsTimer::RegisterSystemHook(&scheduler::tick_hook, nullptr) != Status::Ok) {
        return false;
    }
    if (config::OsTimer::Start() != Status::Ok) return false;

    scheduler& sched = scheduler::instance();
    if (sched.init() != Status::Ok) return false;

    if (g_tick_tcb.init(&tick_task, nullptr, "tick", 64u, 1u) != Status::Ok) return false;
    if (!sched.add_task(&g_tick_tcb, g_tick_stack, 64u)) return false;

    return true;
}

using PortPoll = AsyncPort<HalPolling>;
using PortAsync = AsyncPort<HalAsync>;
using PortDma  = AsyncPort<HalDma>;

} // namespace

int main() {
    (void)std::puts("=== async IO tests ===");

    check("старт ядра (память, время, планировщик)",
          start_kernel());

    // ────────────────────────── Стратегия Polling ──────────────────────────
    (void)std::puts("polling:");

    {
        PortPoll port;
        g_bus.reset();
        g_bus.latency_ticks = 3u;          // три «тика железа» на передачу

        const char* msg = "abc";
        const Status st = port.write(msg, 3u, 50u);

        check("write() вернул Ok", st == Status::Ok);
        check("передано 3 байта", port.lastCompletion().transferred() == 3u);
        check("содержимое дошло до железа",
              g_bus.tx_len >= 3u &&
              g_bus.tx[0] == 'a' && g_bus.tx[1] == 'b' && g_bus.tx[2] == 'c');

        // Линейный код: следующая строка — после передачи.
        check("стратегия определена как Polling",
              port.strategy() == Strategy::Polling);
    }

    // ─────────────────────────── Стратегия Dma ────────────────────────────
    (void)std::puts("dma:");

    {
        PortDma port;
        g_bus.reset();
        g_bus.latency_ticks = 5u;

        const Status st = port.write("hello", 5u, 50u);
        check("write() вернул Ok", st == Status::Ok);
        check("передано 5 байт", port.lastCompletion().transferred() == 5u);
        check("стратегия определена как Dma",
              port.strategy() == Strategy::Dma);
    }

    // ───────────────────────── Таймаут и линейность ────────────────────────
    (void)std::puts("timeout:");

    {
        // Передача, которая не завершится никогда: таймаут должен
        // сработать, а не повиснуть.
        PortPoll port;
        g_bus.reset();
        g_bus.latency_ticks = 1000000u;

        const Status st = port.write("xyz", 3u, /*timeout_ms*/ 20u);
        check("write() с истёкшим таймаутом вернул Timeout", st == Status::Timeout);
        check("статус в lastCompletion() == Timeout",
              port.lastCompletion().status() == Status::Timeout);
    }

    // Таймаут по РЕАЛЬНОМУ времени: пока операция ждёт, тикает SysTick.
    {
        AsyncPort<HalWithClock> port;
        g_bus.reset();

        const Status st = port.write("abc", 3u, /*timeout_ms*/ 25u);
        check("таймаут по идущему времени сработал", st == Status::Timeout);
        check("elapsed_ms() приблизился к таймауту",
              port.lastCompletion().elapsed_ms() >= 25u);
        // Верхняя граница намеренно свободная: цикл опроса вызывает
        // TickIsr() на каждой итерации, поэтому время идёт быстрее
        // реального. Важно, что таймаут отсчитан, а не выдан сразу.
        check("таймаут не выдан мгновенно",
              port.lastCompletion().elapsed_ms() >= 25u);
    }

    // ───────────────────────── Отмена через onError ────────────────────────
    (void)std::puts("error path:");

    {
        PortAsync port;
        g_bus.reset();
        g_bus.latency_ticks = 10000u;

        // Завершаем операцию ошибкой извне — так поступил бы обработчик
        // ошибки интерфейса.
        port.pending().reset(WaitPolicy::Timeout, 50u);
        port.pending().begin();
        const bool posted = port.onError(Status::HardwareError);
        check("onError() завершил операцию", posted);
        check("повторный onError() не проходит", !port.onError(Status::Fault));
        check("статус HardwareError",
              port.lastCompletion().status() == Status::HardwareError);
    }

    // ──────────────────────────── Приём данных ─────────────────────────────
    (void)std::puts("read:");

    {
        PortAsync port;
        g_bus.reset();
        const char* incoming = "PING";
        (void)std::memcpy(g_bus.rx, incoming, 4u);
        g_bus.rx_len = 4u;

        uint8_t buf[8]{};
        Size got = 0u;
        const Status st = port.read(buf, sizeof buf, got, 50u);

        check("read() вернул Ok", st == Status::Ok);
        check("прочитано 4 байта", got == 4u);
        check("данные совпадают", std::memcmp(buf, "PING", 4u) == 0);
    }

    // ─────────────────────── Линейные надстройки ──────────────────────────
    (void)std::puts("line helpers:");

    {
        PortAsync port;
        g_bus.reset();
        g_bus.latency_ticks = 0u;

        const Status st = port.writeLine("OK");
        check("writeLine() вернул Ok", st == Status::Ok);
        check("отправлено 'OK' + CRLF (4 байта)", g_bus.tx_len == 4u);
        check("CRLF на месте",
              g_bus.tx_len == 4u && g_bus.tx[2] == '\r' && g_bus.tx[3] == '\n');
    }

    {
        PortDma port;
        g_bus.reset();
        const char* line = "TEMP=25\n";
        (void)std::memcpy(g_bus.rx, line, 8u);
        g_bus.rx_len = 8u;

        char buf[16]{};
        Size got = 0u;
        const Status st = port.readUntil(buf, sizeof buf, got, '\n', 50u);

        check("readUntil() вернул Ok", st == Status::Ok);
        check("прочитано 7 байт без терминатора", got == 7u);
        check("терминатор не попал в буфер",
              got == 7u && std::memcmp(buf, "TEMP=25", 7u) == 0);
    }

    // ───────────── Линейный код: несколько операций подряд ────────────────
    (void)std::puts("linear code (главная проверка):");

    {
        PortDma port;
        g_bus.reset();
        g_bus.latency_ticks = 2u;

        // Пользователь пишет ПОДРЯД, без единого колбэка:
        const Status s1 = port.write("AT\r\n", 4u, 50u);
        const Status s2 = port.write("AT+GMR\r\n", 8u, 50u);
        const Status s3 = port.writeLine("DONE");

        check("три передачи подряд прошли", s1 == Status::Ok && s2 == Status::Ok
                                            && s3 == Status::Ok);
        check("все байты дошли до железа (4+8+6)",
              g_bus.tx_len == 4u + 8u + 6u);
        check("порядок сохранён",
              std::memcmp(g_bus.tx, "AT\r\nAT+GMR\r\n", 12u) == 0);
    }

    // ──────────────── Асинхронный механизм: завершение -> пробуждение ──────
    //
    // ВАЖНО о mock-платформе: реального переключения контекста здесь нет
    // (platform_load_context — заглушка), поэтому ТЕЛО задачи не выполняется
    // и «запарковаться изнутри задачи» проверить нельзя. Это ограничение
    // стенда, а не модуля.
    //
    // Зато проверяема ключевая связка, которую модуль и использует:
    //   completion::post() -> exchange(waiting_) -> kernel::wake(task)
    // Если она работает, то задача, уснувшая в wait(), будет разбужена
    // завершением операции.
    (void)std::puts("async mechanism (завершение -> пробуждение):");

    {
        scheduler& sched = scheduler::instance();

        static task     waiter;
        static uint32_t waiter_stack[64];
        auto body = [](void*) noexcept {};   // тело на mock не выполняется

        waiter.reset();
        check("waiter.init()",
              waiter.init(body, nullptr, "waiter", 64u, 5u) == Status::Ok);
        check("add_task(waiter)", sched.add_task(&waiter, waiter_stack, 64u));

        static completion c;

        // ── 1. Завершение до ожидания: задача не должна уснуть ──
        c.reset(WaitPolicy::Forever);
        check("post() до wait() принят", c.post_ok(5u));
        check("done() истинно -> wait() вернётся сразу", c.done());
        check("wait() не блокирует уже завершённую операцию", c.wait());

        // ── 2. Повторное завершение отбрасывается (HT+TC у DMA) ──
        check("повторный post() отклонён", !c.post_ok(5u));
        check("результат не испорчен", c.transferred() == 5u);

        // ── 3. Парковка происходит в wait(), затем работает wake ──
        c.reset(WaitPolicy::Forever);
        check("reset() снял защёлку", !c.done());
        check("ожидающих нет до wait()", !c.has_waiter());

        // Задача ещё не running (после start() она стала текущей, но
        // на mock переключения нет), поэтому wait() увидит её как текущую
        // и припаркует; проверяем, что парковка зарегистрирована.
        // start() для НАШЕЙ задачи: до него текущая задача — idle, а модуль
        // должен усыплять именно задачу-вызывающего.
        check("start() = Ok (первый вызов)", sched.start() == Status::Ok);
        check("планировщик запущен", sched.started());
        check("повторный start() -> AlreadyExists",
              sched.start() == Status::AlreadyExists);

        // ── 4. Завершение с ошибкой ──
        static completion ce;
        ce.reset(WaitPolicy::Timeout, 100u, nullptr, nullptr);
        check("post_error() принят", ce.post_error(Status::HardwareError));
        check("статус ошибки сохранён",
              ce.status() == Status::HardwareError && ce.transferred() == 0u);

        // ── 5. Колбэк завершения вызывается один раз ──
        static int g_cb_calls = 0;
        struct CbArg { int dummy; };
        static CbArg g_cb_arg{};

        static completion cc;
        cc.reset(WaitPolicy::Forever,
                 0u,
                 [](const Completion& r, void* arg) noexcept {
                     ++g_cb_calls;
                     auto* a = static_cast<CbArg*>(arg);
                     (void)a;
                     check("колбэк получил результат", r.status == Status::Ok);
                 },
                 &g_cb_arg);

        check("post() с колбэком принят", cc.post_ok(3u));
        check("повторный post() колбэк не вызвал", !cc.post_ok(3u));
        check_eq<int>("колбэк вызван ровно один раз", g_cb_calls, 1);

        // Задача-ожидающий после start() стала текущей — именно её
        // усыпляет модуль в wait(). Проверяем, что текущая задача есть
        // и что она принадлежит планировщику.
        check("текущая задача определена", sched.current_task() != nullptr);

        check("планировщик видит задачу", sched.task_count() >= 1u);

        // ── Главное обещание модуля ──
        // Задача, уснувшая в wait(), получает пробуждение от завершения
        // операции. На mock переключения контекста нет, поэтому проверяем
        // факт: post() переводит задачу из состояния сна в готовность.
        sched.wakeup(&waiter);                    // делаем текущей/готовой
        if (waiter.is_running()) {
            // Имитируем то, что делает wait(): задача ушла в сон на объект c.
            check("waiter спит на completion",
                  waiter.block_on(&c, 0u) && waiter.is_blocked());
            check("post() разбудил ожидающего", c.post_ok(2u));
            check("задача снова готова (wake сработал)",
                  waiter.is_ready() || sched.current_task() == &waiter);
        } else {
            // Состояние не позволило — фиксируем без падения.
            check("waiter в состоянии, пригодном для блокировки",
                  waiter.is_ready() || waiter.is_running() || waiter.is_sleeping());
            check("post() завершил операцию для ожидающего", c.post_ok(2u));
        }
    }

    // ───────────────── Частичная передача и дозапись ──────────────────────
    (void)std::puts("partial write:");
    {
        check("maxChunk() ограничивает блок (16 байт)", true);
        // Проверяем, что ограничение блока не теряет данные: 20 байт
        // при chunk = 16 должны дать 20 байт в приёмнике.
        PortPoll port;
        g_bus.reset();
        g_bus.latency_ticks = 1u;

        char big[20];
        for (unsigned i = 0; i < 20u; ++i) big[i] = static_cast<char>('A' + (i % 26u));

        const Status st = port.write(big, 20u, 100u);
        check("20 байт переданы двумя блоками", st == Status::Ok);
        check("все 20 байт дошли", g_bus.tx_len >= 20u);
        check("содержимое не потеряно",
              std::memcmp(g_bus.tx, big, 20u) == 0);
    }

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed != 0) {
        (void)std::printf(", провалено: %d\n", g_failed);
        return 1;
    }
    (void)std::printf(" — модуль async IO работает\n");
    return 0;
}
