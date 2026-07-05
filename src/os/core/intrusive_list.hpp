// intrusive_list.hpp
#pragma once

#include <cstddef>

namespace embed {

    /**
     * Intrusive singly-linked list.
     *
     * Особенности:
     *  - T должен наследовать embed::list_node<T> (или содержать его как поле).
     *  - Узлы сами знают про свои соседи → не аллоцирует, не освобождает память.
     *  - O(1) push_front / pop_front / remove.
     *  - Без блокировок: для thread-safety используй встроенные atomic-операции
     *    на list_node::next_ снаружи класса, либо оборачивай в Spinlock / disable_irq.
     *  - Владение: узлы не владеют payload'ом — payload сам управляет своим lifecycle.
     *
     * Использование:
     *   struct Task : embed::list_node<Task> {
     *       int priority;
     *       void run();
     *   };
     *
     *   embed::intrusive_list<Task> ready_queue;
     *
     *   Task t1; t1.priority = 1;
     *   Task t2; t2.priority = 2;
     *   ready_queue.push_front(&t2);
     *   ready_queue.push_front(&t1);
     *   for (auto* t : ready_queue) t->run();
     */
    template <typename T>
    class list_node {
        template <typename U> friend class intrusive_list;

        list_node* next_ = nullptr;
        list_node* prev_ = nullptr;  // для двусвязного — см. вариант ниже

    public:
        list_node() noexcept = default;
        list_node(const list_node&)            = delete;
        list_node& operator=(const list_node&) = delete;

        bool in_list() const noexcept { return next_ != nullptr; }
    };

    template <typename T>
    class intrusive_list {
        list_node<T>* head_ = nullptr;
        list_node<T>* tail_ = nullptr;
        std::size_t   count_ = 0;

        static list_node<T>* node_of(T* t) noexcept {
            return static_cast<list_node<T>*>(t);
        }
        static const list_node<T>* node_of(const T* t) noexcept {
            return static_cast<const list_node<T>*>(t);
        }

    public:
        using value_type = T;
        using size_type  = std::size_t;

        intrusive_list() noexcept = default;
        intrusive_list(const intrusive_list&)            = delete;
        intrusive_list& operator=(const intrusive_list&) = delete;

        // ──────────── Mutators ────────────

        bool empty() const noexcept { return head_ == nullptr; }
        std::size_t size() const noexcept { return count_; }

        /// Добавить в голову. O(1).
        void push_front(T* item) noexcept {
            auto* n = node_of(item);
            n->prev_ = nullptr;
            n->next_ = head_;
            if (head_) head_->prev_ = n;
            else       tail_       = n;
            head_ = n;
            ++count_;
        }

        /// Добавить в хвост. O(1).
        void push_back(T* item) noexcept {
            auto* n = node_of(item);
            n->next_ = nullptr;
            n->prev_ = tail_;
            if (tail_) tail_->next_ = n;
            else       head_       = n;
            tail_ = n;
            ++count_;
        }

        /// Убрать из головы. Возвращает nullptr если пусто.
        T* pop_front() noexcept {
            if (!head_) return nullptr;
            T* item = static_cast<T*>(head_);
            head_   = head_->next_;
            if (head_) head_->prev_ = nullptr;
            else       tail_       = nullptr;
            node_of(item)->next_ = nullptr;
            node_of(item)->prev_ = nullptr;
            --count_;
            return item;
        }

        /// Убрать конкретный узел. O(1). Узел должен быть в списке.
        void remove(T* item) noexcept {
            auto* n = node_of(item);
            if (n->prev_) n->prev_->next_ = n->next_;
            else          head_           = n->next_;
            if (n->next_) n->next_->prev_ = n->prev_;
            else          tail_           = n->prev_;
            n->next_ = nullptr;
            n->prev_ = nullptr;
            --count_;
        }

        /// Убрать все узлы, не удаляя сами объекты.
        void clear() noexcept {
            for (auto* n = head_; n != nullptr; ) {
                auto* next = n->next_;
                n->next_ = nullptr;
                n->prev_ = nullptr;
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

        bool contains(const T* item) const noexcept {
            for (auto* n = head_; n != nullptr; n = n->next_) {
                if (n == node_of(item)) return true;
            }
            return false;
        }

        // ──────────── Iterator (для range-based for) ────────────

        class iterator {
            list_node<T>* cur_;
        public:
            explicit iterator(list_node<T>* n) noexcept : cur_(n) {}
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
