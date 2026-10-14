// tests/host/test_dma_arbiter.cpp
// ---------------------------------------------------------------------------
// ТЕСТЫ АРБИТРА КАНАЛОВ DMA (dma_arbiter.hpp).
//
// Здесь проверяется САМА АРБИТРАЦИЯ, без драйверов и без железа: работы —
// заглушки, которые сами решают, когда завершиться. Так видно то, что в
// интеграционных тестах утонуло бы в деталях обмена:
//
//   * работа, которой канал занят, ЖДЁТ и выполняется, когда канал освободят
//     (ровно тот случай, из-за которого арбитр и появился: USART1_TX и
//     SPI2_RX/I2C2_TX — это один и тот же DMA1 Channel4);
//   * работы с разными каналами идут ПАРАЛЛЕЛЬНО (очередь не глобальная);
//   * на одном интерфейсе одновременно идёт не более одной работы;
//   * порядок FIFO;
//   * работа с парой каналов захватывает их РАЗОМ и не встаёт взаимной
//     блокировкой с другой такой же;
//   * завершение прямо внутри start() не ломает раздачу (рекурсия);
//   * ошибка старта, таймаут в очереди и отмена освобождают канал и слот.
//
// Контракт platform_dma_* реализован моделью DMA1 (tests/host/fake_dma.cpp) —
// той же, что и в тестах HAL-логики.
// ---------------------------------------------------------------------------
#include "fake_dma.hpp"

#include "os/config/dma_config.hpp"
#include "os/interfaces/dma_arbiter.hpp"
#include "os/interfaces/dma_manager.hpp"

#include <cstdio>

using namespace embed;

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
    (void)std::printf("  [%s] %-46s got=%ld expected=%ld\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<long>(got), static_cast<long>(expected));
}

DmaArbiter& arb()   { return DmaArbiter::instance(); }
DmaManager& mgr()   { return DmaManager::instance(); }

/// Работа-заглушка: тест сам решает, когда она завершится.
struct StubOp {
    const char* name = "stub";
    Size   bytes     = 0u;
    Status start_result = Status::Ok;   ///< что вернуть из start()
    bool   finish_in_start = false;     ///< завершиться прямо в start()

    unsigned starts  = 0u;
    unsigned cancels = 0u;

    /// Объект завершения, выданный арбитром (нужен тесту, чтобы завершить
    /// работу вручную). Живёт в слоте арбитра, пока работа не освобождена.
    async::completion* token = nullptr;

    static Status start(void* ctx, async::completion& token) noexcept {
        auto* op = static_cast<StubOp*>(ctx);
        ++op->starts;
        op->token = &token;
        if (op->finish_in_start) {
            (void)token.post_ok(op->bytes);
            return Status::Ok;
        }
        return op->start_result;
    }

    static void cancel(void* ctx) noexcept {
        auto* op = static_cast<StubOp*>(ctx);
        ++op->cancels;
        if (op->token != nullptr) (void)op->token->post_error(Status::Aborted);
    }

    /// Завершить работу «из прерывания».
    bool finish(Status st = Status::Ok) noexcept {
        if (token == nullptr) return false;
        const bool ok = (st == Status::Ok) ? token->post_ok(bytes)
                                           : token->post_error(st);
        return ok;
    }
};

/// Собрать работу-заглушку для канала(ов).
DmaJob makeJob(StubOp& op, config::DmaRequest tx,
               config::DmaRequest rx = config::DmaRequest::None) {
    DmaJob job{};
    job.tx     = tx;
    job.rx     = rx;
    job.owner  = &op;             // каждый StubOp — свой «интерфейс»
    job.start  = &StubOp::start;
    job.cancel = &StubOp::cancel;
    job.ctx    = &op;
    job.name   = op.name;
    return job;
}

// ───────────────────────── Канал занят: работа ждёт ─────────────────────────

void test_waits_for_channel() {
    (void)std::puts("арбитр: работа ждёт занятый канал (USART1_TX = SPI2_RX = DMA1 Ch4):");

    StubOp hog;
    StubOp a; a.name = "uart1";  a.bytes = 4u;
    StubOp b; b.name = "i2c2";   b.bytes = 8u;

    // «Соседний интерфейс» занял Channel4 (SPI2_RX — тот же канал, что USART1_TX).
    DmaTicket t_hog{};
    check("сосед принят в работу", arb().submit(makeJob(hog, config::DmaRequest::Spi2Rx), t_hog) == Status::Ok);
    check_eq<unsigned>("сосед начал обмен", hog.starts, 1u);

    // Обе работы претендуют на тот же канал — и обе приняты, а не отклонены.
    DmaTicket t_a{}, t_b{};
    check("работа A принята (не Busy)",
          arb().submit(makeJob(a, config::DmaRequest::Usart1Tx), t_a) == Status::Ok);
    check("работа B принята (не Busy)",
          arb().submit(makeJob(b, config::DmaRequest::Usart1Tx), t_b) == Status::Ok);
    check_eq<unsigned>("A ещё не начиналась", a.starts, 0u);
    check_eq<unsigned>("B ещё не начиналась", b.starts, 0u);
    check_eq<Size>("в очереди 2 работы", arb().queued(), 2u);
    check_eq<unsigned>("в работе 1 (сосед)", arb().stats().running, 1u);

    // Сосед отпустил канал — очередь поехала, по порядку.
    check("сосед завершился", hog.finish());
    check_eq<unsigned>("A пошла", a.starts, 1u);
    check_eq<unsigned>("B ещё ждёт", b.starts, 0u);
    check("A завершилась", a.finish());
    check_eq<unsigned>("B пошла после A", b.starts, 1u);
    check("B завершилась", b.finish());

    check("результат A — Ok", arb().status(t_a) == Status::Ok);
    check_eq<Size>("объём A", arb().transferred(t_a), 4u);
    check("результат B — Ok", arb().status(t_b) == Status::Ok);
    check_eq<Size>("объём B", arb().transferred(t_b), 8u);
    check("отмен не было", a.cancels == 0u && b.cancels == 0u);

    arb().release(t_a);
    arb().release(t_b);
    arb().release(t_hog);
    check("канал освобождён после всех работ",
          mgr().channelAvailable(config::DmaRequest::Usart1Tx));
}

// ─────────────────────── Разные каналы — параллельно ───────────────────────

void test_parallel_channels() {
    (void)std::puts("арбитр: работы на разных каналах идут параллельно:");

    StubOp uart_tx; uart_tx.bytes = 3u;
    StubOp i2c_tx;  i2c_tx.bytes  = 2u;

    DmaTicket t1{}, t2{};
    check("UART TX принят",
          arb().submit(makeJob(uart_tx, config::DmaRequest::Usart1Tx), t1) == Status::Ok);
    check("I2C TX принят",
          arb().submit(makeJob(i2c_tx, config::DmaRequest::I2c1Tx), t2) == Status::Ok);

    check_eq<unsigned>("UART TX в работе", uart_tx.starts, 1u);
    check_eq<unsigned>("I2C TX в работе", i2c_tx.starts, 1u);
    check_eq<unsigned>("две работы одновременно", arb().stats().running, 2u);
    check_eq<Size>("очередь пуста", arb().queued(), 0u);

    uart_tx.finish();
    i2c_tx.finish();
    check("оба результата Ok",
          arb().status(t1) == Status::Ok && arb().status(t2) == Status::Ok);

    arb().release(t1);
    arb().release(t2);
}

// ─────────────── Один интерфейс — одна работа (разные каналы) ───────────────

void test_one_job_per_interface() {
    (void)std::puts("арбитр: на одном интерфейсе — одна работа за раз:");

    StubOp tx; tx.bytes = 1u;
    StubOp rx; rx.bytes = 4u;

    // Один и тот же владелец (интерфейс), но каналы разные: TX — Ch4, RX — Ch5.
    // Владельца задаём явно: у драйвера один активный обмен, поэтому работы
    // на одном интерфейсе арбитр обязан выстроить в очередь, даже если каналы
    // у них разные.
    DmaJob job_rx = makeJob(rx, config::DmaRequest::Usart1Rx);
    job_rx.owner = &tx;

    DmaTicket t_tx{}, t_rx{};
    check("передача принята",
          arb().submit(makeJob(tx, config::DmaRequest::Usart1Tx), t_tx) == Status::Ok);
    check("приём принят (канал свободен, но интерфейс занят)",
          arb().submit(job_rx, t_rx) == Status::Ok);

    check_eq<unsigned>("передача идёт", tx.starts, 1u);
    check_eq<unsigned>("приём ждёт интерфейс", rx.starts, 0u);
    check_eq<unsigned>("в работе одна", arb().stats().running, 1u);

    tx.finish();
    check_eq<unsigned>("после передачи приём пошёл", rx.starts, 1u);

    rx.finish();
    arb().release(t_tx);
    arb().release(t_rx);
}

// ──────────────────────────── Пара каналов разом ────────────────────────────

void test_two_channels_all_or_nothing() {
    (void)std::puts("арбитр: работа с парой каналов захватывает их разом:");

    // «Чужой» держит Ch7 = I2C1_RX.
    StubOp hog; hog.bytes = 1u;
    DmaTicket t_hog{};
    check("сосед занял I2C1_RX",
          arb().submit(makeJob(hog, config::DmaRequest::I2c1Rx), t_hog) == Status::Ok);

    // Работа readRegister: пишет адрес регистра (Ch6) и читает (Ch7).
    StubOp wr; wr.bytes = 1u;
    DmaTicket t_wr{};
    check("writeRead принят",
          arb().submit(makeJob(wr, config::DmaRequest::I2c1Tx, config::DmaRequest::I2c1Rx),
                       t_wr) == Status::Ok);

    check_eq<unsigned>("writeRead ждёт (второй канал занят)", wr.starts, 0u);
    check("первый канал НЕ захвачен частично",
          mgr().channelAvailable(config::DmaRequest::I2c1Tx));

    hog.finish();
    check_eq<unsigned>("после освобождения writeRead пошёл", wr.starts, 1u);
    check("оба канала захвачены",
          !mgr().channelAvailable(config::DmaRequest::I2c1Tx) &&
          !mgr().channelAvailable(config::DmaRequest::I2c1Rx));

    wr.finish();
    arb().release(t_wr);
    arb().release(t_hog);
    check("оба канала освобождены",
          mgr().channelAvailable(config::DmaRequest::I2c1Tx) &&
          mgr().channelAvailable(config::DmaRequest::I2c1Rx));
}

// ──────────────────────────── Завершение в start() ──────────────────────────

void test_sync_completion_in_start() {
    (void)std::puts("арбитр: завершение прямо внутри start() не ломает очередь:");

    StubOp quick;
    quick.bytes = 5u;
    quick.finish_in_start = true;

    StubOp next_op; next_op.bytes = 2u;

    DmaTicket t1{}, t2{};
    check("мгновенная работа принята",
          arb().submit(makeJob(quick, config::DmaRequest::Usart1Tx), t1) == Status::Ok);
    check("следующая работа принята",
          arb().submit(makeJob(next_op, config::DmaRequest::Usart1Tx), t2) == Status::Ok);

    check("мгновенная работа завершена", arb().ready(t1));
    check_eq<Size>("объём передан", arb().transferred(t1), 5u);
    check_eq<unsigned>("следующая работа запущена", next_op.starts, 1u);

    next_op.finish();
    arb().release(t1);
    arb().release(t2);
    check("канал свободен", mgr().channelAvailable(config::DmaRequest::Usart1Tx));
}

// ──────────────────────────── Ошибка старта ────────────────────────────────

void test_start_failure_releases_channel() {
    (void)std::puts("арбитр: ошибка старта освобождает канал и слот:");

    StubOp bad;
    bad.start_result = Status::Busy;      // интерфейс занят обменом вне арбитра

    StubOp good; good.bytes = 1u;

    DmaTicket t_bad{}, t_good{};
    check("работа с ошибкой старта принята",
          arb().submit(makeJob(bad, config::DmaRequest::Usart1Tx), t_bad) == Status::Ok);
    check("её статус — Busy", arb().status(t_bad) == Status::Busy);
    check("канал освобождён",
          mgr().channelAvailable(config::DmaRequest::Usart1Tx));

    check("следующая работа принята",
          arb().submit(makeJob(good, config::DmaRequest::Usart1Tx), t_good) == Status::Ok);
    good.finish();
    check("следующая выполнилась", arb().status(t_good) == Status::Ok);

    arb().release(t_bad);
    arb().release(t_good);
}

// ─────────────────────────────── Таймаут и отмена ───────────────────────────

void test_cancel_and_timeout() {
    (void)std::puts("арбитр: отмена и таймаут снимают работу корректно:");

    StubOp hog; hog.bytes = 1u;
    DmaTicket t_hog{};
    (void)arb().submit(makeJob(hog, config::DmaRequest::Usart1Tx), t_hog);

    // ── Отмена работы, которая ещё стоит в очереди ──
    {
        StubOp queued;
        DmaTicket t{};
        (void)arb().submit(makeJob(queued, config::DmaRequest::Usart1Tx), t);
        check("работа в очереди", arb().state(t) == DmaArbiter::State::Queued);
        check("cancel() == Ok", arb().cancel(t) == Status::Ok);
        check("статус Aborted", arb().status(t) == Status::Aborted);
        check_eq<unsigned>("до обмена дело не дошло", queued.starts, 0u);
        arb().release(t);
        check("слот освобождён", arb().state(t) == DmaArbiter::State::Free);
    }

    // ── Таймаут работы в очереди: run() не ждёт вечно ──
    {
        StubOp waiting;
        Size done = 0u;
        const Status st = arb().run(makeJob(waiting, config::DmaRequest::Usart1Tx),
                                    /*timeout_ms*/ 1u, done);
        check("run() вернул Timeout", st == Status::Timeout);
        check_eq<unsigned>("работа так и не началась", waiting.starts, 0u);
        check_eq<Size>("очередь пуста", arb().queued(), 0u);
    }

    // ── Отмена работы, которая уже в обмене ──
    {
        StubOp running;
        DmaTicket t{};
        // Сосед освобождает канал — работа пойдёт.
        hog.finish();
        (void)arb().submit(makeJob(running, config::DmaRequest::Usart1Tx), t);
        check_eq<unsigned>("работа в обмене", running.starts, 1u);

        check("cancel() в обмене == Ok", arb().cancel(t) == Status::Ok);
        check_eq<unsigned>("драйверу передан abort", running.cancels, 1u);
        check("статус Aborted", arb().status(t) == Status::Aborted);
        check("канал освобождён", mgr().channelAvailable(config::DmaRequest::Usart1Tx));
        arb().release(t);
    }

    arb().release(t_hog);
    check("итог: канал свободен", mgr().channelAvailable(config::DmaRequest::Usart1Tx));
}

// ──────────────────────────────── Полная очередь ────────────────────────────

void test_queue_overflow() {
    (void)std::puts("арбитр: полная очередь отвечает NoMemory, а не теряет работы:");

    static StubOp ops[config::DMA_ARBITER_JOBS + 2u];
    static DmaTicket tickets[config::DMA_ARBITER_JOBS + 2u];

    // Первая работа занимает канал и не завершается — остальные копятся.
    for (uint16_t i = 0u; i < config::DMA_ARBITER_JOBS + 2u; ++i) {
        ops[i].bytes = 1u;
        const Status st = arb().submit(makeJob(ops[i], config::DmaRequest::Usart2Rx), tickets[i]);
        if (i < config::DMA_ARBITER_JOBS) {
            check("работа принята", st == Status::Ok);
        } else {
            check("переполнение -> NoMemory", st == Status::NoMemory);
        }
    }
    check_eq<unsigned>("счётчик переполнений", arb().stats().overflow, 2u);
    check_eq<Size>("в очереди всё, кроме первой",
                   arb().queued(), config::DMA_ARBITER_JOBS - 1u);

    // Освобождаем завершённые/снятые работы — очередь снова принимает.
    for (uint16_t i = 0u; i < config::DMA_ARBITER_JOBS; ++i) {
        (void)arb().cancel(tickets[i]);
        arb().release(tickets[i]);
    }
    StubOp again; again.bytes = 1u;
    DmaTicket t{};
    check("после освобождения слотов работа принята",
          arb().submit(makeJob(again, config::DmaRequest::Usart2Rx), t) == Status::Ok);
    again.finish();
    arb().release(t);
}

// ────────────────────────── Проверка аргументов ─────────────────────────────

void test_argument_checks() {
    (void)std::puts("арбитр: некорректная работа отвергается:");

    DmaTicket t{};
    StubOp op;

    DmaJob no_start{};
    no_start.owner = &op;
    no_start.tx    = config::DmaRequest::Usart1Tx;
    check("без start -> InvalidArg", arb().submit(no_start, t) == Status::InvalidArg);

    DmaJob no_owner = makeJob(op, config::DmaRequest::Usart1Tx);
    no_owner.owner = nullptr;
    check("без владельца -> InvalidArg", arb().submit(no_owner, t) == Status::InvalidArg);

    DmaJob no_channels = makeJob(op, config::DmaRequest::None);
    check("без каналов -> InvalidArg", arb().submit(no_channels, t) == Status::InvalidArg);

    DmaTicket bad{};
    bad.slot = 0xFFFFu;
    check("недействительный билет: статус InvalidArg",
          arb().status(bad) == Status::InvalidArg);
    check("недействительный билет: ready() == false", !arb().ready(bad));
}

} // namespace

int main() {
    (void)std::puts("=== Арбитр каналов DMA (очередь работ) ===");

    check("менеджер каналов инициализирован", mgr().init() == Status::Ok);

    test_waits_for_channel();
    test_parallel_channels();
    test_one_job_per_interface();
    test_two_channels_all_or_nothing();
    test_sync_completion_in_start();
    test_start_failure_releases_channel();
    test_cancel_and_timeout();
    test_queue_overflow();
    test_argument_checks();

    const DmaArbiter::Stats st = arb().stats();
    (void)std::printf("\nстатистика: принято=%u, начато=%u, успешно=%u, ошибок=%u, "
                      "снято=%u, переполнений=%u\n",
                      (unsigned)st.submitted, (unsigned)st.started,
                      (unsigned)st.completed, (unsigned)st.failed,
                      (unsigned)st.cancelled, (unsigned)st.overflow);

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed != 0) {
        (void)std::printf(", провалено: %d\n", g_failed);
        return 1;
    }
    (void)std::printf(" — арбитр каналов работает\n");
    return 0;
}
