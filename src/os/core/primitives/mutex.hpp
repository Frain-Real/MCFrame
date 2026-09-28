#pragma once

#include "types.hpp"
#include <cstdint>

namespace embed {
    class mutex {
        atomic_t<uint32_t> flag_{0};

    public:
        mutex() noexcept = default;
        mutex(const mutex&)            = delete;
        mutex& operator=(const mutex&) = delete;

        //Lock / Unlock

        void lock() noexcept {
            while (flag_.load(MemoryOrderAcquire) != 0) {}
            flag_.store(1, MemoryOrderRelease);
        }

        bool try_lock() noexcept {
            if (flag_.load(MemoryOrderAcquire) != 0) return false;
            flag_.store(1, MemoryOrderRelease);
            return true;
        }

        void unlock() noexcept {
            flag_.store(0, MemoryOrderRelease);
        }
    };

    template <typename M = mutex>
    class lock_guard {
        M& m_;

    public:
        explicit lock_guard(M& m) noexcept : m_(m) { m_.lock(); }
        ~lock_guard() noexcept { m_.unlock(); }

        lock_guard(const lock_guard&)            = delete;
        lock_guard& operator=(const lock_guard&) = delete;
    };

}