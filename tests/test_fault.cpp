// tests/test_fault.cpp
// ---------------------------------------------------------------------------
// Проверки P0-механизмов:
//   * запись об аварии (Fault/panic) и её чтение после reset;
//   * порядок очереди ожидания (приоритет + FIFO внутри приоритета);
//   * наследование приоритета мьютексом;
//   * абсолютный таймаут блокировки и флаг timed_out;
//   * сторожевой маркер стека.
//
// На mock реального переключения контекста нет, поэтому проверяется УЧЁТНАЯ
// часть — именно она на железе определяет, кого разбудят и с каким
// приоритетом. Возврат Status::Fault в тесте PI — следствие mock: задача уже
// «спит», а block_on_locked на хосте возвращается сразу.
// ---------------------------------------------------------------------------
#include "os/core/memory/memory_manager.hpp"
#include "os/core/primitives/mutex.hpp"
#include "os/core/primitives/semaphore.hpp"
#include "os/core/task/sheduler.hpp"
#include "os/fault/fault.hpp"

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
    (void)std::printf("  [%s] %-52s got=%lld expected=%lld\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<long long>(got),
                      static_cast<long long>(expected));
}

void body(void*) noexcept {}

// ─────────────────────────── запись об аварии ───────────────────────────
void test_fault_record() {
    (void)std::puts("fault record:");

    embed::fault::clear();
    check("после clear() записи нет", embed::fault::last() == nullptr);
    check("... и was_fault_reset() == false", !embed::fault::was_fault_reset());

    embed::fault::Info info{};
    info.kind  = embed::fault::Kind::HardFault;
    info.pc    = 0x08001234u;
    info.lr    = 0x08005678u;
    info.psr   = 0x01000000u;
    info.sp    = 0x20001FF0u;
    info.r0    = 0x11111111u;
    info.r1    = 0x22222222u;
    info.r2    = 0x33333333u;
    info.r3    = 0x44444444u;
    info.r12   = 0x55555555u;
    info.cfsr  = 0x00000002u;
    info.hfsr  = 0x40000000u;
    info.mmfar = 0x20000000u;
    info.bfar  = 0x08000000u;
    info.task  = "blink";
    info.file  = "src/app.cpp";
    info.line  = 42u;
    info.tick  = 1234u;

    embed::fault::capture(info);

    const embed::fault::Record* r = embed::fault::last();
    check("запись сохранена", r != nullptr);
    if (r != nullptr) {
        check_eq<unsigned long long>("magic", r->magic,
                                     embed::fault::kMagic);
        check_eq<unsigned long long>("kind", r->kind,
                                     static_cast<unsigned long long>(
                                         embed::fault::Kind::HardFault));
        check_eq<unsigned long long>("pc", r->pc, 0x08001234ull);
        check_eq<unsigned long long>("lr", r->lr, 0x08005678ull);
        check_eq<unsigned long long>("cfsr", r->cfsr, 0x00000002ull);
        check_eq<unsigned long long>("tick", r->tick, 1234ull);
        check_eq<unsigned long long>("line", r->line, 42ull);
        check_eq<unsigned long long>("task", static_cast<unsigned long long>(r->task[0]),
                                     static_cast<unsigned long long>('b'));
        check("where — только имя файла", r->where[0] == 'a' && r->where[1] == 'p');
        check("crc заполнен", r->crc != 0u);
    }

    check("was_fault_reset() == true", embed::fault::was_fault_reset());
    check("init() после capture не портит запись",
          embed::fault::init() && embed::fault::last()->pc == 0x08001234u);

    // report() обязан быть безопасен в любом состоянии (на mock печатает в stdout).
    embed::fault::report();

    embed::fault::clear();
    check("clear() стирает запись", embed::fault::last() == nullptr);
    check("после clear() was_fault_reset() == false",
          !embed::fault::was_fault_reset());

    // ── BKP-метка: .noinit потерян (потеря питания), но тип аварии остался ──
    const embed::platform::Ops& ops = embed::platform::ops();
    check("watchdog note_write доступен",
          ops.watchdog != nullptr && ops.watchdog->note_write != nullptr);
    if (ops.watchdog != nullptr && ops.watchdog->note_write != nullptr) {
        const uint16_t note = static_cast<uint16_t>(
            embed::fault::kNoteMagic |
            static_cast<uint16_t>(embed::fault::Kind::BusFault));
        check("BKP-метка записана", ops.watchdog->note_write(
              embed::fault::kNoteIndex, note));
        check("init() восстанавливает тип из BKP", embed::fault::init());
        const embed::fault::Record* bkp = embed::fault::last();
        check("... и запись помечена bkp",
              bkp != nullptr && bkp->where[0] == 'b' && bkp->where[1] == 'k');
        check_eq<unsigned long long>("... kind == BusFault",
              bkp != nullptr ? bkp->kind : 0u,
              static_cast<unsigned long long>(embed::fault::Kind::BusFault));
    }
    embed::fault::clear();
    check("после BKP-теста запись снова пуста",
          embed::fault::last() == nullptr);

    check("kind_name(HardFault) корректен",
          embed::fault::kind_name(embed::fault::Kind::HardFault)[0] == 'H');
    check("MCFRAME_ASSERT(true) не срабатывает", (MCFRAME_ASSERT(true), true));
}

// ─────────────────────── очередь ожидания и приоритеты ───────────────────────
void test_wait_queue() {
    (void)std::puts("wait queue:");

    embed::wait_queue q;
    embed::task a, b, c;
    (void)a.init(&body, nullptr, "a", 32u, 5u);
    (void)b.init(&body, nullptr, "b", 32u, 1u);
    (void)c.init(&body, nullptr, "c", 32u, 1u);

    check("push(a)", q.push(&a));
    check("push(b)", q.push(&b));
    check("push(c)", q.push(&c));
    check_eq<unsigned>("размер очереди", static_cast<unsigned>(q.size()), 3u);

    // b и c одного приоритета: FIFO, поэтому b раньше c; a — последний.
    check("первым идёт высший приоритет", q.front() == &b);
    check_eq<unsigned long long>("pop -> b",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(q.pop_highest())),
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&b)));
    check_eq<unsigned long long>("pop -> c",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(q.pop_highest())),
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&c)));
    check_eq<unsigned long long>("pop -> a",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(q.pop_highest())),
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&a)));
    check("очередь пуста", q.empty());

    check("повторный push(a)", q.push(&a));
    check("повторная постановка отклонена", !q.push(&a));
    check("remove(a)", q.remove(&a));
    check("remove(a) повторно — false", !q.remove(&a));
    check("очередь пуста после remove", q.empty());
}

// ──────────────────── планировщик: PI, таймаут, стек ────────────────────
void test_scheduler_objects() {
    (void)std::puts("scheduler objects:");

    using embed::Status;
    using embed::scheduler;
    using embed::task;

    check("memory init", embed::g_memory_manager.init(4096u, 512u) == Status::Ok);

    scheduler& s = scheduler::instance();
    check("scheduler init", s.init() == Status::Ok);

    static task low, high;
    static uint32_t low_stack[96];
    static uint32_t high_stack[96];

    check("low init", low.init(&body, nullptr, "low", 96u, 5u) == Status::Ok);
    check("add low", s.add_task(&low, low_stack, 96u));
    check("start", s.start() == Status::Ok);
    check("первая задача — low", s.current_task() == &low);

    // Сильная реализация mcframe_fault_task_name() должна подставить имя
    // текущей задачи, если платформа не передала его сама.
    embed::fault::clear();
    embed::fault::Info auto_task{};
    auto_task.kind = embed::fault::Kind::Panic;
    auto_task.pc   = 0x08000001u;
    embed::fault::capture(auto_task);
    const embed::fault::Record* at = embed::fault::last();
    check("имя задачи подставлено планировщиком",
          at != nullptr && at->task[0] == 'l' && at->task[1] == 'o');
    embed::fault::clear();

    check("high init", high.init(&body, nullptr, "high", 96u, 1u) == Status::Ok);
    check("add high", s.add_task(&high, high_stack, 96u));
    check("high ещё не вытеснил low (mock)",
          s.current_task() == &low);

    embed::mutex m;
    check("low захватил мьютекс", m.lock() == Status::Ok);
    check("владелец — low", m.owner() == &low);

    // Переключаем ядро на high (на mock — учётно, без ассемблера).
    s.context_switch();
    check("high стал текущим", s.current_task() == &high);

    const Status st = m.lock(0u);
    check("high упёрся в занятый мьютекс",
          st == Status::Ok || st == Status::Fault);
    check("high стоит в очереди ожидания", m.waiters() == 1u);
    check_eq<unsigned long long>("владелец унаследовал приоритет high",
          low.priority(), high.priority());

    m.unlock();
    check_eq<unsigned long long>("после unlock приоритет low восстановлен",
          low.priority(), low.base_priority());
    check("мьютекс свободен", !m.locked());
    check("high разбужен", high.is_ready() || high.is_running());

    // Действующий приоритет меняется отдельно от назначенного.
    const uint32_t base = low.base_priority();
    check("set_effective_priority(1)", s.set_effective_priority(&low, 1u) == Status::Ok);
    check_eq<unsigned long long>("действующий приоритет 1", low.priority(), 1u);
    check_eq<unsigned long long>("назначенный приоритет не тронут",
          low.base_priority(), base);
    check("вернуть приоритет", s.set_effective_priority(&low, base) == Status::Ok);

    // ── Сторожевой маркер стека ──
    check("стек low цел", s.stack_guard_ok(&low));
    low_stack[0] = 0u;
    check("затёртый маркер обнаружен", !s.stack_guard_ok(&low));
    low_stack[0] = embed::kStackGuardMagic;
    check("маркер восстановлен", s.stack_guard_ok(&low));

    // ── Таймаут: задача засыпает, тик выставляет timed_out ──
    // Снимаем high, чтобы текущей стала low (приоритет выше, чем idle).
    (void)s.remove_task(&high);
    s.context_switch();
    check("low снова текущая", s.current_task() == &low);

    check("block_on(3) принят", s.block_on(nullptr, 3u) == Status::Ok);
    check("low спит", low.is_sleeping());
    s.advance_ticks(3u);
    check("после 3 тиков low проснулась", low.is_ready());
    check("... по таймауту, а не по событию", low.timed_out());

    // Возвращаем low на ядро: block_on() сам сбросит старый timed_out.
    s.context_switch();
    check("low снова running", low.is_running());

    // ── wake_pending: пробуждение, пришедшее ДО block_on, не теряется ──
    check("wakeup(running) -> pending", s.wakeup(&low) == Status::Ok);
    check("block_on видит pending и не засыпает",
          s.block_on(nullptr, 5u) == Status::Ok && low.is_running());
    check("... старый timed_out сброшен", !low.timed_out());

    // ── Семафор: очередь ожидания и сигнал ──
    embed::semaphore<embed::sem_type::binary> sem;
    sem.signal();
    check("семафор: wait забирает сигнал", sem.wait(0u) == Status::Ok);
    check("семафор: счётчик пуст", sem.value() == 0u);
}

} // namespace

int main() {
    (void)std::puts("=== MCFrame P0 tests (fault/wait) ===");
    test_fault_record();
    test_wait_queue();
    test_scheduler_objects();

    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed != 0) {
        (void)std::printf(", провалено: %d\n", g_failed);
        return 1;
    }
    (void)std::printf(" — все тесты пройдены\n");
    return 0;
}
