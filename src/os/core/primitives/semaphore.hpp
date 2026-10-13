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

        uint32_t count_ = 0u;

        static constexpr bool is_binary() noexcept { return Type == sem_type::binary; }

    public:
        semaphore() noexcept = default;
        semaphore(const semaphore&)            = delete;
        semaphore& operator=(const semaphore&) = delete;

        /// Опустить семафор. ticks == 0 — без таймаута.
        Status wait(uint32_t ticks = 0) noexcept {
            for (;;) {
                {
                    Lock l;
                    if (count_ > 0u) {
                        if (is_binary()) count_ = 0u;
                        else             --count_;
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
            if (count_ == 0u) return false;
            if (is_binary()) count_ = 0u;
            else             --count_;
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
                    count_ = 1u;
                    wake = true;
                } else if (count_ < MaxCount) {
                    ++count_;
                    wake = true;
                }
            }
            if (wake) (void)sched_api::unblock_one(this);
        }

        /// Сигнал из прерывания. Критическую секцию НЕ берём:
        /// прерывания уже запрещены, а Lock включил бы их при выходе.
        void signal_from_isr() noexcept {
            if (is_binary()) {
                count_ = 1u;
            } else if (count_ < MaxCount) {
                ++count_;
            }
            (void)sched_api::unblock_one(this);
        }

        /// Текущее значение (для отладки).
        uint32_t value() const noexcept {
            return __atomic_load_n(&count_, __ATOMIC_ACQUIRE);
        }
    };

}
