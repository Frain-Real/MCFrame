// mutex.hpp — template mutex, kind encoded in the type
//
//   Normal   : Mutex<NonRecursive>            // standard, non-reentrant
//   Recursive: Mutex<Recursive>               // same thread may lock N times
//
// Properties:
//   - Lock-free fast path (single CAS)
//   - No dynamic allocation
//   - Optional timed try_lock
//   - Compile-time selection of reentrancy (no runtime branch on lock path)
//   - ISR-unsafe by design — must be used from thread context only
//
// Header-only. Requires C++17 (C++20 recommended for std::atomic::wait).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

#if defined(__has_include) && __has_include(<linux/futex.h>)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace embedded {

    struct NonRecursive {};
    struct Recursive    {};

    // ---------------------------------------------------------------------------
    //  Owner-id encoding for recursive mutex:
    //  - 0            => unlocked
    //  - upper bits   => owner thread id (pthread_t on Linux)
    //  - lower bits   => recursion depth
    //  We pack them into a single 64-bit atomic so lock is one CAS.
    // ---------------------------------------------------------------------------
    namespace detail {

        using owner_t = std::uint64_t;

        inline owner_t encode_owner(std::uintptr_t tid, std::uint32_t depth) noexcept {
            return (static_cast<owner_t>(tid) << 32) |
            static_cast<owner_t>(depth & 0xFFFFFFFFu);
        }

        inline std::uintptr_t owner_tid(owner_t v) noexcept {
            return static_cast<std::uintptr_t>(v >> 32);
        }

        inline std::uint32_t owner_depth(owner_t v) noexcept {
            return static_cast<std::uint32_t>(v & 0xFFFFFFFFu);
        }

        inline std::uintptr_t current_thread_id() noexcept {
            // Linux: pthread_t is opaque but unique per thread for our purposes
            // (cast to integer via thread::id hash). On bare-metal replace with
            // your task handle.
            return static_cast<std::uintptr_t>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
        }

        inline void wait_on(const std::atomic<std::int32_t>& v, std::int32_t expected) noexcept {
            #if defined(__has_include) && __has_include(<linux/futex.h>)
            ::syscall(SYS_futex,
                      reinterpret_cast<std::intptr_t>(const_cast<std::atomic<std::int32_t>*>(&v)),
                      FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                      expected, nullptr, nullptr, 0);
            #else
            (void)v; (void)expected;
            std::this_thread::yield();
            #endif
        }

        inline void wake_one(const std::atomic<std::int32_t>& v) noexcept {
            #if defined(__has_include) && __has_include(<linux/futex.h>)
            ::syscall(SYS_futex,
                      reinterpret_cast<std::intptr_t>(const_cast<std::atomic<std::int32_t>*>(&v)),
                      FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                      1, nullptr, nullptr, 0);
            #else
            (void)v;
            #endif
        }

        inline void wake_all(const std::atomic<std::int32_t>& v) noexcept {
            #if defined(__has_include) && __has_include(<linux/futex.h>)
            ::syscall(SYS_futex,
                      reinterpret_cast<std::intptr_t>(const_cast<std::atomic<std::int32_t>*>(&v)),
                      FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                      static_cast<std::int32_t>(0x7fffffff),
                      nullptr, nullptr, 0);
            #else
            (void)v;
            #endif
        }

    } // namespace detail

    // ---------------------------------------------------------------------------
    //  Non-recursive mutex
    // ---------------------------------------------------------------------------
    template <typename RecursivePolicy>
    class Mutex;

    template <>
    class Mutex<NonRecursive> {
    public:
        static constexpr bool is_recursive = false;
        using policy = NonRecursive;

        constexpr Mutex() noexcept = default;

        Mutex(const Mutex&) = delete;
        Mutex& operator=(const Mutex&) = delete;

        void lock() noexcept {
            if (try_lock()) return;

            // contended path: park on futex until state goes to 0
            std::int32_t expected = 1;  // we observed locked
            while (state_.load(std::memory_order_relaxed) != 0) {
                // set waiter bit (high bit) to opt into FUTEX_WAIT semantics that
                // survive spurious wakeups and notify the next unlock
                std::int32_t s = state_.load(std::memory_order_relaxed);
                if (s == 0) break;  // raced with unlock
                detail::wait_on(state_, s);
            }
            // after wakeup, try to acquire
            while (!try_lock()) {
                detail::wait_on(state_, 1);
            }
            (void)expected;
        }

        bool try_lock() noexcept {
            std::int32_t expected = 0;  // unlocked
            return state_.compare_exchange_strong(
                expected, 1,
                std::memory_order_acquire,
                std::memory_order_relaxed);
        }

        template <typename Rep, typename Period>
        bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout) noexcept {
            if (try_lock()) return true;
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (true) {
                if (try_lock()) return true;
                if (std::chrono::steady_clock::now() >= deadline) return false;
                std::int32_t s = state_.load(std::memory_order_relaxed);
                if (s != 0) detail::wait_on(state_, s);
            }
        }

        bool try_lock_until(const std::chrono::steady_clock::time_point& tp) noexcept {
            return try_lock_for(tp - std::chrono::steady_clock::now());
        }

        void unlock() noexcept {
            state_.store(0, std::memory_order_release);
            detail::wake_one(state_);
        }

        bool locked() const noexcept {
            return state_.load(std::memory_order_acquire) != 0;
        }

    private:
        // 0 = unlocked, 1 = locked (no contention marker — futex recheck handles it)
        std::atomic<std::int32_t> state_{0};
    };

    // ---------------------------------------------------------------------------
    //  Recursive mutex
    // ---------------------------------------------------------------------------
    template <>
    class Mutex<Recursive> {
    public:
        static constexpr bool is_recursive = true;
        using policy = Recursive;

        constexpr Mutex() noexcept = default;

        Mutex(const Mutex&) = delete;
        Mutex& operator=(const Mutex&) = delete;

        void lock() noexcept {
            const auto tid = detail::current_thread_id();
            detail::owner_t cur = state_.load(std::memory_order_relaxed);

            // fast path: same thread re-locks -> just bump depth
            if (cur != 0 && detail::owner_tid(cur) == tid) {
                if (try_bump_depth(cur)) return;
            }

            // otherwise: spin until we own the mutex
            while (true) {
                cur = state_.load(std::memory_order_relaxed);
                if (cur == 0) {
                    if (try_acquire_fresh(tid)) return;
                    continue;
                }
                if (detail::owner_tid(cur) == tid) {
                    if (try_bump_depth(cur)) return;
                    continue;
                }
                // contended on a foreign owner
                detail::wait_on(contention_, 1);
            }
        }

        bool try_lock() noexcept {
            const auto tid = detail::current_thread_id();
            detail::owner_t cur = state_.load(std::memory_order_relaxed);
            if (cur == 0) {
                return try_acquire_fresh(tid);
            }
            if (detail::owner_tid(cur) == tid) {
                return try_bump_depth(cur);
            }
            return false;
        }

        template <typename Rep, typename Period>
        bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout) noexcept {
            const auto tid = detail::current_thread_id();
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (true) {
                if (try_lock()) return true;
                if (std::chrono::steady_clock::now() >= deadline) return false;
                detail::owner_t cur = state_.load(std::memory_order_relaxed);
                if (cur != 0 && detail::owner_tid(cur) != tid) {
                    detail::wait_on(contention_, 1);
                }
            }
        }

        bool try_lock_until(const std::chrono::steady_clock::time_point& tp) noexcept {
            return try_lock_for(tp - std::chrono::steady_clock::now());
        }

        void unlock() noexcept {
            const auto tid = detail::current_thread_id();
            detail::owner_t cur = state_.load(std::memory_order_relaxed);

            // debug: only the owner may unlock
            assert(cur != 0 && detail::owner_tid(cur) == tid &&
            "unlock() from non-owner thread");

            const std::uint32_t depth = detail::owner_depth(cur);
            if (depth > 1) {
                // decrement depth
                detail::owner_t expected = cur;
                state_.compare_exchange_strong(
                    expected, detail::encode_owner(tid, depth - 1),
                                               std::memory_order_release,
                                               std::memory_order_relaxed);
                // no wake: mutex still owned by us
            } else {
                // depth == 1: fully release
                state_.store(0, std::memory_order_release);
                contention_.store(0, std::memory_order_release);
                detail::wake_one(contention_);
            }
        }

        bool locked() const noexcept {
            return state_.load(std::memory_order_acquire) != 0;
        }

        std::uint32_t depth() const noexcept {
            return detail::owner_depth(state_.load(std::memory_order_acquire));
        }

    private:
        bool try_acquire_fresh(std::uintptr_t tid) noexcept {
            detail::owner_t expected = 0;
            const detail::owner_t desired = detail::encode_owner(tid, 1);
            if (state_.compare_exchange_strong(
                expected, desired,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
                contention_.store(1, std::memory_order_relaxed);
            return true;
                }
                return false;
        }

        bool try_bump_depth(detail::owner_t cur) noexcept {
            const auto tid = detail::owner_tid(cur);
            const std::uint32_t depth = detail::owner_depth(cur);
            if (depth == 0xFFFFFFFFu) {
                return false;  // overflow guard
            }
            detail::owner_t expected = cur;
            return state_.compare_exchange_strong(
                expected, detail::encode_owner(tid, depth + 1),
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed);
        }

        // owner in upper 32 bits, depth in lower 32 bits
        std::atomic<detail::owner_t> state_{0};
        // separate word used as futex address to wait on contention
        std::atomic<std::int32_t> contention_{0};
    };

    // ---------------------------------------------------------------------------
    //  RAII lock guard
    // ---------------------------------------------------------------------------
    template <typename RecursivePolicy>
    class LockGuard {
    public:
        explicit LockGuard(Mutex<RecursivePolicy>& m) noexcept : m_(m) { m_.lock(); }
        ~LockGuard() { m_.unlock(); }

        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;

    private:
        Mutex<RecursivePolicy>& m_;
    };

    // ---------------------------------------------------------------------------
    //  Aliases
    // ---------------------------------------------------------------------------
    using MutexNR    = Mutex<NonRecursive>;
    using MutexRec   = Mutex<Recursive>;

    using LockGuardNR  = LockGuard<NonRecursive>;
    using LockGuardRec = LockGuard<Recursive>;

    static_assert(!MutexNR::is_recursive,  "NonRecursive must not be recursive");
    static_assert( MutexRec::is_recursive, "Recursive must be recursive");

} // namespace embedded
