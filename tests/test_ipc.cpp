// tests/test_ipc.cpp
// ---------------------------------------------------------------------------
// P1: MessageQueue, Mailbox, EventFlags, WorkQueue, log, shell,
// статистика задач и round-robin.
// ---------------------------------------------------------------------------
#include "os/core/ipc/event_flags.hpp"
#include "os/core/ipc/mailbox.hpp"
#include "os/core/ipc/message_queue.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/core/task/sheduler.hpp"
#include "os/core/work/work_queue.hpp"
#include "os/log/log.hpp"
#include "os/shell/shell.hpp"
#include "os/shell/shell_ota.hpp"
#include "os/time/deferred_timer.hpp"
#include "os/shell/shell_watchdog.hpp"

#include <cstdio>
#include <cstring>

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
    (void)std::printf("  [%s] %-52s got=%lld expected=%lld\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<long long>(got),
                      static_cast<long long>(expected));
}

bool contains(const char* hay, const char* needle) {
    return hay != nullptr && needle != nullptr &&
           std::strstr(hay, needle) != nullptr;
}

void body(void*) noexcept {}

#if MCFRAME_TEST_HOOKS
struct BlockCtx {
    embed::MessageQueue<uint32_t, 2>* q = nullptr;
    embed::EventFlags* ef = nullptr;
    embed::Mailbox<uint32_t>* mb = nullptr;
    embed::scheduler* s = nullptr;
    uint32_t mode = 0u;
};

void block_hook(void* arg) noexcept {
    auto* c = static_cast<BlockCtx*>(arg);
    switch (c->mode) {
        case 0u: (void)c->q->push_from_isr(42u); break;
        case 1u: c->s->advance_ticks(1u); break;
        case 2u: c->ef->set_from_isr(0x1u); break;
        case 3u: c->mb->post_from_isr(7u); break;
        default: break;
    }
}
#endif

// ─────────────────── статистика и round-robin ───────────────────
embed::task g_main;
embed::task g_peer;
uint32_t    g_main_stack[96];
uint32_t    g_peer_stack[96];

void test_stats_round_robin() {
    (void)std::puts("stats / round-robin:");

    using embed::Status;
    using embed::scheduler;

    check("memory init", embed::g_memory_manager.init(4096u, 512u) == Status::Ok);
    scheduler& s = scheduler::instance();
    check("scheduler init", s.init() == Status::Ok);

    check("main init", g_main.init(&body, nullptr, "main", 96u, 5u) == Status::Ok);
    check("add main", s.add_task(&g_main, g_main_stack, 96u));
    check("start", s.start() == Status::Ok);
    check("main current", s.current_task() == &g_main);

    check("peer init", g_peer.init(&body, nullptr, "peer", 96u, 5u) == Status::Ok);
    check("add peer", s.add_task(&g_peer, g_peer_stack, 96u));

    embed::TaskHandler snap[embed::kMaxTasks + 1u]{};
    const embed::Size n = s.task_snapshot(snap, embed::kMaxTasks + 1u);
    check("snapshot: main+peer+idle", n == 3u);

    const uint64_t rt_before = g_main.runtime_ticks();
    const embed::Size sw_before = s.switch_requests();

    s.set_time_slice(1u);
    s.tick();
    check("тик учёл runtime текущей задачи",
          g_main.runtime_ticks() == rt_before + 1u);
    check("round-robin запросил переключение", s.switch_requests() > sw_before);
    check("main вытеснен в ready", g_main.state() == embed::task_state::suspended);

    s.context_switch();
    check("после context_switch текущая — peer", s.current_task() == &g_peer);
    check("peer получил диспетчеризацию", g_peer.switches() >= 1u);

    s.set_time_slice(0u);
    check("remove peer", s.remove_task(&g_peer) == Status::Ok);
    s.context_switch();
    check("main снова текущая", s.current_task() == &g_main);
}

// ─────────────── блокирующие IPC (детерминированный хук) ───────────────
void test_blocking_ipc() {
#if MCFRAME_TEST_HOOKS
    (void)std::puts("blocking ipc:");

    embed::scheduler& s = embed::scheduler::instance();
    embed::MessageQueue<uint32_t, 2> q;
    embed::EventFlags ef;
    embed::Mailbox<uint32_t> mb;

    BlockCtx ctx{};
    ctx.q = &q;
    ctx.ef = &ef;
    ctx.mb = &mb;
    ctx.s = &s;
    s.set_block_hook(&block_hook, &ctx);

    uint32_t v = 0u;
    ctx.mode = 0u;
    check("pop просыпается от push из ISR",
          q.pop(v, 50u) == embed::Status::Ok && v == 42u);
    // На mock нет реального переключения: после пробуждения задача осталась
    // ready. Возвращаем её на ядро, иначе следующий block_on() увидит
    // state != running.
    s.context_switch();

    ctx.mode = 1u;
    check("pop возвращает Timeout по абсолютному сроку",
          q.pop(v, 1u) == embed::Status::Timeout);
    s.context_switch();

    ctx.mode = 2u;
    check("EventFlags::wait видит set из ISR",
          ef.wait(0x1u, false, true, 50u) == embed::Status::Ok);
    check("... и сбрасывает бит", !ef.any(0x1u));
    s.context_switch();

    ctx.mode = 3u;
    check("Mailbox::pend просыпается от post_from_isr",
          mb.pend(v, 50u) == embed::Status::Ok && v == 7u);
    s.context_switch();

    s.set_block_hook(nullptr, nullptr);
    check("хук снят", true);
#endif
}

// ─────────────────────── DeferredTimer ───────────────────────
void timer_cb(void*) noexcept {}

void test_deferred_timer() {
    (void)std::puts("deferred timer:");

    static embed::WorkQueue<2> wq;
    static uint32_t stack[64];
    check("workqueue start", wq.start("dt", stack, 64u, 5u) == embed::Status::Ok);

    embed::config::OsTimer::Config cfg{};
    check("timer init", embed::config::OsTimer::Init(cfg) == embed::Status::Ok);

    embed::DeferredTimer<2> dt;
    check("create", dt.create(wq, 10u, true, &timer_cb) == embed::Status::Ok);
    check("created", dt.created());
    check("remove", dt.remove() == embed::Status::Ok);
    check("removed", !dt.created());
}

// ─────────────────────────── IPC ───────────────────────────
void test_message_queue() {
    (void)std::puts("message queue:");
    using embed::MessageQueue;

    MessageQueue<uint32_t, 2> q;
    check("push 1", q.push(1u) == embed::Status::Ok);
    check("push 2", q.push(2u) == embed::Status::Ok);
    check_eq<unsigned>("size 2", static_cast<unsigned>(q.size()), 2u);
    check("queue full", q.full());
    check("try_push на полной — false", !q.try_push(3u));

    uint32_t v = 0u;
    check("try_pop -> 1", q.try_pop(v) && v == 1u);
    check("push 3 после pop", q.push(3u) == embed::Status::Ok);
    check("try_pop -> 2", q.try_pop(v) && v == 2u);
    check("try_pop -> 3", q.try_pop(v) && v == 3u);
    check("queue empty", q.empty());

    check("push_from_isr", q.push_from_isr(7u));
    check("pop_from_isr", q.pop_from_isr(v) && v == 7u);
    check("empty after ISR", q.empty());

    // T не обязан быть default-constructible: хранилище сырое.
    struct NoDefault {
        explicit NoDefault(uint32_t x) noexcept : value(x) {}
        NoDefault(const NoDefault&) = default;
        NoDefault& operator=(const NoDefault&) = default;
        uint32_t value;
    };
    MessageQueue<NoDefault, 2> qnd;
    check("push NoDefault", qnd.push(NoDefault(55u)) == embed::Status::Ok);
    NoDefault out(0u);
    check("pop NoDefault", qnd.pop(out) == embed::Status::Ok && out.value == 55u);
}

void test_mailbox() {
    (void)std::puts("mailbox:");
    using embed::Mailbox;

    Mailbox<uint32_t> mb;
    mb.post(1u);
    check("has value", mb.has_value());
    mb.post(2u);
    uint32_t v = 0u;
    check("mailbox хранит последнее", mb.try_pend(v) && v == 2u);
    check("после pend пусто", !mb.has_value());

    mb.post(3u);
    check("pend -> 3", mb.pend(v) == embed::Status::Ok && v == 3u);
    mb.post_from_isr(9u);
    check("post_from_isr -> 9", mb.try_pend(v) && v == 9u);

    mb.post(11u);
    check("peek не забирает", mb.peek(v) && v == 11u && mb.has_value());
    mb.clear();
    check("clear", !mb.has_value());
}

void test_event_flags() {
    (void)std::puts("event flags:");
    using embed::EventFlags;

    EventFlags ef;
    ef.set(0x5u);
    check("any(0x1)", ef.any(0x1u));
    check("all(0x5)", ef.all(0x5u));

    check("wait any + clear", ef.wait(0x1u, false, true) == embed::Status::Ok);
    check("бит 0 снят", !ef.any(0x1u) && ef.any(0x4u));
    check("wait bit1", ef.wait(0x4u, false, true) == embed::Status::Ok);
    check_eq<unsigned>("flags пусты", ef.value(), 0u);

    ef.set(0x3u);
    check("wait all без очистки",
          ef.wait(0x3u, true, false) == embed::Status::Ok);
    check_eq<unsigned>("биты остались", ef.value(), 3u);

    ef.clear(0xFFu);
    ef.set_from_isr(0x8u);
    check("set_from_isr", ef.any(0x8u));
    check("wait(0) -> InvalidArg",
          ef.wait(0u) == embed::Status::InvalidArg);
}

void work_cb(void* arg) noexcept {
    auto* n = static_cast<uint32_t*>(arg);
    ++(*n);
}

void test_work_queue() {
    (void)std::puts("work queue:");
    using embed::WorkQueue;

    // Очередь и её TCB обязаны жить дольше задачи-исполнителя: задача
    // остаётся в планировщике и после выхода из теста (shell/ps её увидит).
    static WorkQueue<2> wq;
    uint32_t calls = 0u;

    check("post до start", wq.post(&work_cb, &calls));
    check("size 1", wq.size() == 1u);
    check("run_one", wq.run_one() && calls == 1u);
    check("empty", wq.empty());

    static uint32_t stack[96];
    check("start", wq.start("work", stack, 96u, 5u) == embed::Status::Ok);
    check("worker в ready", wq.worker_task()->is_ready() || wq.worker_task()->is_running());

    check("post", wq.post(&work_cb, &calls));
    check("post_from_isr", wq.post_from_isr(&work_cb, &calls));
    check("size 2", wq.size() == 2u);
    check("full", wq.full());
    check("post в полную — false", !wq.post(&work_cb, &calls));

    check("run_one 1", wq.run_one() && calls == 2u);
    check("run_one 2", wq.run_one() && calls == 3u);
    check("empty after run", wq.empty());
    check("null fn отклонён", !wq.post(nullptr));
}

// ─────────────────────────── log / shell ───────────────────────────
struct OutCtx {
    char  buf[4096] = {};
    embed::Size len = 0u;
};

void out_writer(void* ctx, const char* text) noexcept {
    auto* o = static_cast<OutCtx*>(ctx);
    if (o == nullptr || text == nullptr) return;
    const embed::Size n = std::strlen(text);
    if ((o->len + n) >= sizeof(o->buf)) return;
    for (embed::Size i = 0u; i < n; ++i) o->buf[o->len + i] = text[i];
    o->len += n;
    o->buf[o->len] = '\0';
}

void test_log_shell() {
    (void)std::puts("log / shell:");

    embed::log::clear();
    embed::log::set_level(embed::log::Level::Debug);

    embed::log::write(embed::log::Level::Info, "TST", "hello");
    check_eq<unsigned>("log pending == 1",
          static_cast<unsigned>(embed::log::pending()), 1u);

    embed::log::Entry e{};
    check("log pop", embed::log::pop(e));
    check("log tag", std::strcmp(e.tag, "TST") == 0);
    check("log text", std::strcmp(e.text, "hello") == 0);

    embed::log::from_isr(embed::log::Level::Warn, "ISR", "tick");
    check_eq<unsigned>("ring после ISR",
          static_cast<unsigned>(embed::log::pending()), 1u);

    embed::log::set_level(embed::log::Level::Error);
    embed::log::write(embed::log::Level::Info, "TST", "hidden");
    check_eq<unsigned>("Info отфильтрован",
          static_cast<unsigned>(embed::log::pending()), 1u);
    embed::log::set_level(embed::log::Level::Debug);

    embed::log::write(embed::log::Level::Error, "TST", "boom");

    OutCtx out;
    embed::shell::Console sh;
    sh.init(&out_writer, &out);
    check("shell: встроенные команды", sh.command_count() >= 6u);

    sh.execute("help");
    check("help: есть ps", contains(out.buf, "ps -"));
    check("help: есть free", contains(out.buf, "free -"));

    sh.execute("ps");
    check("ps: заголовок", contains(out.buf, "prio/base"));
    check("ps: задача main", contains(out.buf, "main"));
    check("ps: idle", contains(out.buf, "idle"));

    sh.execute("free");
    check("free: heap", contains(out.buf, "heap:"));
    check("free: stack", contains(out.buf, "stack:"));

    sh.execute("stats");
    check("stats: ticks", contains(out.buf, "ticks="));
    check("stats: load", contains(out.buf, "load="));

    sh.execute("log");
    check("log command: сообщение", contains(out.buf, "boom"));

    sh.execute("nope");
    check("unknown command", contains(out.buf, "unknown command"));

    // Строка через feed(): help + Enter.
    sh.feed('h');
    sh.feed('e');
    sh.feed('l');
    sh.feed('p');
    sh.feed('\n');
    check("feed исполняет строку", contains(out.buf, "commands:"));
}

void test_optional_shell_commands() {
    (void)std::puts("optional shell commands:");

    OutCtx out;
    embed::shell::Console sh;
    sh.init(&out_writer, &out);

    embed::shell::add_watchdog_commands(sh);
    check("wd добавлена", sh.command_count() >= 7u);
    sh.execute("wd");
    check("wd печатает участников", contains(out.buf, "participants="));

    embed::shell::add_ota_commands(sh);
    check("ota добавлена", sh.command_count() >= 8u);
    sh.execute("ota");
    check("ota без init сообщает об этом", contains(out.buf, "not initialized"));
}

} // namespace

int main() {
    (void)std::puts("=== MCFrame P1 tests (ipc/shell) ===");
    test_stats_round_robin();
    test_blocking_ipc();
    test_message_queue();
    test_mailbox();
    test_event_flags();
    test_work_queue();
    test_deferred_timer();
    test_log_shell();
    test_optional_shell_commands();

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed != 0) {
        (void)std::printf(", провалено: %d\n", g_failed);
        return 1;
    }
    (void)std::printf(" — все тесты пройдены\n");
    return 0;
}
