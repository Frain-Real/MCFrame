#pragma once
#include "../../common.hpp"


namespace embed {

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
        using Size  = uint32_t;

        queue() noexcept = default;
        queue(const queue&)            = delete;
        queue& operator=(const queue&) = delete;

        // Capacity
        bool        empty() const noexcept { return head_ == nullptr; }
        Size   size()  const noexcept { return count_; }

        // Mutators

        void push(T* item) noexcept {
            auto* n = node_of(item);
            n->next_ = nullptr;
            if (tail_) tail_->next_ = n;
            else       head_        = n;
            tail_ = n;
            ++count_;
        }

        T* pop() noexcept {
            if (!head_) return nullptr;
            T* item = static_cast<T*>(head_);
            head_   = head_->next_;
            if (!head_) tail_ = nullptr;
            node_of(item)->next_ = nullptr;
            --count_;
            return item;
        }

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

        // Accessors
        T*       front()       noexcept { return static_cast<T*>(head_); }
        const T* front() const noexcept { return static_cast<T*>(head_); }
        T*       back()        noexcept { return static_cast<T*>(tail_); }
        const T* back()  const noexcept { return static_cast<T*>(tail_); }

        // Iterator
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

}

namespace embed {
    template <uint32_t N>
    class task_queue {
        static_assert(N >= 2,        "task_queue: N must be >= 2");
        static_assert((N & (N-1)) == 0, "task_queue: N must be a power of two");

    public:
        using task_fn   = void (*)(void* ctx);

        static constexpr Size capacity() noexcept { return N - 1; }

    private:
        struct task {
            task_fn fn  = nullptr;
            void*   ctx = nullptr;
        };

        static constexpr Size kMask = N - 1;

        task      buf_[N]{};
        Size head_  = 0;
        Size tail_  = 0;
        Size count_ = 0;

    public:
        task_queue() noexcept = default;
        task_queue(const task_queue&)            = delete;
        task_queue& operator=(const task_queue&) = delete;

        // Capacity

        bool      empty() const noexcept { return count_ == 0; }
        bool      full()  const noexcept { return count_ == capacity(); }
        Size size()  const noexcept { return count_; }

        // Mutators
        bool post(task_fn fn, void* ctx = nullptr) noexcept {
            if (full()) return false;
            buf_[head_] = {fn, ctx};
            head_ = (head_ + 1) & kMask;
            ++count_;
            return true;
        }

        bool run_one() noexcept {
            if (empty()) return false;
            task& t = buf_[tail_];
            tail_ = (tail_ + 1) & kMask;
            --count_;
            if (t.fn) t.fn(t.ctx);
            return true;
        }

        void run_all() noexcept {
            while (run_one()) {}
        }

        void clear() noexcept {
            head_  = 0;
            tail_  = 0;
            count_ = 0;
        }
    };

}