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
#include "os/core/primitives/semaphore.hpp"
#include "os/core/primitives/types.hpp"
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

// ─────────── Куча: слияние свободных блоков В ОБЕ стороны ───────────
//
// Регресс: heap_coalesce() сливал блок только со СЛЕДУЮЩИМ по адресу соседом.
// Освобождение «A, затем C, затем B» (A, B, C идут подряд) оставляло в
// free-list два соседних свободных блока — A и B+C, — которые больше никогда
// не сольются: слияние запускается только при free() освобождаемого блока.
// heap_free_ сходился, поэтому heap_check() проходил, но first-fit не видел
// сплошного куска, а heap_largest_free() занижал отчёт.
void test_heap_coalesce() {
    (void)std::puts("heap coalesce:");
    using embed::g_memory_manager;
    using embed::Size;

    if (!g_memory_manager.heap_available()) {
        check("куча сконфигурирована", false);
        return;
    }
    const Size total = g_memory_manager.heap_total();

    // Предусловие: куча обязана быть одним сплошным блоком — иначе провал
    // ниже нельзя однозначно отнести к проверяемому сценарию (например, если
    // предыдущий тест оставил фрагментацию).
    check("перед тестом куча — один сплошной блок",
          g_memory_manager.heap_largest_free() == total);

    // Три раунда подряд: слияние обязано быть идемпотентным, а не
    // «сработать один раз», и не должно терять/задваивать байты.
    for (unsigned round = 0; round < 3u; ++round) {
        void* a = g_memory_manager.heap_alloc(64u);
        void* b = g_memory_manager.heap_alloc(64u);
        void* c = g_memory_manager.heap_alloc(64u);
        check("A, B, C выделены", a != nullptr && b != nullptr && c != nullptr);
        if (a == nullptr || b == nullptr || c == nullptr) return;

        // Ключевой порядок освобождения: сначала крайние, потом СЕРЕДИНА.
        g_memory_manager.heap_free(a);
        g_memory_manager.heap_free(c);
        g_memory_manager.heap_free(b);

        check("heap_check() после free(A), free(C), free(B)",
              g_memory_manager.heap_check());
        check_eq<unsigned long long>("вся память вернулась",
              g_memory_manager.heap_free_bytes(), total);
        check_eq<unsigned long long>("куча слилась в один блок",
              g_memory_manager.heap_largest_free(), total);
    }

    // Обратный порядок освобождения (сосед справа) тоже обязан давать один блок.
    void* x = g_memory_manager.heap_alloc(96u);
    void* y = g_memory_manager.heap_alloc(160u);
    check("X, Y выделены", x != nullptr && y != nullptr);
    if (x != nullptr && y != nullptr) {
        g_memory_manager.heap_free(y);
        g_memory_manager.heap_free(x);
        check_eq<unsigned long long>("free(Y), free(X) — тоже один блок",
              g_memory_manager.heap_largest_free(), total);
    }

    // Середина занята: слияние назад не должно «проскочить» через занятый
    // блок и склеить чужие области.
    void* p1 = g_memory_manager.heap_alloc(128u);
    void* p2 = g_memory_manager.heap_alloc(128u);
    void* p3 = g_memory_manager.heap_alloc(128u);
    check("P1, P2, P3 выделены", p1 != nullptr && p2 != nullptr && p3 != nullptr);
    if (p1 != nullptr && p2 != nullptr && p3 != nullptr) {
        g_memory_manager.heap_free(p1);
        g_memory_manager.heap_free(p3);
        const Size largest_with_allocated_middle = g_memory_manager.heap_largest_free();
        check("свободные края не склеились через занятую середину",
              largest_with_allocated_middle < total);
        check("куча цела при занятой середине", g_memory_manager.heap_check());

        // Освобождаем середину — только теперь всё обязано слиться в один блок.
        g_memory_manager.heap_free(p2);
        check_eq<unsigned long long>("после free(P2) — один блок",
              g_memory_manager.heap_largest_free(), total);
        check("куча цела после слияния через середину",
              g_memory_manager.heap_check());
    }

    // Стресс: случайные alloc/free. Свойство, которое обязано держаться
    // всегда: после освобождения ВСЕГО куча снова состоит из ОДНОГО блока,
    // а free-list не разъезжается с обходом по адресам. Именно это ломало
    // слияние «только вперёд» на длинной последовательности.
    {
        constexpr unsigned kSlots = 16u;
        void* slots[kSlots]{};
        uint32_t rnd = 0x12345678u;         // ГПСЧ без rand(): детерминированно
        bool integrity_ok = true;

        for (unsigned step = 0u; step < 400u; ++step) {
            rnd = rnd * 1664525u + 1013904223u;
            const unsigned i = (rnd >> 16) % kSlots;
            if (slots[i] == nullptr) {
                slots[i] = g_memory_manager.heap_alloc(16u + ((rnd >> 8) % 64u));
            } else {
                g_memory_manager.heap_free(slots[i]);
                slots[i] = nullptr;
            }
            if ((step % 25u) == 0u && !g_memory_manager.heap_check()) {
                integrity_ok = false;
            }
        }
        check("стресс: куча цела в процессе", integrity_ok);

        for (unsigned i = 0u; i < kSlots; ++i) {
            if (slots[i] != nullptr) {
                g_memory_manager.heap_free(slots[i]);
                slots[i] = nullptr;
            }
        }
        check("стресс: куча цела после освобождения всего",
              g_memory_manager.heap_check());
        check_eq<unsigned long long>("стресс: все байты вернулись",
              g_memory_manager.heap_free_bytes(), total);
        check_eq<unsigned long long>("стресс: вся память — один блок",
              g_memory_manager.heap_largest_free(), total);
    }
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

// ─────────── Пул стеков: только LIFO, учёт без «сирот» ───────────
//
// Регресс: stack_free() уменьшал stack_pool_used_ на размер ЛЮБОГО региона, а
// не только последнего. После free(A) при живом B (выделен позже) вершина
// уезжала вниз, и следующий stack_alloc() выдавал участок, который уже занят
// живым стеком. Плюс stack_alloc() не связывал регион со слотом, а
// task_stack_init() искал слот по задаче — получались «осиротевшие» записи и
// две записи на одну базу.
void test_stack_pool() {
    (void)std::puts("stack pool:");
    using embed::g_memory_manager;
    using embed::Size;
    using embed::Status;
    using embed::scheduler;

    scheduler& s = scheduler::instance();
    const Status ist = s.init();          // AlreadyExists, если уже инициализирован
    check("планировщик готов (idle создан)",
          ist == Status::Ok || ist == Status::AlreadyExists);

    const Size pool_total = g_memory_manager.stack_pool_total();
    if (pool_total == 0u) {
        check("пул стеков сконфигурирован", false);
        return;
    }
    check_eq<unsigned long long>("пул пуст (0 слов занято)",
          g_memory_manager.stack_pool_used(), 0ull);

    // ── 1. Регионы выдаются от вершины, адреса строго возрастают ──
    uint32_t* a = g_memory_manager.stack_alloc(64u);
    uint32_t* b = g_memory_manager.stack_alloc(64u);
    uint32_t* c = g_memory_manager.stack_alloc(32u);
    check("A, B, C выделены", a != nullptr && b != nullptr && c != nullptr);
    if (a == nullptr || b == nullptr || c == nullptr) return;
    check("регионы не пересекаются", (a + 64u == b) && (b + 64u == c));
    check_eq<unsigned long long>("учтено 160 слов",
          g_memory_manager.stack_pool_used(), 160ull);

    // ── 2. Освобождение ИЗ СЕРЕДИНЫ запрещено ──
    // Прежде именно здесь вершина уезжала вниз, и B/C доставались дважды.
    check("free(A) из середины -> NotSupported",
          g_memory_manager.stack_free(a) == Status::NotSupported);
    check("free(B) из середины -> NotSupported",
          g_memory_manager.stack_free(b) == Status::NotSupported);
    check_eq<unsigned long long>("после отказа учёт не изменился",
          g_memory_manager.stack_pool_used(), 160ull);

    // Вершина обязана остаться на месте: следующий регион получает адрес
    // сразу за C, а не поверх B.
    uint32_t* probe = g_memory_manager.stack_alloc(16u);
    check("после отказов вершина не съехала", probe == c + 32u);
    check("пробный регион возвращён (вершина) -> Ok",
          g_memory_manager.stack_free(probe) == Status::Ok);
    check_eq<unsigned long long>("учёт вернулся к 160",
          g_memory_manager.stack_pool_used(), 160ull);

    // ── 3. LIFO-порядок работает, учёт возвращается к нулю ──
    // Настоящий bug-сценарий: A и C освобождаются, пока B жив; C — вершина и
    // возвращается, после чего возвращать A ещё нельзя (сверху B).
    check("free(C) (вершина) -> Ok", g_memory_manager.stack_free(c) == Status::Ok);
    check("free(A) после C -> NotSupported (сверху B)",
          g_memory_manager.stack_free(a) == Status::NotSupported);
    check("free(B) (вершина) -> Ok", g_memory_manager.stack_free(b) == Status::Ok);
    check_eq<unsigned long long>("после free(B) учтено 64",
          g_memory_manager.stack_pool_used(), 64ull);
    check("free(A) -> Ok", g_memory_manager.stack_free(a) == Status::Ok);
    check_eq<unsigned long long>("пул пуст", g_memory_manager.stack_pool_used(), 0ull);

    // ── 4. Чужие и повторные освобождения не портят учёт ──
    check("повторный free(A) -> NotFound",
          g_memory_manager.stack_free(a) == Status::NotFound);
    static uint32_t static_stack[64]{};
    check("статический (не из пула) указатель -> NotFound",
          g_memory_manager.stack_free(static_stack) == Status::NotFound);
    check("free(nullptr) -> InvalidArg",
          g_memory_manager.stack_free(nullptr) == Status::InvalidArg);
    check_eq<unsigned long long>("учёт не поехал",
          g_memory_manager.stack_pool_used(), 0ull);

    // ── 5. Регион, которому задача ещё не отдана, тоже возвращается ──
    // Прежде такой регион был невидим для stack_free (поиск требовал
    // task != nullptr): слова пула утекали молча.
    uint32_t* d = g_memory_manager.stack_alloc(64u);
    check("D выделен", d != nullptr);
    if (d != nullptr) {
        check("free(D) без привязки к задаче -> Ok",
              g_memory_manager.stack_free(d) == Status::Ok);
        check_eq<unsigned long long>("учёт снова 0",
              g_memory_manager.stack_pool_used(), 0ull);
        check("вершина откатилась: следующий регион — та же база",
              g_memory_manager.stack_alloc(64u) == d);
        check("... освобождаем", g_memory_manager.stack_free(d) == Status::Ok);
    }

    // ── 6. Привязка региона к задаче ──
    embed::task t1;
    check("t1.init() == Ok",
          t1.init(&entry, nullptr, "pool1", 64u, 1u) == Status::Ok);
    uint32_t* e = g_memory_manager.stack_alloc(64u);
    uint32_t* f = g_memory_manager.stack_alloc(32u);
    check("E, F выделены", e != nullptr && f != nullptr);
    if (e != nullptr && f != nullptr) {
        check("task_stack_init привязал регион E",
              g_memory_manager.task_stack_init(&t1, e, 64u) != nullptr);

        // Тот же размер, но база занята: повторно она выдаваться не должна.
        uint32_t* probe2 = g_memory_manager.stack_alloc(64u);
        check("занятая база не выдаётся второй раз", probe2 != e && probe2 != nullptr);
        if (probe2 != nullptr) {
            check("пробный регион возвращён",
                  g_memory_manager.stack_free(probe2) == Status::Ok);
        }
        check("стек работающей задачи не возвращается -> Busy",
              g_memory_manager.stack_free(e) == Status::Busy);

        // task_unregister() снимает задачу с учёта, но запись региона пула
        // оставляет — иначе его слова утекли бы (прежнее поведение).
        g_memory_manager.task_unregister(&t1);
        // Отказ здесь именно из-за LIFO (сверху F), а НЕ из-за потери записи:
        // прежде на этом месте был бы NotFound.
        check("после task_unregister регион всё ещё на учёте",
              g_memory_manager.stack_free(e) == Status::NotSupported);
        check("free(F) (вершина) -> Ok", g_memory_manager.stack_free(f) == Status::Ok);
        check("free(E) после task_unregister -> Ok",
              g_memory_manager.stack_free(e) == Status::Ok);
        check_eq<unsigned long long>("учёт снова 0 после пары E/F",
              g_memory_manager.stack_pool_used(), 0ull);
    }

    // ── 7. Размер в task_stack_init обязан совпасть с размером региона ──
    uint32_t* g = g_memory_manager.stack_alloc(48u);
    check("G выделен", g != nullptr);
    if (g != nullptr) {
        embed::task t2;
        check("t2.init() == Ok",
              t2.init(&entry, nullptr, "pool2", 96u, 1u) == Status::Ok);
        check("чужой размер стека отвергнут",
              g_memory_manager.task_stack_init(&t2, g, 96u) == nullptr);
        check("после отказа регион всё ещё возвращается",
              g_memory_manager.stack_free(g) == Status::Ok);
    }

    // ── 8. Исчерпание: таблица учёта или размер пула, но не «в минус» ──
    {
        constexpr unsigned kMax = 32u;
        uint32_t* blocks[kMax]{};
        unsigned n = 0u;
        for (; n < kMax; ++n) {
            blocks[n] = g_memory_manager.stack_alloc(32u);
            if (blocks[n] == nullptr) break;
        }
        check("пул/таблица исчерпались (alloc вернул nullptr)", n < kMax);
        check_eq<unsigned long long>("учтено ровно по числу выданных регионов",
              g_memory_manager.stack_pool_used(),
              static_cast<unsigned long long>(n) * 32ull);
        check("учёт не превысил размер пула",
              g_memory_manager.stack_pool_used() <= pool_total);
        bool contiguous = true;
        for (unsigned i = 1u; i < n; ++i) {
            if (blocks[i] != blocks[i - 1u] + 32u) contiguous = false;
        }
        check("базы уникальны и идут подряд", contiguous);

        bool all_freed = true;
        for (unsigned i = n; i > 0u; --i) {
            if (g_memory_manager.stack_free(blocks[i - 1u]) != Status::Ok) {
                all_freed = false;
            }
        }
        check("все регионы возвращены в LIFO-порядке", all_freed);
        check_eq<unsigned long long>("пул пуст после исчерпания",
              g_memory_manager.stack_pool_used(), 0ull);
    }

    // ── 9. Сквозной путь через планировщик ──
    // alloc -> add_task -> remove_task -> stack_free. Ровно так пул и должен
    // использоваться: задача не знает, откуда стек, а учёт не теряется.
    embed::task t3;
    check("t3.init() == Ok",
          t3.init(&entry, nullptr, "pool3", 96u, 5u) == Status::Ok);
    uint32_t* h = g_memory_manager.stack_alloc(96u);
    check("H выделен", h != nullptr);
    if (h != nullptr) {
        check("add_task с регионом пула", s.add_task(&t3, h, 96u));
        check("стек задачи в пуле не освобождается -> Busy",
              g_memory_manager.stack_free(h) == Status::Busy);
        check("remove_task == Ok", s.remove_task(&t3) == Status::Ok);
        check("после remove_task стек возвращается в пул",
              g_memory_manager.stack_free(h) == Status::Ok);
        check_eq<unsigned long long>("пул пуст после сквозного пути",
              g_memory_manager.stack_pool_used(), 0ull);
    }

    // Статический стек задачи в пул не возвращается: он не регион пула.
    check("статический стек задачи -> NotFound",
          g_memory_manager.stack_free(g_s1) == Status::NotFound);
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

// ─────────── Краевые случаи: remove_task(current) и exit() ───────────
//
// Прежде remove_task() для ТЕКУЩЕЙ задачи только обнулял cores_[i].current:
// задача продолжала исполняться (её уже нет ни в ready, ни в sleep_list —
// вытеснить некому), а следующее переключение контекста заставало current
// == nullptr. exit() вообще не инициировал переключение.
//
// На mock реального переключения нет (platform trigger_context_switch — пустая
// функция), поэтому проверяем УЧЁТНУЮ часть: снятие с очередей, выбор новой
// текущей задачи и состояние завершённой задачи. Для этого вызываем
// scheduler::context_switch() явно — он делает ту же работу, что делает
// PendSV-обработчик на железе (save/load контекста на mock — заглушки).
void test_remove_current_and_exit() {
    (void)std::puts("remove current / exit:");
    using embed::Size;
    using embed::Status;
    using embed::scheduler;
    using embed::task;
    using embed::TaskHandler;
    using embed::task_state;

    scheduler& s = scheduler::instance();
    if (!s.started()) {
        check("планировщик запущен", s.start() == Status::Ok);
    }

    auto body = [](void*) noexcept {};

    // ── 1. remove_task(ТЕКУЩЕЙ задачи) ──
    // Свежая задача с высшим приоритетом (0) гарантирует, что pick_next()
    // выберет именно задачу, а не idle (у idle приоритет самый низкий).
    static task tc;
    static uint32_t tc_stack[64];
    check("tc.init() == Ok", tc.init(body, nullptr, "cur", 64u, 0u) == Status::Ok);
    check("add_task(tc)", s.add_task(&tc, tc_stack, 64u));
    s.context_switch();

    TaskHandler cur = s.current_task();
    check("ядро получило настоящую задачу", cur != nullptr);
    if (cur == nullptr) return;

    const Size tasks_before = s.task_count();
    const Size switches_before = s.switch_requests();
    check("remove_task(current) == Ok", s.remove_task(cur) == Status::Ok);
    check("remove_task(current) запросил переключение контекста",
          s.switch_requests() > switches_before);
    check("удалённая задача больше не текущая", s.current_task() != cur);
    check_eq<unsigned long long>("task_count уменьшился",
          s.task_count(), tasks_before - 1u);
    check("удалённая задача помечена done", cur->state() == task_state::done);
    check("удалённая задача не в ready", !cur->is_ready());
    check("повторный remove_task -> NotFound",
          s.remove_task(cur) == Status::NotFound);

    // Ядро обязано получить новую задачу, а не остаться без current.
    s.context_switch();
    check("после remove_task(current) ядро получило задачу",
          s.current_task() != nullptr);
    check("... и это НЕ удалённая задача", s.current_task() != cur);

    // А удаление НЕ текущей задачи переключения не требует: проверяем, что
    // исправление точечное, а не «дёргаем переключение всегда».
    {
        static task tidle;
        static uint32_t tidle_stack[32];
        check("tidle.init() == Ok",
              tidle.init(body, nullptr, "other", 32u, 1u) == Status::Ok);
        check("add_task(tidle)", s.add_task(&tidle, tidle_stack, 32u));
        // add_task() сам может запросить вытеснение (если новая задача
        // приоритетнее текущей) — поэтому снимок берём ПОСЛЕ него.
        const Size sw = s.switch_requests();
        check("remove_task(не текущей) == Ok", s.remove_task(&tidle) == Status::Ok);
        check("remove_task(не текущей) не просит переключение",
              s.switch_requests() == sw);
    }

    // ── 2. exit() текущей задачи ──
    static task te;
    static uint32_t te_stack[64];
    check("te.init() == Ok", te.init(body, nullptr, "exit", 64u, 0u) == Status::Ok);
    check("add_task(te)", s.add_task(&te, te_stack, 64u));
    s.context_switch();
    check("ядро получило задачу для exit()", s.current_task() != nullptr);

    TaskHandler ex = s.current_task();
    if (ex != nullptr) {
        const Size tasks_before_exit = s.task_count();
        const Size sw_exit = s.switch_requests();
        check("exit() == Ok", s.exit() == Status::Ok);
        check("exit() запросил переключение контекста",
              s.switch_requests() > sw_exit);
        check("после exit() задача не текущая", s.current_task() != ex);
        check("после exit() задача завершена", ex->state() == task_state::done);
        check_eq<unsigned long long>("exit() снял задачу с учёта",
              s.task_count(), tasks_before_exit - 1u);
        check("повторный exit() без текущей задачи -> NotFound",
              s.exit() == Status::NotFound);
        check("exit() не оставил задачу в ready", !ex->is_ready());
    }

    // Планировщик возвращаем в рабочее состояние: ядру нужен current.
    s.context_switch();
    check("после exit() ядро получило задачу", s.current_task() != nullptr);
}

// ─────────── Подготовка переключения (то, что делает PendSV) ───────────
//
// context_switch() = save_context → prepare_context_switch → load_context.
// На МКУ СЕРЕДИНУ вызывает обработчик PendSV, и он обязан ВЕРНУТЬСЯ: если бы
// загрузку контекста делал C-код (platform_load_context с `bx 0xFFFFFFFD`),
// кадры обработчика остались бы в MSP навсегда и стек кончился бы за десятки
// переключений (см. platforms/stm32/platform_ops.cpp). Поэтому выбор следующей
// задачи вынесен в отдельную функцию, и её учётную часть можно проверить на
// хосте — здесь проверяется именно она.
void test_prepare_context_switch() {
    (void)std::puts("подготовка переключения (середина PendSV):");
    using embed::Status;
    using embed::scheduler;
    using embed::task;
    using embed::TaskHandler;

    scheduler& s = scheduler::instance();
    if (!s.started()) {
        check("планировщик запущен", s.start() == Status::Ok);
    }

    auto body = [](void*) noexcept {};

    // Задача с высшим приоритетом: она обязана быть выбрана, если текущая
    // задача не running (в этом случае вытеснять нечего).
    static task hi;
    static uint32_t hi_stack[64];
    check("hi.init() == Ok", hi.init(body, nullptr, "pendsv", 64u, 0u) == Status::Ok);
    check("hi добавлена", s.add_task(&hi, hi_stack, 64u));

    // «SP, который вернул save_context()» — на хосте его роль играет любой
    // указатель: важно, куда планировщик его запишет.
    static uint32_t fake_frame[16];
    uint32_t* const saved_sp = &fake_frame[8];

    TaskHandler before = s.current_task();
    const bool was_running = (before != nullptr) && before->is_running();

    uint32_t* next_sp = s.prepare_context_switch(saved_sp);

    // Главный инвариант: вернулся SP ИМЕННО той задачи, которая стала текущей.
    // Обработчик загрузит контекст ровно отсюда, поэтому расхождение означало
    // бы запуск одной задачи по кадрам другой.
    check("вернулся не nullptr (загружать контекст есть откуда)", next_sp != nullptr);
    check("вернулся SP новой текущей задачи",
          next_sp != nullptr && next_sp == s.current_task()->load_sp());
    check("выбранная задача помечена running",
          s.current_task() != nullptr && s.current_task()->is_running());

    if (was_running) {
        // Уходящая задача обязана быть сохранена ДО выбора следующей: её SP
        // записан в её же TCB, а сама она возвращена в очередь готовых.
        check("SP уходящей задачи записан в её TCB", before->load_sp() == saved_sp);
        // Состояние вытесненной задачи — suspended («вытеснена и стоит в
        // ready-очереди»): это то, что ставит task::suspend(), и именно его
        // разбирает wakeup(). Здесь важно, что задача НЕ потеряна.
        check("уходящая задача не потеряна (в очереди готовых)",
              before->state() == embed::task_state::suspended || before->is_ready());
        check("уходящая задача больше не current", s.current_task() != before);
    }

    // Повторный вызов без сохранённого SP (так делает первый вызов обработчика
    // на mock, где save_context возвращает nullptr) не должен ничего ломать.
    TaskHandler cur = s.current_task();
    uint32_t* again = s.prepare_context_switch(nullptr);
    check("без сохранённого SP переключение всё равно состоялось", again != nullptr);
    check("и вернулся SP текущей задачи",
          again != nullptr && again == s.current_task()->load_sp());
    if (cur != nullptr && cur != s.current_task()) {
        // Прежняя задача была running — её сняли с ядра, но не потеряли.
        check("снятая задача не потеряна",
              cur->state() == embed::task_state::suspended ||
              cur->is_ready() || cur->is_sleeping() || cur->is_blocked());
    }
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

// ───────────────────────────── atomic_t ─────────────────────────────
//
// atomic_t — единственный способ атомарного доступа в MCFrame (замена
// std::atomic и «голым» __atomic_*-встройкам), поэтому его контракт должен
// быть зафиксирован проверками: у builtin-функций нет ни типовой проверки,
// ни защиты от потери порядка доступа к памяти.
void test_atomic() {
    (void)std::puts("atomic_t:");
    using embed::atomic_bool;
    using embed::atomic_t;
    using embed::MemoryOrderAcquire;
    using embed::MemoryOrderAcqRel;
    using embed::MemoryOrderRelaxed;
    using embed::MemoryOrderRelease;

    atomic_t<uint32_t> n{7u};
    check_eq<unsigned>("значение из конструктора", n.load(), 7u);
    n.store(9u, MemoryOrderRelease);
    check_eq<unsigned>("store/load с заданным порядком", n.load(MemoryOrderAcquire), 9u);

    // exchange обязан вернуть ПРЕЖНЕЕ значение, иначе на нём нельзя
    // строить захват (как в completion::post).
    check_eq<unsigned>("exchange возвращает прежнее", n.exchange(11u), 9u);
    check_eq<unsigned>("exchange записал новое", n.load(), 11u);

    uint32_t expected = 11u;
    check("CAS(11 -> 12) выполнен", n.compare_exchange_strong(expected, 12u));
    check_eq<unsigned>("после успешного CAS", n.load(), 12u);

    expected = 100u;
    check("CAS(100 -> 200) отвергнут", !n.compare_exchange_strong(expected, 200u));
    check_eq<unsigned>("expected получил фактическое значение", expected, 12u);
    check_eq<unsigned>("значение при неудаче не изменилось", n.load(), 12u);

    // Слабая CAS вызывается только в цикле: «не повезло» — не ошибка.
    uint32_t cur = n.load(MemoryOrderRelaxed);
    uint32_t spins = 0u;
    while (!n.compare_exchange_weak(cur, cur + 1u, MemoryOrderAcqRel,
                                    MemoryOrderRelaxed)) {
        if (++spins > 1000u) break;
    }
    check("цикл слабой CAS завершился", spins <= 1000u);
    check_eq<unsigned>("цикл слабой CAS увеличил на 1", n.load(), 13u);

    check_eq<unsigned>("fetch_add возвращает прежнее",
                       n.fetch_add(2u, MemoryOrderRelaxed), 13u);
    check_eq<unsigned>("после fetch_add", n.load(), 15u);
    check_eq<unsigned>("fetch_sub возвращает прежнее",
                       n.fetch_sub(5u, MemoryOrderRelaxed), 15u);
    check_eq<unsigned>("после fetch_sub", n.load(), 10u);

    check_eq<unsigned>("++ даёт новое значение", ++n, 11u);
    check_eq<unsigned>("постфиксный ++ даёт прежнее", n++, 11u);
    n = 20u;                              // operator=(T) — как у std::atomic
    check_eq<unsigned>("operator= записывает значение", n.load(), 20u);
    n -= 4u;
    check_eq<unsigned>("operator-= уменьшает", n.load(), 16u);

    atomic_bool flag{false};
    bool was = false;
    check("CAS false->true выигрывает защёлку",
          flag.compare_exchange_strong(was, true));
    was = false;
    check("повторный CAS false->true отвергнут",
          !flag.compare_exchange_strong(was, true));
    check("после проигранного CAS флаг взведён", flag.load());
    flag.store(false, MemoryOrderRelease);
    check("store(false) снимает флаг", !flag.load());
}

// ─────────────────────── счётчик семафора (atomic_t) ───────────────────────
//
// Раньше счётчик был обычным uint32_t, а value() читал его «голым»
// __atomic_load_n. Проверяем оба пути: под критической секцией (wait/
// try_wait/signal) и БЕЗ неё (signal_from_isr), включая ограничение MaxCount.
void test_semaphore_counters() {
    (void)std::puts("semaphore:");
    using embed::sem_type;
    using embed::semaphore;
    using embed::Status;

    semaphore<sem_type::counting, 3u> cs;
    check_eq<unsigned>("счётный: начальное значение 0", cs.value(), 0u);
    check("счётный: try_wait на пустом -> false", !cs.try_wait());

    for (unsigned i = 0u; i < 5u; ++i) cs.signal_from_isr();
    check_eq<unsigned>("счётный: signal_from_isr не поднимает выше MaxCount",
                       cs.value(), 3u);

    check("счётный: try_wait забирает ресурс", cs.try_wait());
    check_eq<unsigned>("счётный: счётчик уменьшился", cs.value(), 2u);
    check("счётный: wait забирает ресурс",
          cs.wait() == Status::Ok && cs.wait() == Status::Ok);
    check_eq<unsigned>("счётный: после двух wait счётчик пуст", cs.value(), 0u);

    semaphore<sem_type::binary> bs;
    bs.signal();
    check_eq<unsigned>("бинарный: signal даёт 1", bs.value(), 1u);
    bs.signal();
    check_eq<unsigned>("бинарный: повторный signal не даёт 2", bs.value(), 1u);
    check("бинарный: try_wait забирает событие", bs.try_wait());
    check_eq<unsigned>("бинарный: после try_wait 0", bs.value(), 0u);

    semaphore<sem_type::binary> bs_isr;
    bs_isr.signal_from_isr();
    check_eq<unsigned>("бинарный: signal_from_isr даёт 1", bs_isr.value(), 1u);
    check("бинарный: wait видит событие из прерывания",
          bs_isr.wait() == Status::Ok);
}

} // namespace

int main() {
    (void)std::puts("=== MCFrame self-tests (mock) ===");
    test_big_math();
    test_memory();
    test_heap_coalesce();
    test_scheduler();
    test_stack_pool();
    test_sleep_wake();
    test_remove_current_and_exit();
    test_prepare_context_switch();
    test_utilities();
    test_atomic();
    test_semaphore_counters();

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed != 0) {
        (void)std::printf(", провалено: %d\n", g_failed);
        return 1;
    }
    (void)std::printf(" — все тесты пройдены\n");
    return 0;
}
