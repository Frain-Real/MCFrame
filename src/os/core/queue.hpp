// queue.hpp
#pragma once

#include "types.hpp"

namespace embed {

    /// Узел intrusive-очереди. Наследуйся от него (или встрой полем).
    template <typename T>
    class queue_node {
        template <typename U> friend class queue;

        queue_node* next_ = nullptr;

    public:
        queue_node() noexcept = default;
        queue_node(const queue_node&)            = delete;
        queue_node& operator=(const queue_node&) = delete;

        bool in_queue() const noexcept { return next_ != nullptr; }
    };

    /// Intrusive FIFO-очередь. Не владеет элементами.
    template <typename T>
    class queue {
        queue_node<T>* head_ = nullptr;
        queue_node<T>* tail_ = nullptr;
        uint32_t       count_ = 0;

        static queue_node<T>* node_of(T* t) noexcept {
            return static_cast<queue_node<T>*>(t);
        }
        static const queue_node<T>* node_of(const T* t) noexcept {
            return static_cast<const queue_node<T>*>(t);
        }

    public:
        using value_type = T;
        using size_type  = uint32_t;

        queue() noexcept = default;
        queue(const queue&)            = delete;
        queue& operator=(const queue&) = delete;

        // ──────────── Capacity ────────────

        bool        empty() const noexcept { return head_ == nullptr; }
        size_type   size()  const noexcept { return count_; }

        // ──────────── Mutators ────────────

        /// Положить в хвост. O(1).
        void push(T* item) noexcept {
            auto* n = node_of(item);
            n->next_ = nullptr;
            if (tail_) tail_->next_ = n;
            else       head_        = n;
            tail_ = n;
            ++count_;
        }

        /// Забрать из головы. O(1). Возвращает nullptr если пусто.
        T* pop() noexcept {
            if (!head_) return nullptr;
            T* item = static_cast<T*>(head_);
            head_   = head_->next_;
            if (!head_) tail_ = nullptr;
            node_of(item)->next_ = nullptr;
            --count_;
            return item;
        }

        /// Убрать все элементы, не удаляя их. O(n).
        void clear() noexcept {
            for (auto* n = head_; n != nullptr; ) {
                auto* next = n->next_;
                n->next_ = nullptr;
                n = next;
            }
            head_  = nullptr;
            tail_  = nullptr;
            count_ = 0;
        }

        // ──────────── Accessors ────────────

        T*       front()       noexcept { return static_cast<T*>(head_); }
        const T* front() const noexcept { return static_cast<T*>(head_); }
        T*       back()        noexcept { return static_cast<T*>(tail_); }
        const T* back()  const noexcept { return static_cast<T*>(tail_); }

        // ──────────── Iterator ────────────

        class iterator {
            queue_node<T>* cur_;
        public:
            explicit iterator(queue_node<T>* n) noexcept : cur_(n) {}
            T& operator*()  const noexcept { return *static_cast<T*>(cur_); }
            T* operator->() const noexcept { return  static_cast<T*>(cur_); }

            iterator& operator++() noexcept { cur_ = cur_->next_; return *this; }
            iterator  operator++(int) noexcept { auto t = *this; ++(*this); return t; }

            bool operator==(const iterator& o) const noexcept { return cur_ == o.cur_; }
            bool operator!=(const iterator& o) const noexcept { return cur_ != o.cur_; }
        };

        iterator begin() noexcept { return iterator(head_); }
        iterator end()   noexcept { return iterator(nullptr); }
    };

} // namespace embed

namespace embed {

    /// Очередь выполнения функций. Фиксированная ёмкость, без аллокаций.
    template <uint32_t N>
    class task_queue {
        static_assert(N >= 2,        "task_queue: N must be >= 2");
        static_assert((N & (N-1)) == 0, "task_queue: N must be a power of two");

    public:
        using size_type = uint32_t;
        using task_fn   = void (*)(void* ctx);

        static constexpr size_type capacity() noexcept { return N - 1; }

    private:
        struct task {
            task_fn fn  = nullptr;
            void*   ctx = nullptr;
        };

        static constexpr size_type kMask = N - 1;

        task      buf_[N]{};
        size_type head_  = 0;
        size_type tail_  = 0;
        size_type count_ = 0;

    public:
        task_queue() noexcept = default;
        task_queue(const task_queue&)            = delete;
        task_queue& operator=(const task_queue&) = delete;

        // ──────────── Capacity ────────────

        bool      empty() const noexcept { return count_ == 0; }
        bool      full()  const noexcept { return count_ == capacity(); }
        size_type size()  const noexcept { return count_; }

        // ──────────── Mutators ────────────

        /// Добавить задачу. Возвращает false если очередь полна. O(1).
        bool post(task_fn fn, void* ctx = nullptr) noexcept {
            if (full()) return false;
            buf_[head_] = {fn, ctx};
            head_ = (head_ + 1) & kMask;
            ++count_;
            return true;
        }

        /// Выполнить одну задачу. Возвращает false если очередь пуста. O(1).
        bool run_one() noexcept {
            if (empty()) return false;
            task& t = buf_[tail_];
            tail_ = (tail_ + 1) & kMask;
            --count_;
            if (t.fn) t.fn(t.ctx);
            return true;
        }

        /// Выполнить все задачи. O(n).
        void run_all() noexcept {
            while (run_one()) {}
        }

        /// Очистить очередь, не выполняя задачи. O(1).
        void clear() noexcept {
            head_  = 0;
            tail_  = 0;
            count_ = 0;
        }
    };

} // namespace embed