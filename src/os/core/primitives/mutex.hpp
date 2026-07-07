// mutex.hpp
#pragma once

#include "types.hpp"

namespace embed {

    /// Простой спинлок на atomic_t. Без рекурсии, без приоритетов.
    class mutex {
        atomic_t<uint32_t> flag_{0};

    public:
        mutex() noexcept = default;
        mutex(const mutex&)            = delete;
        mutex& operator=(const mutex&) = delete;

        // ──────────── Lock / Unlock ────────────

        /// Захватить. Крутится пока не освободится.
        void lock() noexcept {
            while (flag_.load(MemoryOrderAcquire) != 0) {
                // spin
            }
            flag_.store(1, MemoryOrderRelease);
        }

        /// Попробовать захватить без ожидания. true если удалось.
        bool try_lock() noexcept {
            if (flag_.load(MemoryOrderAcquire) != 0) return false;
            flag_.store(1, MemoryOrderRelease);
            return true;
        }

        /// Освободить.
        void unlock() noexcept {
            flag_.store(0, MemoryOrderRelease);
        }
    };

    /// RAII-обёртка для автоматического захвата/освобождения.
    template <typename M = mutex>
    class lock_guard {
        M& m_;

    public:
        explicit lock_guard(M& m) noexcept : m_(m) { m_.lock(); }
        ~lock_guard() noexcept { m_.unlock(); }

        lock_guard(const lock_guard&)            = delete;
        lock_guard& operator=(const lock_guard&) = delete;
    };

} // namespace embed