// mutex.hpp
#pragma once

#include "types.hpp"
#include "../../common.hpp"
#include "../task/task.hpp"       // wait_queue + TaskHandler
#include <cstdint>

namespace embed {

/// Планировщик предоставляет эти операции (реализация в sheduler.cpp).
/// Объявлены здесь, чтобы примитивы не тянули весь заголовок планировщика.
namespace sched_api {
    /// Текущая задача или nullptr.
    TaskHandler current() noexcept;

    /// Монотонное время в тиках: база АБСОЛЮТНЫХ таймаутов.
    uint64_t now_ticks() noexcept;

    /// Заблокировать текущую задачу без очереди (совместимость, async).
    /// ticks == 0 — без таймаута. Возврат: Ok | Timeout | Fault | ...
    Status block_on(void* obj, uint32_t ticks) noexcept;

    /// Заблокировать текущую задачу, УЖЕ стоя под lock и проверив предикат.
    ///
    /// Критическая секция переходит сюда: функция ставит задачу в очередь
    /// ожидания, снимает с процессора и сама делает unlock_early() перед
    /// yield(). Только так проверка предиката и сон — одна операция, и
    /// пробуждение из прерывания не теряется.
    ///
    /// ticks — ОСТАТОК до абсолютного дедлайна (0 — без таймаута).
    Status block_on_locked(Lock& lock, wait_queue& q, void* obj,
                           uint32_t ticks) noexcept;

    /// Разбудить самую приоритетную задачу из очереди q.
    bool unblock_one(wait_queue& q) noexcept;

    /// Совместимость: разбудить первую задачу, ждущую на obj.
    bool unblock_one(void* obj) noexcept;

    /// Разбудить конкретную задачу (для асинхронных операций).
    bool wake(TaskHandler t) noexcept;

    /// Уступить процессор, не блокируясь.
    void yield() noexcept;

    /// Приоритеты: set_priority меняет назначенный и действующий,
    /// set_effective_priority — только действующий (PI/ceiling).
    Status set_priority(TaskHandler t, uint32_t priority) noexcept;
    Status set_effective_priority(TaskHandler t, uint32_t priority) noexcept;
} // namespace sched_api

/// «Потолок приоритета не задан» (см. priority ceiling).
inline constexpr uint32_t kNoCeiling = 0xFFFFFFFFu;

/// Мьютекс с очередью ожидания, наследованием приоритета и потолком.
///
/// История: спинлок на load/store (TOCTOU + busy-wait) заменён блокировкой
/// задачи. Теперь добавлены три вещи, без которых мьютекс в RTOS — источник
/// непредсказуемых задержек:
///
///  * ОЧЕРЕДЬ ОЖИДАНИЯ: разбуженный выбирается по приоритету (FIFO внутри
///    приоритета), а не «первый в реестре задач». Раньше unblock_one()
///    линейно сканировал все задачи и мог разбудить низкоприоритетную.
///  * PRIORITY INHERITANCE: пока высокоприоритетная задача ждёт, владелец
///    наследует её приоритет. Без этого средняя задача вытесняет владельца
///    и высокоприоритетная ждёт неопределённо долго.
///  * PRIORITY CEILING: необязательный потолок — владелец сразу
///    поднимается до него. Проще PI и достаточно для многих систем.
///
/// Ограничение: реализовано одноуровневое наследование. Вложенные мьютексы
/// с разными потолками могут временно оставить приоритет выше базового;
/// для F103 с 8 приоритетами и статическими объектами это осознанный
/// компромисс, а не недосмотр.
class mutex {
    TaskHandler owner_ = nullptr;
    uint32_t    depth_ = 0;      // поддержка рекурсивного захвата
    bool        recursive_ = false;

    uint32_t    ceiling_ = kNoCeiling;
    bool        ceiling_active_ = false;
    uint32_t    saved_ceiling_priority_ = 0;

    bool        boosted_ = false;
    uint32_t    saved_owner_priority_ = 0;

    wait_queue  waiters_;

    void apply_ceiling(TaskHandler self) noexcept {
        if (ceiling_ == kNoCeiling || self == nullptr) return;
        if (self->priority() <= ceiling_) return;

        saved_ceiling_priority_ = self->priority();
        ceiling_active_ = true;
        (void)sched_api::set_effective_priority(self, ceiling_);
    }

    void restore_ceiling(TaskHandler self) noexcept {
        if (!ceiling_active_) return;
        if (self != nullptr) {
            (void)sched_api::set_effective_priority(self, saved_ceiling_priority_);
        }
        ceiling_active_ = false;
    }

    /// Владелец наследует приоритет ожидающего, если тот выше.
    void boost_owner(TaskHandler self) noexcept {
        if (owner_ == nullptr || owner_ == self) return;
        if (self->priority() >= owner_->priority()) return;

        if (!boosted_) {
            saved_owner_priority_ = owner_->priority();
            boosted_ = true;
        }
        (void)sched_api::set_effective_priority(owner_, self->priority());
    }

    void restore_owner_priority() noexcept {
        if (!boosted_) return;
        if (owner_ != nullptr) {
            (void)sched_api::set_effective_priority(owner_, saved_owner_priority_);
        }
        boosted_ = false;
    }

public:
    explicit mutex(bool recursive = false,
                   uint32_t ceiling = kNoCeiling) noexcept
        : recursive_(recursive), ceiling_(ceiling) {}
    mutex(const mutex&)            = delete;
    mutex& operator=(const mutex&) = delete;

    /// Захват. ticks == 0 — без таймаута, иначе АБСОЛЮТНЫЙ дедлайн:
    /// серия ложных пробуждений не продлевает ожидание.
    Status lock(uint32_t ticks = 0) noexcept {
        const uint64_t deadline = (ticks != 0u)
            ? (sched_api::now_ticks() + static_cast<uint64_t>(ticks))
            : 0u;

        for (;;) {
            Lock l;
            TaskHandler self = sched_api::current();
            if (self == nullptr) return Status::Unavailable;

            if (owner_ == nullptr) {
                owner_ = self;
                depth_ = 1u;
                apply_ceiling(self);
                return Status::Ok;
            }
            if (recursive_ && owner_ == self) {
                ++depth_;
                return Status::Ok;
            }

            // Владелец — другая задача. Поднимаем его приоритет ДО сна:
            // иначе он может быть вытеснен и мы не дождёмся никогда.
            boost_owner(self);

            uint32_t remaining = 0u;
            if (deadline != 0u) {
                const uint64_t now = sched_api::now_ticks();
                if (now >= deadline) return Status::Timeout;
                const uint64_t left = deadline - now;
                remaining = (left > 0xFFFFFFFFu)
                    ? 0xFFFFFFFFu
                    : static_cast<uint32_t>(left);
            }

            const Status st = sched_api::block_on_locked(l, waiters_, this, remaining);
            if (st != Status::Ok) return st;
        }
    }

    bool try_lock() noexcept {
        Lock l;
        TaskHandler self = sched_api::current();
        if (self == nullptr) return false;
        if (owner_ == nullptr) {
            owner_ = self;
            depth_ = 1u;
            apply_ceiling(self);
            return true;
        }
        if (recursive_ && owner_ == self) {
            ++depth_;
            return true;
        }
        return false;
    }

    void unlock() noexcept {
        Lock l;
        if (depth_ == 0u) return;            // разблокировка без захвата
        if (--depth_ != 0u) return;

        TaskHandler self = owner_;
        // Сначала снимаем PI-подъём, затем ceiling: иначе ceiling вернёт
        // приоритет, сохранённый ДО подъёма, и база потеряется.
        restore_owner_priority();
        restore_ceiling(self);
        owner_ = nullptr;

        // Будим самого приоритетного. Очередь живёт под тем же Lock:
        // сигнал не может «проскочить» между снятием владельца и wake.
        if (!waiters_.empty()) {
            (void)sched_api::unblock_one(waiters_);
        }
    }

    TaskHandler owner() const noexcept { return owner_; }
    bool locked() const noexcept { return owner_ != nullptr; }
    Size waiters() const noexcept { return waiters_.size(); }
};

template <typename M = mutex>
class lock_guard {
    M&      m_;
    bool    owns_ = false;
public:
    explicit lock_guard(M& m) noexcept : m_(m) { owns_ = (m_.lock() == Status::Ok); }
    ~lock_guard() noexcept { if (owns_) m_.unlock(); }

    lock_guard(const lock_guard&)            = delete;
    lock_guard& operator=(const lock_guard&) = delete;

    bool owns() const noexcept { return owns_; }
};

}
