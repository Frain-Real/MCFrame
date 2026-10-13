// intrusive_list.hpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace embed {

    /// Узел двусвязного интрузивного списка.
    ///
    /// ВАЖНО: принадлежность списку хранится ЯВНО (owner_), а не выводится
    /// из next_ != nullptr: прежний in_list() возвращал false для последнего
    /// элемента (у него next_ == nullptr), поэтому проверки «уже в списке»
    /// и «двойное удаление» не работали.
    template <typename T>
    class list_node {
        template <typename U, bool Embedded> friend class intrusive_list;
        list_node*  next_  = nullptr;
        list_node*  prev_  = nullptr;
        const void* owner_ = nullptr;

    public:
        list_node() noexcept = default;
        list_node(const list_node&)            = delete;
        list_node& operator=(const list_node&) = delete;

        bool in_list() const noexcept { return owner_ != nullptr; }
    };

    /// Преобразование элемент <-> узел.
    ///
    /// Embedded == true  — T наследует list_node<T> ПЕРВЫМ базовым классом,
    ///                     static_cast между указателями бесплатен.
    /// Embedded == false — узел лежит по смещению 0 внутри T (первый член),
    ///                     преобразование также бесплатно.
    ///
    /// Выбор делается ПАРАМЕТРОМ ШАБЛОНА, а не `if constexpr` по указателю
    /// на функцию: такой указатель не является константным выражением,
    /// поэтому обе ветки всё равно разбираются компилятором.
    template <typename T, bool Embedded>
    struct node_accessor {
        static list_node<T>* node(T* t) noexcept {
            return static_cast<list_node<T>*>(t);
        }
        static T* item(list_node<T>* n) noexcept {
            return static_cast<T*>(n);
        }
    };

    template <typename T>
    struct node_accessor<T, false> {
        static list_node<T>* node(T* t) noexcept {
            return reinterpret_cast<list_node<T>*>(t);   // узел — первый член
        }
        static T* item(list_node<T>* n) noexcept {
            return reinterpret_cast<T*>(n);
        }
    };

    /// Двусвязный интрузивный список.
    ///
    /// ИНВАРИАНТ: один узел находится не более чем в одном списке. Именно
    /// поэтому у задачи ОТДЕЛЬНЫЕ узлы для реестра задач и для очередей
    /// планировщика: прежняя версия имела один узел на объект, и вторая
    /// вставка молча отклонялась — add_task() не работал вовсе.
    ///
    /// Инварианты списка:
    ///  * remove() для элемента из чужого списка — no-op (возвращает false);
    ///  * повторная вставка того же узла игнорируется.
    template <typename T, bool Embedded = true>
    class intrusive_list {
        using acc = node_accessor<T, Embedded>;

        list_node<T>* head_  = nullptr;
        list_node<T>* tail_  = nullptr;
        uint32_t      count_ = 0;

        static list_node<T>* node_of(T* t) noexcept {
            return t ? acc::node(t) : nullptr;
        }
        static list_node<T>* node_of(const T* t) noexcept {
            return node_of(const_cast<T*>(t));
        }
        static T* item_of(list_node<T>* n) noexcept {
            return n ? acc::item(n) : nullptr;
        }

    public:
        intrusive_list() noexcept = default;
        intrusive_list(const intrusive_list&)            = delete;
        intrusive_list& operator=(const intrusive_list&) = delete;

        // ── Состояние ──
        bool     empty() const noexcept { return head_ == nullptr; }
        uint32_t size()  const noexcept { return count_; }

        // ── Модификация ──
        bool push_front(T* item) noexcept {
            auto* n = node_of(item);
            if (n == nullptr || n->owner_ != nullptr) return false;
            n->prev_  = nullptr;
            n->next_  = head_;
            if (head_) head_->prev_ = n;
            else       tail_        = n;
            head_     = n;
            n->owner_ = this;
            ++count_;
            return true;
        }

        bool push_back(T* item) noexcept {
            auto* n = node_of(item);
            if (n == nullptr || n->owner_ != nullptr) return false;
            n->next_  = nullptr;
            n->prev_  = tail_;
            if (tail_) tail_->next_ = n;
            else       head_        = n;
            tail_     = n;
            n->owner_ = this;
            ++count_;
            return true;
        }

        T* pop_front() noexcept {
            if (!head_) return nullptr;
            list_node<T>* n = head_;
            T* item = item_of(n);
            head_   = n->next_;
            if (head_) head_->prev_ = nullptr;
            else       tail_        = nullptr;
            n->next_  = nullptr;
            n->prev_  = nullptr;
            n->owner_ = nullptr;
            --count_;
            return item;
        }

        /// false, если узел не принадлежит этому списку (прежде это
        /// разрушало счётчик и указатели списка).
        bool remove(T* item) noexcept {
            auto* n = node_of(item);
            if (n == nullptr || n->owner_ != this) return false;

            if (n->prev_) n->prev_->next_ = n->next_;
            else          head_           = n->next_;
            if (n->next_) n->next_->prev_ = n->prev_;
            else          tail_           = n->prev_;

            n->next_  = nullptr;
            n->prev_  = nullptr;
            n->owner_ = nullptr;
            --count_;
            return true;
        }

        bool contains(const T* item) const noexcept {
            const list_node<T>* n = node_of(item);
            return n != nullptr && n->owner_ == this;
        }

        void clear() noexcept {
            for (auto* n = head_; n != nullptr; ) {
                auto* next = n->next_;
                n->next_  = nullptr;
                n->prev_  = nullptr;
                n->owner_ = nullptr;
                n = next;
            }
            head_  = nullptr;
            tail_  = nullptr;
            count_ = 0;
        }

        // ── Доступ ──
        T* front() noexcept { return item_of(head_); }
        const T* front() const noexcept { return const_cast<intrusive_list*>(this)->item_of(head_); }
        T* back()  noexcept { return item_of(tail_); }
        const T* back() const noexcept { return const_cast<intrusive_list*>(this)->item_of(tail_); }

        // ── Итераторы ──
        class iterator {
            list_node<T>* cur_;
        public:
            explicit iterator(list_node<T>* n) noexcept : cur_(n) {}
            T& operator*()  const noexcept { return *item_of(cur_); }
            T* operator->() const noexcept { return  item_of(cur_); }

            iterator& operator++() noexcept {
                if (cur_) cur_ = cur_->next_;
                return *this;
            }
            iterator operator++(int) noexcept { auto t = *this; ++(*this); return t; }

            bool operator==(const iterator& o) const noexcept { return cur_ == o.cur_; }
            bool operator!=(const iterator& o) const noexcept { return cur_ != o.cur_; }
        };

        /// Итератор, безопасный к удалению текущего элемента.
        class safe_iterator {
            list_node<T>* cur_;
            list_node<T>* nxt_;
        public:
            explicit safe_iterator(list_node<T>* n) noexcept
                : cur_(n), nxt_(n ? n->next_ : nullptr) {}
            T* get() const noexcept { return item_of(cur_); }
            bool done() const noexcept { return cur_ == nullptr; }
            void advance() noexcept { cur_ = nxt_; nxt_ = cur_ ? cur_->next_ : nullptr; }
        };

        safe_iterator safe_begin() const noexcept { return safe_iterator(head_); }

        iterator begin() noexcept { return iterator(head_); }
        iterator end()   noexcept { return iterator(nullptr); }
        iterator begin() const noexcept { return iterator(head_); }
        iterator end()   const noexcept { return iterator(nullptr); }
    };

    /// Реестр объектов фиксированной ёмкости.
    ///
    /// Массив, а не интрузивный список: у задачи уже занят узел очередями
    /// планировщика, и второй узел ради реестра избыточен. Ёмкость мала
    /// (kMaxTasks = 32), поэтому линейный поиск дешевле и безопаснее.
    template <typename T, uint32_t Capacity>
    class object_registry {
        T*       items_[Capacity]{};
        uint32_t count_ = 0;

    public:
        object_registry() noexcept = default;
        object_registry(const object_registry&)            = delete;
        object_registry& operator=(const object_registry&) = delete;

        bool     empty() const noexcept { return count_ == 0u; }
        uint32_t size()  const noexcept { return count_; }
        uint32_t capacity() const noexcept { return Capacity; }

        bool contains(const T* item) const noexcept {
            for (uint32_t i = 0; i < count_; ++i) {
                if (items_[i] == item) return true;
            }
            return false;
        }

        bool add(T* item) noexcept {
            if (item == nullptr || count_ >= Capacity) return false;
            if (contains(item)) return false;
            items_[count_++] = item;
            return true;
        }

        bool remove(T* item) noexcept {
            for (uint32_t i = 0; i < count_; ++i) {
                if (items_[i] != item) continue;
                for (uint32_t j = i; j + 1u < count_; ++j) items_[j] = items_[j + 1u];
                items_[--count_] = nullptr;
                return true;
            }
            return false;
        }

        T* at(uint32_t i) const noexcept { return (i < count_) ? items_[i] : nullptr; }

        void clear() noexcept {
            for (uint32_t i = 0; i < count_; ++i) items_[i] = nullptr;
            count_ = 0;
        }

        /// Обход с явным управлением индексом. При удалении текущего элемента
        /// НЕ вызывайте advance() — следующий элемент встанет на его место.
        class iterator {
            const object_registry* reg_;
            uint32_t               idx_;
        public:
            iterator(const object_registry* r, uint32_t i) noexcept : reg_(r), idx_(i) {}
            T*   get()  const noexcept { return reg_->at(idx_); }
            bool done() const noexcept { return idx_ >= reg_->count_; }
            void advance() noexcept { ++idx_; }
            void skip()    noexcept {}     // элемент удалён — индекс уже верен
        };

        iterator begin() const noexcept { return iterator(this, 0u); }
    };

}
