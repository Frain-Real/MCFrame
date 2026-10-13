// tests/test_os.cpp
// ---------------------------------------------------------------------------
// Самотесты ядра (запускаются на mock-платформе).
//
// Смысл: каждый исправленный баг зафиксирован проверкой, чтобы он не вернулся.
// Запуск: ctest --test-dir .build/mock --output-on-failure
// ---------------------------------------------------------------------------
#include "os/big_math.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/config/time_config.hpp"
#include "os/core/task/sheduler.hpp"
#include "os/utilities.hpp"

#include <cstdio>

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
    (void)std::printf("  [%s] %-46s got=%lld expected=%lld\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<long long>(got),
                      static_cast<long long>(expected));
}

// ─────────────────────────────── big_math ───────────────────────────────
void test_big_math() {
    (void)std::puts("big_math:");
    using embed::BigInt;
    using embed::Status;

    BigInt<8> a(100u), b(7u);
    check_eq<unsigned long long>("100 / 7 == 14", (a / b).d[0], 14ull);
    check_eq<unsigned long long>("100 % 7 == 2",  (a % b).d[0], 2ull);

    BigInt<8> a2(1000u);
    check_eq<unsigned long long>("1000 / 7 == 142", (a2 / b).d[0], 142ull);

    BigInt<8> a3(123456789u), d3(1000u);
    check_eq<unsigned long long>("123456789 / 1000 == 123456", (a3 / d3).d[0], 123456ull);
    check_eq<unsigned long long>("123456789 % 1000 == 789",    (a3 % d3).d[0], 789ull);

    // Перенос при сложении сообщается, а не теряется.
    BigInt<2> x;
    x.d[0] = 0xFFFFFFFFu; x.d[1] = 0xFFFFFFFFu; x.len = 2;
    BigInt<2> one(1u);
    BigInt<2> s = x + one;
    (void)s;
    check("max + 1 -> Status::Overflow", x.last_status() == Status::Overflow);

    // Перенос при умножении доходит до старшего слова.
    BigInt<8> p1(65536u), p2(65536u);
    BigInt<8> pr = p1 * p2;
    check("65536^2 == 2^32 (перенос дошёл)",
          pr.d[0] == 0u && pr.d[1] == 1u && pr.len == 2u);

    // Самоприсваивание больше не портит результат.
    BigInt<8> aa(50u), bb(20u);
    const Status st = BigInt<8>::sub_abs(aa, bb, aa);
    check("sub_abs(out == a) == 30", st == Status::Ok && aa.d[0] == 30u);

    // Деление на ноль распознаётся.
    BigInt<8> zero;
    BigInt<8> qz = a / zero;
    (void)qz;
    check("деление на ноль -> NotSupported", a.last_status() == Status::NotSupported);
}

// ─────────────────────────────── память ───────────────────────────────
void test_memory() {
    (void)std::puts("memory:");

    check("init(4096, 512) == Ok",
          embed::g_memory_manager.init(4096u, 512u) == embed::Status::Ok);

    check("повторный init -> AlreadyExists",
          embed::g_memory_manager.init(4096u, 512u) == embed::Status::AlreadyExists);

    check("heap_total() > 0", embed::g_memory_manager.heap_total() > 0u);
    check("heap_free_bytes() <= heap_total()",
          embed::g_memory_manager.heap_free_bytes() <= embed::g_memory_manager.heap_total());

    // Цикл alloc/free: статистика обязана вернуться к исходной.
    const embed::Size before = embed::g_memory_manager.heap_free_bytes();
    void* p = embed::g_memory_manager.heap_alloc(128u);
    check("heap_alloc(128) != nullptr", p != nullptr);
    check("статистика уменьшилась",
          embed::g_memory_manager.heap_free_bytes() < before);

    embed::g_memory_manager.heap_free(p);
    check("после free статистика восстановлена",
          embed::g_memory_manager.heap_free_bytes() == before);

    // Двойное освобождение детектируется, а не корёжит список.
    embed::g_memory_manager.heap_free(p);
    check_eq<unsigned long long>("двойной free -> heap_faults() == 1",
          embed::g_memory_manager.heap_faults(), 1ull);

    // Запрос больше кучи обязан вернуть nullptr.
    check("запрос > кучи -> nullptr",
          embed::g_memory_manager.heap_alloc(embed::g_memory_manager.heap_total() + 1u)
              == nullptr);

    check("heap_check() == true", embed::g_memory_manager.heap_check());

    // Фрагментация: много блоков разного размера, освобождение через один.
    void* blocks[8]{};
    for (unsigned i = 0; i < 8u; ++i) {
        blocks[i] = embed::g_memory_manager.heap_alloc(32u + i * 8u);
    }
    bool all_ok = true;
    for (unsigned i = 0; i < 8u; ++i) all_ok = all_ok && (blocks[i] != nullptr);
    check("8 разноразмерных блоков выделены", all_ok);

    for (unsigned i = 0; i < 8u; i += 2u) embed::g_memory_manager.heap_free(blocks[i]);
    check("куча цела после фрагментации", embed::g_memory_manager.heap_check());

    for (unsigned i = 1; i < 8u; i += 2u) embed::g_memory_manager.heap_free(blocks[i]);
    check("куча цела после полного освобождения", embed::g_memory_manager.heap_check());
    check("вся память возвращена",
          embed::g_memory_manager.heap_free_bytes() == before);
}

// ─────────────────────────────── планировщик ───────────────────────────────
namespace {
embed::task     g_t1, g_t2;
uint32_t        g_s1[128], g_s2[128];

/// Тело задачи. Без реального переключения контекста (mock) оно не
/// выполняется, поэтому здесь достаточно пустой реализации.
void entry(void*) noexcept {}
} // namespace

void test_scheduler() {
    (void)std::puts("scheduler:");
    using embed::Status;
    using embed::scheduler;

    scheduler& s = scheduler::instance();

    check("init() == Ok", s.init() == Status::Ok);
    check("повторный init -> AlreadyExists", s.init() == Status::AlreadyExists);
    check_eq<unsigned long long>("idle создан: ready_count(0) == 1",
          s.ready_count(0u), 1ull);

    check("t1.init() == Ok",
          g_t1.init(&entry, nullptr, "t1", 128u, 1u) == Status::Ok);
    check("t2.init() == Ok",
          g_t2.init(&entry, nullptr, "t2", 128u, 2u) == Status::Ok);

    // Регистрация должна проходить: раньше add_task() отклонял задачу,
    // потому что узел уже принадлежал реестру задач.
    check("add_task(t1) == true", s.add_task(&g_t1, g_s1, 128u));
    check("add_task(t2) == true", s.add_task(&g_t2, g_s2, 128u));
    check_eq<unsigned long long>("task_count() == 2", s.task_count(), 2ull);

    check("повторный add_task(t1) отклонён", !s.add_task(&g_t1, g_s1, 128u));

    check("remove_task(t2) == Ok", s.remove_task(&g_t2) == Status::Ok);
    check_eq<unsigned long long>("task_count() == 1 после remove", s.task_count(), 1ull);
    check("повторный remove_task(t2) -> NotFound",
          s.remove_task(&g_t2) == Status::NotFound);
}

// ─────────────────────── сон -> тик -> пробуждение ───────────────────────
// Этот путь прежде не покрывался: задача могла уйти в sleep_list и не
// вернуться (context_switch обнулял current до сохранения SP).
void test_sleep_wake() {
    (void)std::puts("sleep / wake:");
    using embed::Status;
    using embed::scheduler;
    using embed::task;

    scheduler& s = scheduler::instance();

    // Время нужно, чтобы тик вообще шёл.
    embed::config::OsTimer::Config tcfg{};
    tcfg.default_tick_ms = 10;
    check("OsTimer::Init() == Ok",
          embed::config::OsTimer::Init(tcfg) == Status::Ok);

    // Без регистрации хука TickIsr() не вызывает scheduler::tick(), и тики
    // не обрабатываются вовсе. Ровно на этой ошибке и был пойман тест.
    check("RegisterSystemHook(tick_hook) == Ok",
          embed::config::OsTimer::RegisterSystemHook(&scheduler::tick_hook, nullptr)
              == Status::Ok);

    // Задачи с высоким приоритетом создаём отдельно, чтобы не мешать
    // предыдущему тесту.
    static task sa, sb;
    static uint32_t sa_stack[48], sb_stack[48];
    auto body = [](void*) noexcept {};

    check("sa.init() == Ok", sa.init(body, nullptr, "sa", 48u, 0u) == Status::Ok);
    check("sb.init() == Ok", sb.init(body, nullptr, "sb", 48u, 1u) == Status::Ok);
    check("add_task(sa)", s.add_task(&sa, sa_stack, 48u));
    check("add_task(sb)", s.add_task(&sb, sb_stack, 48u));

    check("start() == Ok", s.start() == Status::Ok);
    check_eq<unsigned long long>("выбрана задача высшего приоритета",
          s.current_task() == &sa ? 1ull : 0ull, 1ull);

    check_eq<unsigned long long>("перед сном sleep_count() == 0",
          s.sleep_count(), 0ull);

    check("sleep_for(3) == Ok", s.sleep_for(3u) == Status::Ok);
    check_eq<unsigned long long>("задача ушла в sleep_list",
          s.sleep_count(), 1ull);

    // 2 тика — задача ещё спит.
    embed::config::OsTimer::TickIsr();
    embed::config::OsTimer::TickIsr();
    check_eq<unsigned long long>("через 2 тика ещё спит", s.sleep_count(), 1ull);

    // 3-й тик — задержка истекла.
    embed::config::OsTimer::TickIsr();
    check_eq<unsigned long long>("через 3 тика проснулась", s.sleep_count(), 0ull);

    // Главное: задача НЕ потеряна — либо выполняется, либо в очереди готовых.
    check("задача не потеряна (running или ready)",
          s.current_task() == &sa || sa.is_ready());

    // Водяной знак стека должен измеряться и не превышать выданный размер.
    embed::g_memory_manager.update_all_high_water();
    check("водяной знак стека в пределах размера",
          embed::g_memory_manager.task_stack_high_water(&sa) <= 48u);
    check("стек не переполнен",
          !embed::g_memory_manager.task_stack_overflow(&sa));

    // Граничные значения задержки проверяем на самой модели task:
    // sleep_for(1) обязан разбудить ровно через 1 тик (прежде — через 2),
    // иначе период сна систематически смещён на тик.
    {
        static task probe;
        probe.reset();
        (void)probe.init(body, nullptr, "probe", 48u, 1u);
        probe.save_sp(sa_stack);          // эмулируем инициализированный стек
        check("probe.start()", probe.start());
        probe.set_running();              // задача выполняется

        check("probe.sleep_for(1)", probe.sleep_for(1u));
        check("после sleep_for(1) задача спит", probe.is_sleeping());
        check("tick_delay() — 1-й тик: истекло", probe.tick_delay());
        // tick_delay() только считает тики; перевод в ready делает
        // wake_from_sleep(), который вызывает планировщик.
        check("wake_from_sleep() переводит в ready",
              probe.wake_from_sleep() && probe.is_ready());

        probe.set_running();
        check("probe.sleep_for(3)", probe.sleep_for(3u));
        check("после 1-го тика ещё спит", !probe.tick_delay() && probe.is_sleeping());
        check("после 2-го тика ещё спит", !probe.tick_delay() && probe.is_sleeping());
        check("после 3-го тика истекло", probe.tick_delay());
    }

    // sleep_for(0) — это просто уступка, блокировки быть не должно.
    sa.set_running();
    check("sleep_for(0) == Ok", s.sleep_for(0u) == Status::Ok);
    check_eq<unsigned long long>("sleep_for(0) не блокирует задачу",
          s.sleep_count(), 0ull);

    // Тик не должен ломать очередь готовых.
    for (unsigned i = 0; i < 20u; ++i) embed::config::OsTimer::TickIsr();
    check("после 20 тиков куча цела", embed::g_memory_manager.heap_check());
    check("после 20 тиков задача всё ещё учтена",
          s.current_task() == &sa || sa.is_ready() || sb.is_ready());
}

// ─────────────────────────────── утилиты ───────────────────────────────
void test_utilities() {
    (void)std::puts("utilities:");
    using namespace embed::utilities;

    check_eq<unsigned long long>("os_rand_range(min == max) == min",
          os_rand_range(nullptr, 5u, 5u), 5ull);

    os_rand_ctx_t ctx{0u};
    os_rand_init(&ctx, 0u);
    check("seed == 0 заменяется (xorshift не залипает)", os_rand(&ctx) != 0u);

    // Диапазон не выходит за границы.
    os_rand_ctx_t rctx{12345u};
    bool in_range = true;
    for (int i = 0; i < 1000; ++i) {
        const uint32_t v = os_rand_range(&rctx, 10u, 20u);
        if (v < 10u || v > 20u) in_range = false;
    }
    check("os_rand_range(10,20) держит диапазон", in_range);

    // Полный диапазон uint32 не должен вызывать деление на ноль.
    (void)os_rand_range(&rctx, 0u, 0xFFFFFFFFu);
    check("os_rand_range(0, UINT32_MAX) не делит на ноль", true);

    uint8_t buf[8]{0xFFu,0xFFu,0xFFu,0xFFu,0xFFu,0xFFu,0xFFu,0xFFu};
    memset_(buf, 0, sizeof buf);
    bool zeroed = true;
    for (unsigned char b : buf) zeroed = zeroed && (b == 0u);
    check("memset_ зануляет весь буфер", zeroed);
}

} // namespace

int main() {
    (void)std::puts("=== MCFrame self-tests (mock) ===");
    test_big_math();
    test_memory();
    test_scheduler();
    test_sleep_wake();
    test_utilities();

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed != 0) {
        (void)std::printf(", провалено: %d\n", g_failed);
        return 1;
    }
    (void)std::printf(" — все тесты пройдены\n");
    return 0;
}
