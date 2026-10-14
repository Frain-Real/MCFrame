// types.hpp
// ---------------------------------------------------------------------------
// Базовые типы примитивов синхронизации.
//
// atomic_t — ЕДИНСТВЕННЫЙ способ работы с атомарными переменными в MCFrame.
//
// ПОЧЕМУ НЕ std::atomic
//   * <atomic> тянет libstdc++ и рассчитан на hosted-сборку; прошивка
//     собирается с -fno-exceptions -fno-rtti и без динамической инициализации;
//   * для 64-битных типов std::atomic на 32-битном Cortex-M3 требует
//     libatomic, которого в линковке прошивки нет;
//   * поведение обязано быть ОДИНАКОВЫМ в mock-сборке (ПК) и на железе.
//
// ПОЧЕМУ НЕ «ГОЛЫЕ» __atomic_*-ВСТРОЙКИ
//   * вызов __atomic_load_n(&field, __ATOMIC_ACQUIRE) можно написать для
//     любого поля и с любым порядком — компилятор не проверит ни то, что это
//     то же поле, что и в записи, ни то, что порядок вообще осмысленный;
//   * при копировании строки кода порядок теряется, и гонка возвращается;
//   * ошибку видно только на железе и только иногда.
//   atomic_t делает атомарный доступ явным (load/store/exchange/
//   compare_exchange_*/fetch_*) и задаёт порядок по умолчанию.
//
// ВАЖНО: volatile НЕ даёт атомарности. Он лишь запрещает компилятору держать
// значение в регистре. Атомарность обеспечивают встроенные функции __atomic_*.
// ---------------------------------------------------------------------------
#pragma once

namespace embed {

    /// Порядок доступа к памяти. Значения совпадают с __ATOMIC_* из GCC/Clang,
    /// поэтому преобразование в int не требует ни таблиц, ни ветвлений.
    enum MemoryOrder {
        MemoryOrderRelaxed = __ATOMIC_RELAXED,
        MemoryOrderAcquire = __ATOMIC_ACQUIRE,
        MemoryOrderRelease = __ATOMIC_RELEASE,
        MemoryOrderAcqRel  = __ATOMIC_ACQ_REL,
        MemoryOrderSeqCst  = __ATOMIC_SEQ_CST
    };

    /// Атомарная переменная.
    ///
    /// По умолчанию порядок — MemoryOrderSeqCst: «самый безопасный, пока не
    /// доказано обратное». Ослаблять порядок (Relaxed) допустимо, только если
    /// доступ уже защищён критической секцией (Lock) или гонки нет по
    /// построению, и это видно из комментария на месте.
    ///
    /// T обязан быть тривиально копируемым. Типы шире машинного слова
    /// (uint64_t на Cortex-M3) не поддерживаются: компилятор превратит такие
    /// операции в вызовы libatomic, которых в прошивке нет — для 64-битных
    /// счётчиков используйте Lock.
    template <typename T>
    class atomic_t {
        volatile T v_{};

        /// Общая часть weak/strong: при неудаче в expected попадает
        /// ФАКТИЧЕСКОЕ значение — это и есть «чтение под CAS».
        bool cas(T& expected, T desired, bool weak,
                 MemoryOrder success, MemoryOrder failure) noexcept {
            T exp = expected;
            const bool ok = __atomic_compare_exchange_n(
                &v_, &exp, desired, weak,
                static_cast<int>(success), static_cast<int>(failure));
            if (!ok) expected = exp;
            return ok;
        }

    public:
        using value_type = T;

        static_assert(__is_trivially_copyable(T),
                      "atomic_t: T обязан быть тривиально копируемым — "
                      "иначе __atomic_* не даст атомарности");
        static_assert(sizeof(T) <= sizeof(void*),
                      "atomic_t: тип шире машинного слова не lock-free — "
                      "используйте Lock");

        // Конструкторы constexpr: глобальный atomic_t (счётчик, флаг) обязан
        // попадать в .bss статически. Иначе на прошивке появляется вызов
        // из .init_array — то есть код, исполняемый ДО main().
        constexpr atomic_t() noexcept = default;
        constexpr explicit atomic_t(T desired) noexcept : v_(desired) {}

        // Копирование атомарной переменной — это гонка «прочитал-записал».
        // Как и std::atomic, запрещаем: копируйте значение через load().
        atomic_t(const atomic_t&)            = delete;
        atomic_t& operator=(const atomic_t&) = delete;

        /// Запись значения оператором: `flag = true;` — как у std::atomic.
        /// Порядок SeqCst, потому что «тихий» оператор не должен незаметно
        /// ослаблять синхронизацию; нужен другой — store(..., order).
        T operator=(T desired) noexcept {
            store(desired, MemoryOrderSeqCst);
            return desired;
        }

        // ───────────────────────── Чтение и запись ─────────────────────────

        T load(MemoryOrder mo = MemoryOrderSeqCst) const noexcept {
            return __atomic_load_n(&v_, static_cast<int>(mo));
        }

        void store(T desired, MemoryOrder mo = MemoryOrderSeqCst) noexcept {
            __atomic_store_n(&v_, desired, static_cast<int>(mo));
        }

        /// Записать новое значение и вернуть прежнее.
        T exchange(T desired, MemoryOrder mo = MemoryOrderSeqCst) noexcept {
            return __atomic_exchange_n(&v_, desired, static_cast<int>(mo));
        }

        // ─────────────────────── Сравнение с обменом ───────────────────────

        /// Слабая CAS. На LL/SC-архитектурах (в том числе ARMv7-M:
        /// ldrex/strex в цикле) она вправе вернуть false, хотя значение
        /// не менялось, — «не повезло». Поэтому weak вызывают ТОЛЬКО в цикле.
        /// Там, где цикла нет (одноразовая защёлка), нужна strong: иначе
        /// операция молча теряется.
        bool compare_exchange_weak(T& expected, T desired,
                                   MemoryOrder success = MemoryOrderSeqCst,
                                   MemoryOrder failure = MemoryOrderSeqCst) noexcept {
            return cas(expected, desired, true, success, failure);
        }

        /// Сильная CAS: возвращает false ТОЛЬКО если значение не совпало.
        /// Эквивалент __atomic_compare_exchange_n(..., weak = false).
        bool compare_exchange_strong(T& expected, T desired,
                                     MemoryOrder success = MemoryOrderSeqCst,
                                     MemoryOrder failure = MemoryOrderSeqCst) noexcept {
            return cas(expected, desired, false, success, failure);
        }

        // ─────────────────────── Чтение с изменением ───────────────────────

        /// Атомарное «прочитать, прибавить, записать». Возвращает ПРЕЖНЕЕ
        /// значение (как std::atomic::fetch_add).
        T fetch_add(T arg, MemoryOrder mo = MemoryOrderSeqCst) noexcept {
            return __atomic_fetch_add(&v_, arg, static_cast<int>(mo));
        }

        T fetch_sub(T arg, MemoryOrder mo = MemoryOrderSeqCst) noexcept {
            return __atomic_fetch_sub(&v_, arg, static_cast<int>(mo));
        }

        T fetch_and(T arg, MemoryOrder mo = MemoryOrderSeqCst) noexcept {
            return __atomic_fetch_and(&v_, arg, static_cast<int>(mo));
        }

        T fetch_or(T arg, MemoryOrder mo = MemoryOrderSeqCst) noexcept {
            return __atomic_fetch_or(&v_, arg, static_cast<int>(mo));
        }

        T fetch_xor(T arg, MemoryOrder mo = MemoryOrderSeqCst) noexcept {
            return __atomic_fetch_xor(&v_, arg, static_cast<int>(mo));
        }

        // ─────────────── Операторы (для счётчиков и флагов) ────────────────
        //
        // Порядок у операторов — MemoryOrderSeqCst: «тихая» перегрузка
        // оператора не должна незаметно ослаблять синхронизацию. Нужен
        // другой порядок — вызывайте fetch_add()/fetch_sub() явно.

        T operator++()    noexcept { return fetch_add(T(1)) + T(1); }  // новое значение
        T operator++(int) noexcept { return fetch_add(T(1)); }         // прежнее значение
        T operator--()    noexcept { return fetch_sub(T(1)) - T(1); }
        T operator--(int) noexcept { return fetch_sub(T(1)); }

        T operator+=(T arg) noexcept { return fetch_add(arg) + arg; }
        T operator-=(T arg) noexcept { return fetch_sub(arg) - arg; }
        T operator&=(T arg) noexcept { return fetch_and(arg) & arg; }
        T operator|=(T arg) noexcept { return fetch_or(arg)  | arg; }
        T operator^=(T arg) noexcept { return fetch_xor(arg) ^ arg; }
    };

    /// Атомарный флаг: имена короче, чем atomic_t<bool>.
    using atomic_bool = atomic_t<bool>;

}
