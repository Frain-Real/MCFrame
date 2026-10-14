// async_completion.hpp
#pragma once
// ---------------------------------------------------------------------------
// Объект завершения асинхронной операции — ядро модуля async IO.
//
// ЗАЧЕМ ЭТО НУЖНО
// Пользователь пишет линейный код:
//
//     port.write(buf, len);      // выглядит как блокирующий вызов
//     // следующая строка выполняется уже после передачи
//
// Внутри задача НЕ крутится в цикле: она усыпляется, а будит её либо
// обработчик прерывания/DMA (когда передача завершена), либо отсчёт
// таймаута. Пользовательских колбэков писать не нужно — колбэк один
// и спрятан внутри модуля.
//
// ПОЧЕМУ НЕ semaphore ИЗ ЯДРА
// Семафор сообщает «событие произошло», но не несёт РЕЗУЛЬТАТ: сколько
// байт передано и с каким статусом. Для ввода-вывода это обязательно —
// иначе нельзя отличить «передано 5 из 10» от «ошибка канала».
//
// ПОЧЕМУ ОДНОРАЗОВЫЙ
// Объект используется на одну операцию и переиспользуется через reset().
// Это исключает ошибку «событие пришло от ПРЕДЫДУЩЕЙ операции»: без
// сброса защёлкнутое состояние сразу завершало бы следующий вызов.
// ---------------------------------------------------------------------------

#include "async_kernel.hpp"
#include "../core/primitives/types.hpp"   // atomic_t / atomic_bool / MemoryOrder

namespace embed::async {

/// Ограничение времени ожидания.
enum class WaitPolicy : uint8_t {
    /// Ждать, пока операция не завершится.
    Forever = 0,
    /// Ждать не дольше timeout_ms.
    Timeout,
};

/// Результат завершения операции.
struct Completion {
    Status status      = Status::Busy;
    Size   transferred = 0u;
};

/// Колбэк завершения. Вызывается тем, кто завершил операцию: обработчиком
/// прерывания/DMA либо циклом ожидания. Поэтому обязан быть коротким —
/// без блокировок, без mutex, без sleep.
using DoneCallback = void (*)(const Completion& result, void* arg) noexcept;

class completion;   // объявлен ниже

/// Опрос интерфейса. Вызывается модулем В ЦИКЛЕ ОЖИДАНИЯ.
/// Это и есть точка входа для стратегии Polling: HAL опрашивает регистры
/// и, когда передача закончена, сам завершает объект через post().
using PollCallback = void (*)(completion& self, void* arg) noexcept;

namespace detail {
/// Сколько уступок допускается, если время недоступно (таймер не запущен).
/// Нужно, чтобы ожидание не превращалось в бесконечный цикл.
inline constexpr uint32_t kNoTimerYieldBudget = 10000u;
} // namespace detail

/// Одноразовый объект завершения операции ввода-вывода.
class completion {
public:
    completion() noexcept = default;

    completion(const completion&)            = delete;
    completion& operator=(const completion&) = delete;

    // ─────────────────────────── Подготовка ───────────────────────────

    /// Подготовить объект к НОВОЙ операции. Вызывать перед каждым запуском.
    void reset(WaitPolicy policy = WaitPolicy::Forever,
               uint32_t   timeout_ms = 0u,
               DoneCallback cb = nullptr,
               void*      cb_arg = nullptr) noexcept {
        status_      = Status::Busy;
        transferred_ = 0u;
        cb_          = cb;
        cb_arg_      = cb_arg;
        policy_      = policy;
        timeout_ms_  = timeout_ms;
        start_ms_    = 0u;
        spin_budget_ = detail::kNoTimerYieldBudget;
        // Снимаем «припаркованность» обычным store (Relaxed): строгий порядок
        // задаёт следующий за ним store(done_, Release) — он не даст этим
        // записям уехать после снятия защёлки.
        waiting_.store(nullptr, MemoryOrderRelaxed);

        // Защёлку снимаем ПОСЛЕДНЕЙ: прерывание, пришедшее в этот момент,
        // увидит уже согласованное состояние.
        done_.store(false, MemoryOrderRelease);
    }

    /// Начать отсчёт таймаута. Вызывается непосредственно перед стартом
    /// передачи, чтобы не включать в таймаут время подготовки.
    void begin() noexcept {
        if (policy_ == WaitPolicy::Timeout && kernel::has_time()) {
            start_ms_ = kernel::time_ms();
        }
    }

    // ─────────────────────────── Завершение ───────────────────────────

    /// Завершить операцию. Безопасно вызывать ИЗ ПРЕРЫВАНИЯ.
    ///
    /// Возвращает false, если объект уже был завершён: так отсекаются
    /// повторные прерывания (HT+TC у DMA, «дребезг» флагов).
    bool post(const Completion& result) noexcept {
        bool expected = false;
        // CAS false -> true: только первый вызов «выигрывает» защёлку.
        // strong, а не weak: здесь НЕТ цикла, а слабая версия вправе вернуть
        // false без реального несовпадения — операция была бы потеряна.
        if (!done_.compare_exchange_strong(expected, true,
                                          MemoryOrderAcqRel,
                                          MemoryOrderAcquire)) {
            return false;
        }

        status_      = result.status;
        transferred_ = result.transferred;

        if (cb_ != nullptr) {
            const Completion snapshot = result;
            cb_(snapshot, cb_arg_);
        }

        // Если задача уже припаркована — будим её.
        // Обмен атомарный: либо задача увидит done_ == true и не уснёт,
        // либо она уже в ожидании и получит wake. Третьего варианта нет.
        TaskHandler w = waiting_.exchange(nullptr, MemoryOrderAcqRel);
        if (w != nullptr) (void)kernel::wake(w);

        return true;
    }

    bool post_ok(Size transferred = 0u) noexcept {
        return post(Completion{Status::Ok, transferred});
    }

    bool post_error(Status st) noexcept {
        return post(Completion{st, 0u});
    }

    // ─────────────────────────── Ожидание ───────────────────────────

    bool done() const noexcept {
        return done_.load(MemoryOrderAcquire);
    }

    /// Дождаться завершения. Возвращает true, если операция завершилась
    /// (смотрите status()), false — если ждать было невозможно
    /// (нет контекста задачи до старта планировщика).
    bool wait() noexcept {
        if (done()) return true;

        if (kernel::in_task()) {
            return kernel::has_time() ? wait_with_timer() : wait_with_budget();
        }

        // Планировщик ещё не запущен (или мы не в задаче): усыпить себя
        // некого. Но опрос интерфейса всё равно нужен — иначе операция
        // никогда не завершится. Крутим ограниченный цикл опроса:
        // он завершается по done() либо по исчерпанию бюджета.
        return wait_without_task();
    }

    /// Опрос без ожидания: удобно в callback/polling-стиле.
    bool poll() noexcept {
        if (done()) return true;

        if (policy_ == WaitPolicy::Timeout && kernel::has_time()) {
            if (elapsed_ms() >= timeout_ms_) {
                (void)post(Completion{Status::Timeout, transferred_});
            }
        }
        return done();
    }

    // ─────────────────────────── Результат ───────────────────────────

    /// Копия результата. Возвращаем ЗНАЧЕНИЕ, а не ссылку: прежде здесь
    /// была ссылка на временный объект (висячая ссылка).
    Completion result() const noexcept {
        return Completion{status_, transferred_};
    }

    Status status()      const noexcept { return status_; }
    Size   transferred() const noexcept { return transferred_; }

    /// Сколько миллисекунд прошло с begin() (0, если время недоступно).
    uint32_t elapsed_ms() const noexcept {
        if (!kernel::has_time()) return 0u;
        return static_cast<uint32_t>(kernel::time_ms() - start_ms_);
    }

    void set_callback(DoneCallback cb, void* arg) noexcept {
        cb_     = cb;
        cb_arg_ = arg;
    }

    /// Задать опрос интерфейса для стратегии Polling.
    void set_poll(PollCallback poll_fn, void* arg) noexcept {
        poll_     = poll_fn;
        poll_arg_ = arg;
    }

    bool has_waiter() const noexcept {
        return waiting_.load(MemoryOrderAcquire) != nullptr;
    }

private:
    /// Остаток таймаута в тиках планировщика, округляя ВВЕРХ: даже остаток
    /// меньше кванта обязан дать одну уступку, иначе таймаут «съедает» тик.
    uint32_t ticks_left() const noexcept {
        constexpr uint32_t kYieldCostMs = 1u;
        const uint64_t now = kernel::time_ms();
        const uint32_t elapsed = static_cast<uint32_t>(now - start_ms_);
        if (elapsed >= timeout_ms_) return 0u;
        return (timeout_ms_ - elapsed + kYieldCostMs - 1u) / kYieldCostMs;
    }

    bool wait_with_timer() noexcept {
        for (;;) {
            if (done()) return true;

            const TaskHandler self = kernel::current();
            if (self == nullptr) return done();

            do_poll();                              // Polling-стратегия

            if (policy_ == WaitPolicy::Timeout) {
                const uint32_t ticks = ticks_left();
                if (ticks == 0u) break;             // время вышло
                park(self);
                if (done()) return true;            // успело завершиться
                (void)kernel::block_on(this, ticks);
            } else {
                park(self);
                if (done()) return true;
                (void)kernel::block_on(this, 0u);   // без ограничения
            }
        }

        (void)post(Completion{Status::Timeout, transferred_});
        return true;
    }

    /// Ожидание без контекста задачи: только опрос + уступка.
    /// Используется, когда планировщик ещё не запущен.
    bool wait_without_task() noexcept {
        for (;;) {
            if (done()) return true;
            if (spin_budget_ == 0u) {
                (void)post(Completion{Status::Timeout, transferred_});
                return true;
            }
            --spin_budget_;
            do_poll();
            if (done()) return true;
            kernel::yield();
        }
    }

    bool wait_with_budget() noexcept {
        for (;;) {
            if (done()) return true;
            if (spin_budget_ == 0u) {
                (void)post(Completion{Status::Timeout, transferred_});
                return true;
            }
            --spin_budget_;
            do_poll();                              // Polling-стратегия
            if (done()) return true;
            kernel::yield();                        // уступаем, не крутимся
        }
    }

    void do_poll() noexcept {
        if (poll_ != nullptr) poll_(*this, poll_arg_);
    }

    /// Припарковать себя как ожидающего. Атомарность обязательна: иначе
    /// возможно окно, где post() не находит ожидающего, а задача уже
    /// уснула — и не проснётся никогда.
    void park(TaskHandler self) noexcept {
        TaskHandler expected = nullptr;
        (void)waiting_.compare_exchange_strong(expected, self,
                                              MemoryOrderAcqRel,
                                              MemoryOrderAcquire);
    }

    /// Защёлка «операция завершена». atomic_t<bool>, а не volatile bool:
    /// пишет её обработчик прерывания, читает задача — volatile не даёт
    /// ни атомарности, ни порядка.
    atomic_bool done_{false};

    Status        status_      = Status::Busy;
    Size          transferred_ = 0u;

    DoneCallback  cb_          = nullptr;
    void*         cb_arg_      = nullptr;
    PollCallback  poll_        = nullptr;
    void*         poll_arg_    = nullptr;

    WaitPolicy    policy_      = WaitPolicy::Forever;
    uint32_t      timeout_ms_  = 0u;
    uint64_t      start_ms_    = 0u;
    uint32_t      spin_budget_ = detail::kNoTimerYieldBudget;

    /// Задача, ожидающая завершения (nullptr — никто не ждёт).
    atomic_t<TaskHandler> waiting_{nullptr};
};

} // namespace embed::async
