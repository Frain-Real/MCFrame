// semaphore.hpp
#pragma once
#include <cstdint>
#include "types.hpp"
#include "../../common.hpp"
#include "mutex.hpp"     // sched_api

namespace embed {

    enum class sem_type {
        binary,
        counting
    };

    /// Семафор.
    ///
    /// Прежняя реализация ждала в busy-spin (`while (count == 0) continue;`),
    /// из-за чего ожидающая задача выедала квант CPU и не могла быть
    /// вытеснена, а при вызове из задачи с высоким приоритетом приводила
    /// к срабатыванию watchdog.
    ///
    /// Теперь ожидание уходит в планировщик: задача блокируется и
    /// пробуждается сигналом. Есть отдельный signal_from_isr(), который
    /// не берёт критическую секцию, потому что мы уже в прерывании.
    template <sem_type Type, uint32_t MaxCount = 1>
    class semaphore {
        static_assert(MaxCount >= 1u, "semaphore: MaxCount must be >= 1");

        /// Счётчик — atomic_t, а не обычный uint32_t:
        ///  * signal_from_isr() намеренно работает БЕЗ критической секции
        ///    (прерывания уже запрещены, а Lock включил бы их при выходе),
        ///    поэтому его изменение обязано быть атомарным;
        ///  * value() читает счётчик, не заходя в критическую секцию.
        ///
        /// Внутри Lock порядок доступа к памяти ослаблен до Relaxed: доступ
        /// к счётчику там ВЗАИМОИСКЛЮЧЁН (прерывание не может вклиниться
        /// между чтением и записью), а вызов platform::ops() на входе и
        /// выходе из секции — барьер для компилятора. Порядок Acquire/Release
        /// нужен только на путях БЕЗ Lock: signal_from_isr() и value().
        atomic_t<uint32_t> count_{0u};

        static constexpr bool is_binary() noexcept { return Type == sem_type::binary; }

        /// Забрать ресурс: 1 -> 0 для бинарного, count-1 для счётного.
        /// Должно вызываться ТОЛЬКО под Lock.
        void take_locked() noexcept {
            if (is_binary()) count_.store(0u, MemoryOrderRelaxed);
            else             (void)count_.fetch_sub(1u, MemoryOrderRelaxed);
        }

        /// Сколько ресурсов свободно. Вызывать под Lock.
        uint32_t count_locked() const noexcept {
            return count_.load(MemoryOrderRelaxed);
        }

    public:
        semaphore() noexcept = default;
        semaphore(const semaphore&)            = delete;
        semaphore& operator=(const semaphore&) = delete;

        /// Опустить семафор. ticks == 0 — без таймаута.
        Status wait(uint32_t ticks = 0) noexcept {
            for (;;) {
                {
                    Lock l;
                    if (count_locked() > 0u) {
                        take_locked();
                        return Status::Ok;
                    }
                }
                // Ресурса нет — блокируемся, НЕ крутимся.
                const Status st = sched_api::block_on(this, ticks);
                if (st != Status::Ok) return st;
            }
        }

        /// Попробовать без ожидания.
        bool try_wait() noexcept {
            Lock l;
            if (count_locked() == 0u) return false;
            take_locked();
            return true;
        }

        /// Сигнал из контекста задачи.
        void signal() noexcept {
            bool wake = false;
            {
                Lock l;
                if (is_binary()) {
                    // Бинарный семафор хранит только «есть событие»;
                    // счётчик намеренно не растёт выше 1.
                    count_.store(1u, MemoryOrderRelaxed);
                    wake = true;
                } else if (count_locked() < MaxCount) {
                    (void)count_.fetch_add(1u, MemoryOrderRelaxed);
                    wake = true;
                }
            }
            if (wake) (void)sched_api::unblock_one(this);
        }

        /// Сигнал из прерывания. Критическую секцию НЕ берём:
        /// прерывания уже запрещены, а Lock включил бы их при выходе.
        ///
        /// Здесь атомарность обязательна по-настоящему: задача может
        /// находиться в wait()/value() в момент прерывания. Порядок —
        /// Release: счётчик публикуется, а ожидающий его читает с Acquire не
        /// раньше, чем планировщик разбудит задачу.
        void signal_from_isr() noexcept {
            if (is_binary()) {
                count_.store(1u, MemoryOrderRelease);
            } else {
                // CAS-цикл вместо «проверить и увеличить»: иначе два
                // вложенных прерывания могут поднять счётчик выше MaxCount,
                // и семафор «подарит» лишний ресурс.
                uint32_t cur = count_.load(MemoryOrderRelaxed);
                while (cur < MaxCount &&
                       !count_.compare_exchange_weak(cur, cur + 1u,
                                                     MemoryOrderRelease,
                                                     MemoryOrderRelaxed)) {
                    // cur обновлён фактическим значением — проверяем снова.
                }
            }
            (void)sched_api::unblock_one(this);
        }

        /// Текущее значение (для отладки).
        /// Acquire: счётчик читается вне критической секции, в том числе
        /// параллельно с signal_from_isr().
        uint32_t value() const noexcept {
            return count_.load(MemoryOrderAcquire);
        }
    };

}
