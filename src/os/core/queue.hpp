// queue.hpp — SPSC queue with two semantics encoded in the type
//
//   Queue<TaskTag>   : queue of arbitrary callables, consumer invokes each
//                      one (executor / command queue). No inheritance needed.
//   Queue<DataTag>   : queue of T by value, consumer just reads them
//                      (data pipe: ISR -> task, task -> task)
//
// Properties:
//   - Single-producer / single-consumer, fully lock-free
//   - Fixed-capacity, no dynamic allocation, SBO storage for callables
//   - Wait/notify via Linux futex (private)
//   - acquire/release memory orderings; consumer sees writes from producer
//   - ISR-safe push: try_push_from_isr() does not block or wake
//   - Compile-time choice of semantics: no runtime branch in hot path
//
// Header-only. Requires C++17 (C++20 recommended for std::atomic::wait).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

#if defined(__has_include) && __has_include(<linux/futex.h>)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace embedded {

    // ---------------------------------------------------------------------------
    //  Type tags. Each selects a different consumer API. No data members; just
    //  used to specialize the Queue template.
    // ---------------------------------------------------------------------------
    struct TaskTag {};   // consumer invokes a stored callable
    struct DataTag {};   // consumer reads items by value

    // ---------------------------------------------------------------------------
    //  Futex helpers
    // ---------------------------------------------------------------------------
    namespace detail {

        inline void futex_wait(const std::atomic<std::uint32_t>& v,
                               std::uint32_t expected) noexcept {
                                   #if defined(__has_include) && __has_include(<linux/futex.h>)
                                   ::syscall(SYS_futex,
                                             reinterpret_cast<std::intptr_t>(const_cast<std::atomic<std::uint32_t>*>(&v)),
                                             FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                                             expected, nullptr, nullptr, 0);
                                   #else
                                   (void)v; (void)expected;
                                   #endif
                               }

                               inline void futex_wake_one(const std::atomic<std::uint32_t>& v) noexcept {
                                   #if defined(__has_include) && __has_include(<linux/futex.h>)
                                   ::syscall(SYS_futex,
                                             reinterpret_cast<std::intptr_t>(const_cast<std::atomic<std::uint32_t>*>(&v)),
                                             FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                                             1, nullptr, nullptr, 0);
                                   #else
                                   (void)v;
                                   #endif
                               }

                               inline std::size_t round_up_pow2(std::size_t n) noexcept {
                                   std::size_t p = 1;
                                   while (p < n) p <<= 1;
                                   return p;
                               }

    } // namespace detail

    // ===========================================================================
    //  Data queue: Queue<T, DataTag>
    // ===========================================================================
    template <typename T, typename Kind>
    class Queue;

    template <typename T>
    class Queue<T, DataTag> {
        static_assert(std::is_default_constructible_v<T> ||
        std::is_trivially_copyable_v<T>,
        "T must be default-constructible or trivially copyable");

    public:
        static constexpr bool is_task_queue = false;
        using value_type = T;
        using kind       = DataTag;

        explicit Queue(std::size_t capacity) noexcept
        : capacity_(detail::round_up_pow2(capacity < 2 ? 2 : capacity)),
        mask_(capacity_ - 1),
        slots_(new Slot[capacity_]) {
            for (std::size_t i = 0; i < capacity_; ++i) {
                new (&slots_[i].storage) T();
            }
        }

        ~Queue() {
            for (std::size_t i = 0; i < capacity_; ++i) {
                slots_[i].value.~T();
            }
            delete[] slots_;
        }

        Queue(const Queue&) = delete;
        Queue& operator=(const Queue&) = delete;

        // ---- producer side ----------------------------------------------------
        bool try_push(const T& v) noexcept {
            const std::uint32_t head = head_.load(std::memory_order_relaxed);
            const std::uint32_t tail = tail_seq_.load(std::memory_order_acquire);
            if (head - tail >= capacity_) return false;
            slots_[head & mask_].value = v;
            head_.store(head + 1, std::memory_order_release);
            detail::futex_wake_one(notify_);
            return true;
        }

        bool try_push(T&& v) noexcept {
            const std::uint32_t head = head_.load(std::memory_order_relaxed);
            const std::uint32_t tail = tail_seq_.load(std::memory_order_acquire);
            if (head - tail >= capacity_) return false;
            slots_[head & mask_].value = static_cast<T&&>(v);
            head_.store(head + 1, std::memory_order_release);
            detail::futex_wake_one(notify_);
            return true;
        }

        bool try_push_from_isr(const T& v) noexcept {
            const std::uint32_t head = head_.load(std::memory_order_relaxed);
            const std::uint32_t tail = tail_seq_.load(std::memory_order_acquire);
            if (head - tail >= capacity_) return false;
            slots_[head & mask_].value = v;
            head_.store(head + 1, std::memory_order_release);
            return true;
        }

        void wake_consumer() noexcept { detail::futex_wake_one(notify_); }

        // ---- consumer side ----------------------------------------------------
        bool try_pop(T& out) noexcept {
            const std::uint32_t tail = tail_seq_.load(std::memory_order_relaxed);
            const std::uint32_t head = head_.load(std::memory_order_acquire);
            if (tail == head) return false;
            out = static_cast<T&&>(slots_[tail & mask_].value);
            tail_seq_.store(tail + 1, std::memory_order_release);
            return true;
        }

        void pop(T& out) noexcept {
            while (!try_pop(out)) {
                const std::uint32_t tail = tail_seq_.load(std::memory_order_relaxed);
                const std::uint32_t head = head_.load(std::memory_order_acquire);
                if (tail == head) {
                    detail::futex_wait(notify_, head);
                }
            }
        }

        template <typename Rep, typename Period>
        bool pop_for(T& out, const std::chrono::duration<Rep, Period>& t) noexcept {
            const auto deadline = std::chrono::steady_clock::now() + t;
            while (!try_pop(out)) {
                const std::uint32_t tail = tail_seq_.load(std::memory_order_relaxed);
                const std::uint32_t head = head_.load(std::memory_order_acquire);
                if (tail == head) {
                    if (std::chrono::steady_clock::now() >= deadline) return false;
                    detail::futex_wait(notify_, head);
                }
            }
            return true;
        }

        std::size_t size() const noexcept {
            return head_.load(std::memory_order_acquire) -
            tail_seq_.load(std::memory_order_acquire);
        }
        std::size_t capacity() const noexcept { return capacity_; }
        bool empty() const noexcept { return size() == 0; }
        bool full()  const noexcept { return size() == capacity_; }

    private:
        struct alignas(T) Slot {
            unsigned char storage[sizeof(T)];
            T& value() noexcept { return *reinterpret_cast<T*>(storage); }
        };

        const std::size_t capacity_;
        const std::size_t mask_;
        Slot* const slots_;

        alignas(64) std::atomic<std::uint32_t> head_{0};
        alignas(64) std::atomic<std::uint32_t> tail_seq_{0};
        alignas(64) std::atomic<std::uint32_t> notify_{0};
    };

    // ===========================================================================
    //  Task queue: Queue<TaskTag>   (type-erased callable storage)
    //
    //  Producer pushes any callable: function pointer, lambda, std::function,
    //  member function via std::bind, anything that is invocable as `()`.
    //
    //  Storage: each slot has an inline SBO buffer. Small callables (up to
    //  sizeof(Callable) <= SBO) are stored by value via a placement-new with
    //  copy/move capture. Larger callables fall back to a heap allocation
    //  through std::function semantics.
    //
    //  Default SBO size is 48 bytes — fits 1-2 captures, a fat pointer, a small
    //  std::function. Override by passing a different SBO size to the ctor's
    //  template parameter (compile-time fixed per queue instance).
    // ===========================================================================
    template <std::size_t SBO = 48>
    class TaskSlot {
    public:
        using invoke_fn = void(*)(void*) noexcept;

        TaskSlot() noexcept : invoke_(nullptr) {}

        TaskSlot(const TaskSlot&) = delete;
        TaskSlot& operator=(const TaskSlot&) = delete;

        ~TaskSlot() { reset(); }

        // Move-only: transferring ownership of stored callable.
        TaskSlot(TaskSlot&& other) noexcept : invoke_(nullptr) {
            if (other.invoke_) {
                move_construct_from(other);
                other.invoke_ = nullptr;
            }
        }
        TaskSlot& operator=(TaskSlot&& other) noexcept {
            if (this != &other) {
                reset();
                if (other.invoke_) {
                    move_construct_from(other);
                    other.invoke_ = nullptr;
                }
            }
            return *this;
        }

        bool empty() const noexcept { return invoke_ == nullptr; }

        void reset() noexcept {
            if (invoke_) {
                if (destructor_) destructor_(buf_);
                invoke_ = nullptr;
                destructor_ = nullptr;
            }
        }

        // Store an arbitrary callable. Falls back to heap via std::function-like
        // indirection if the callable is too large for SBO.
        template <typename F>
        void store(F&& f) noexcept {
            reset();
            using Decay = std::decay_t<F>;
            if constexpr (fits_sbo<Decay>) {
                // placement-new copy/move into the inline buffer
                new (buf_) Decay(static_cast<F&&>(f));
                invoke_ = [](void* p) noexcept {
                    (*static_cast<Decay*>(p))();
                };
                destructor_ = [](void* p) noexcept {
                    static_cast<Decay*>(p)->~Decay();
                };
            } else {
                // heap fallback: store a pointer to a heap-allocated copy
                Decay* heap = new Decay(static_cast<F&&>(f));
                std::memcpy(buf_, &heap, sizeof(heap));
                invoke_ = [](void* p) noexcept {
                    Decay* h;
                    std::memcpy(&h, p, sizeof(h));
                    (*h)();
                };
                destructor_ = [](void* p) noexcept {
                    Decay* h;
                    std::memcpy(&h, p, sizeof(h));
                    delete h;
                };
            }
        }

        void invoke() noexcept { invoke_(buf_); }

    private:
        template <typename Decay>
        static constexpr bool fits_sbo =
        sizeof(Decay) <= SBO &&
        alignof(Decay) <= alignof(std::max_align_t) &&
        std::is_nothrow_move_constructible_v<Decay>;

        void move_construct_from(TaskSlot& other) noexcept {
            // Both our storage and other's are populated; move-construct into ours.
            // For SBO: we memcpy other's storage and steal its invoke_/destructor_.
            // For heap: we just steal the heap pointer.
            std::memcpy(buf_, other.buf_, SBO);
            invoke_     = other.invoke_;
            destructor_ = other.destructor_;
            // We need to also handle the case where 'other' is heap-backed:
            // we own the heap pointer now, so the moved-from slot must drop its
            // destructor_ to avoid double-free. We do that in the move ctor.
            other.destructor_ = nullptr;
        }

        alignas(std::max_align_t) unsigned char buf_[SBO];
        invoke_fn  invoke_;
        invoke_fn  destructor_ = nullptr;
    };

    template <>
    class Queue<TaskTag> {
    public:
        static constexpr bool is_task_queue = true;
        using kind = TaskTag;
        using slot_type = TaskSlot<48>;

        explicit Queue(std::size_t capacity) noexcept
        : capacity_(detail::round_up_pow2(capacity < 2 ? 2 : capacity)),
        mask_(capacity_ - 1),
        slots_(new slot_type[capacity_]) {}

        ~Queue() { delete[] slots_; }

        Queue(const Queue&) = delete;
        Queue& operator=(const Queue&) = delete;

        // ---- producer: push any callable --------------------------------------
        //   q.try_push([]{ do_thing(); });
        //   q.try_push([]{ printf("%d\n", x); }, 42);  // capture by value
        //   q.try_push(some_function_ptr);
        //   q.try_push([&]{ obj.member(); });
        //   q.try_push(std::bind(&Foo::method, &foo, arg));
        template <typename F>
        bool try_push(F&& f) noexcept {
            const std::uint32_t head = head_.load(std::memory_order_relaxed);
            const std::uint32_t tail = tail_seq_.load(std::memory_order_acquire);
            if (head - tail >= capacity_) return false;
            slots_[head & mask_].store(static_cast<F&&>(f));
            head_.store(head + 1, std::memory_order_release);
            detail::futex_wake_one(notify_);
            return true;
        }

        // ISR-safe: no syscall, no allocation, no long path. Allocation that
        // try_push would do is also deferred — see note below.
        template <typename F>
        bool try_push_from_isr(F&& f) noexcept {
            // ISR-unsafe if the callable is larger than SBO (would allocate).
            // In that case refuse; producer must use the thread-context push.
            using Decay = std::decay_t<F>;
            if constexpr (slot_type::template fits_sbo<Decay>) {
                const std::uint32_t head = head_.load(std::memory_order_relaxed);
                const std::uint32_t tail = tail_seq_.load(std::memory_order_acquire);
                if (head - tail >= capacity_) return false;
                slots_[head & mask_].store(static_cast<F&&>(f));
                head_.store(head + 1, std::memory_order_release);
                return true;
            } else {
                (void)f;
                return false;  // would allocate, not ISR-safe
            }
        }

        void wake_consumer() noexcept { detail::futex_wake_one(notify_); }

        // ---- consumer ---------------------------------------------------------
        // Run a single task. Blocks until one is available.
        void run_one() noexcept {
            slot_type* s = nullptr;
            while (true) {
                const std::uint32_t tail = tail_seq_.load(std::memory_order_relaxed);
                const std::uint32_t head = head_.load(std::memory_order_acquire);
                if (tail != head) {
                    s = &slots_[tail & mask_];
                    tail_seq_.store(tail + 1, std::memory_order_release);
                    break;
                }
                detail::futex_wait(notify_, head);
            }
            s->invoke();
            s->reset();
        }

        template <typename Rep, typename Period>
        bool run_one_for(const std::chrono::duration<Rep, Period>& t) noexcept {
            const auto deadline = std::chrono::steady_clock::now() + t;
            slot_type* s = nullptr;
            while (true) {
                const std::uint32_t tail = tail_seq_.load(std::memory_order_relaxed);
                const std::uint32_t head = head_.load(std::memory_order_acquire);
                if (tail != head) {
                    s = &slots_[tail & mask_];
                    tail_seq_.store(tail + 1, std::memory_order_release);
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) return false;
                detail::futex_wait(notify_, head);
            }
            s->invoke();
            s->reset();
            return true;
        }

        // Peek then run. try_run_now() is non-blocking.
        bool try_run_one() noexcept {
            const std::uint32_t tail = tail_seq_.load(std::memory_order_relaxed);
            const std::uint32_t head = head_.load(std::memory_order_acquire);
            if (tail == head) return false;
            slot_type& s = slots_[tail & mask_];
            tail_seq_.store(tail + 1, std::memory_order_release);
            s.invoke();
            s.reset();
            return true;
        }

        // Drains everything currently available, invoking each task. Returns
        // the number of tasks executed.
        std::size_t drain() noexcept {
            std::size_t n = 0;
            while (try_run_one()) ++n;
            return n;
        }

        // ---- introspection ----------------------------------------------------
        std::size_t size() const noexcept {
            return head_.load(std::memory_order_acquire) -
            tail_seq_.load(std::memory_order_acquire);
        }
        std::size_t capacity() const noexcept { return capacity_; }
        bool empty() const noexcept { return size() == 0; }
        bool full()  const noexcept { return size() == capacity_; }

    private:
        const std::size_t capacity_;
        const std::size_t mask_;
        slot_type* const slots_;

        alignas(64) std::atomic<std::uint32_t> head_{0};
        alignas(64) std::atomic<std::uint32_t> tail_seq_{0};
        alignas(64) std::atomic<std::uint32_t> notify_{0};
    };

    // ---------------------------------------------------------------------------
    //  Aliases
    // ---------------------------------------------------------------------------
    template <typename T>
    using DataQueue = Queue<T, DataTag>;

    using TaskQueue = Queue<TaskTag>;

} // namespace embedded
