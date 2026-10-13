// mutex.hpp
#pragma once

#include "types.hpp"
#include "../../common.hpp"
#include <cstdint>

namespace embed {

class task;
using TaskHandler = task*;

/// Планировщик предоставляет эти операции (реализация в sheduler.cpp).
/// Объявлены здесь, чтобы примитивы не тянули весь заголовок планировщика.
namespace sched_api {
    /// Текущая задача или nullptr.
    TaskHandler current() noexcept;
    /// Заблокировать текущую задачу на объекте obj на ticks тиков
    /// (0 — без таймаута). Возвращает Status::Ok, если блокировка выполнена.
    Status block_on(void* obj, uint32_t ticks) noexcept;
    /// Разбудить одну задачу, ждущую на obj.
    bool unblock_one(void* obj) noexcept;
} // namespace sched_api

/// Мьютекс.
///
/// Прежняя реализация была спинлоком на load/store:
///  * проверка и установка флага не были атомарны (TOCTOU);
///  * ожидающая задача жгла CPU в `while (flag != 0) {}`, и при вытеснении
///    владельца это превращалось в вечное зависание;
///  * прерывания не маскировались, поэтому ISR мог видеть «взято» и
///    крутиться до конца прерывания.
///
/// Теперь: критическая секция только вокруг проверки/модификации состояния,
/// а ожидание уходит в планировщик (задача спит, а не крутится).
class mutex {
    TaskHandler owner_ = nullptr;
    uint32_t    depth_ = 0;      // поддержка рекурсивного захвата
    bool        recursive_ = false;

public:
    explicit mutex(bool recursive = false) noexcept : recursive_(recursive) {}
    mutex(const mutex&)            = delete;
    mutex& operator=(const mutex&) = delete;

    /// Захват. ticks == 0 — без таймаута.
    Status lock(uint32_t ticks = 0) noexcept {
        for (;;) {
            TaskHandler self;
            {
                Lock l;
                self = sched_api::current();
                if (owner_ == nullptr) { owner_ = self; depth_ = 1u; return Status::Ok; }
                if (recursive_ && owner_ == self) { ++depth_; return Status::Ok; }
                if (self == nullptr) return Status::Unavailable;  // нет планировщика
            }
            // Владелец — другая задача: блокируемся, НЕ крутимся.
            const Status st = sched_api::block_on(this, ticks);
            if (st != Status::Ok) return st;
        }
    }

    bool try_lock() noexcept {
        Lock l;
        TaskHandler self = sched_api::current();
        if (owner_ == nullptr) { owner_ = self; depth_ = 1u; return true; }
        if (recursive_ && owner_ == self) { ++depth_; return true; }
        return false;
    }

    void unlock() noexcept {
        bool wake = false;
        {
            Lock l;
            if (depth_ == 0u) return;            // разблокировка без захвата
            if (--depth_ != 0u) return;
            owner_ = nullptr;
            wake = true;
        }
        if (wake) (void)sched_api::unblock_one(this);
    }

    TaskHandler owner() const noexcept { return owner_; }
    bool locked() const noexcept { return owner_ != nullptr; }
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
