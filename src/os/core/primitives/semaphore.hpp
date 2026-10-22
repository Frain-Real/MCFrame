// semaphore.hpp
#pragma once
#include <cstdint>
#include "types.hpp"
#include "../../common.hpp"
#include "mutex.hpp"     // sched_api, wait_queue

namespace embed {

    enum class sem_type {
        binary,
        counting
    };

    /// Семафор.
    ///
    /// История: busy-spin (`while (count == 0) continue;`) заменён сном
    /// задачи. Теперь добавлены ещё две вещи:
    ///
    ///  * ОЧЕРЕДЬ ОЖИДАНИЯ: разбуженный выбирается по приоритету, а не
    ///    «первый в реестре задач»; signal() передаёт ресурс напрямую
    ///    ожидающему, а не увеличивает счётчик «в пустоту»;
    ///  * ОДНА КРИТИЧЕСКАЯ СЕКЦИЯ на проверку и сон: между «ресурса нет»
    ///    и постановкой в очередь не может вклиниться signal_from_isr(),
    ///    поэтому пробуждение не теряется (прежний wait() терял его).
    ///
    /// Счётчик остаётся atomic_t: value() читается без Lock, а
    /// signal_from_isr() может выполняться из прерывания.
    template <sem_type Type, uint32_t MaxCount = 1>
    class semaphore {
        static_assert(MaxCount >= 1u, "semaphore: MaxCount must be >= 1");

        atomic_t<uint32_t> count_{0u};
        wait_queue         waiters_;

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

        /// Сколько тиков осталось до абсолютного дедлайна.
        /// 0 — дедлайн уже наступил; при deadline == 0 — «без таймаута».
        static uint32_t remaining_ticks(uint64_t deadline) noexcept {
            if (deadline == 0u) return 0u;
            const uint64_t now = sched_api::now_ticks();
            if (now >= deadline) return 0u;
            const uint64_t left = deadline - now;
            return (left > 0xFFFFFFFFu)
                ? 0xFFFFFFFFu
                : static_cast<uint32_t>(left);
        }

        /// Отдать ресурс ожидающему или положить в счётчик.
        /// Вызывать под Lock: иначе между проверкой очереди и сном задачи
        /// сигнал потеряется.
        void give_locked() noexcept {
            if (!waiters_.empty()) {
                if (sched_api::unblock_one(waiters_)) return;
            }
            if (is_binary()) {
                // Бинарный семафор хранит только «есть событие»;
                // счётчик намеренно не растёт выше 1.
                count_.store(1u, MemoryOrderRelease);
            } else if (count_locked() < MaxCount) {
                (void)count_.fetch_add(1u, MemoryOrderRelease);
            }
        }

    public:
        semaphore() noexcept = default;
        semaphore(const semaphore&)            = delete;
        semaphore& operator=(const semaphore&) = delete;

        /// Опустить семафор. ticks == 0 — без таймаута, иначе АБСОЛЮТНЫЙ
        /// дедлайн (серия пробуждений не продлевает ожидание).
        Status wait(uint32_t ticks = 0) noexcept {
            const uint64_t deadline = (ticks != 0u)
                ? (sched_api::now_ticks() + static_cast<uint64_t>(ticks))
                : 0u;

            for (;;) {
                Lock l;

                TaskHandler self = sched_api::current();
                if (self == nullptr) return Status::Unavailable;

                if (count_locked() > 0u) {
                    take_locked();
                    return Status::Ok;
                }

                if (deadline != 0u) {
                    const uint32_t left = remaining_ticks(deadline);
                    if (left == 0u) return Status::Timeout;
                    const Status st =
                        sched_api::block_on_locked(l, waiters_, this, left);
                    if (st != Status::Ok) return st;
                } else {
                    const Status st =
                        sched_api::block_on_locked(l, waiters_, this, 0u);
                    if (st != Status::Ok) return st;
                }
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
            Lock l;
            give_locked();
        }

        /// Сигнал из прерывания.
        ///
        /// Lock здесь БЕЗОПАСЕН и обязателен: critical_enter() возвращает
        /// предыдущий PRIMASK (в прерывании это 1), а critical_exit()
        /// восстанавливает именно его, а не разрешает прерывания. Без
        /// общей секции signal_from_isr() мог бы проверить пустую очередь,
        /// а задача — тут же в неё встать: ресурс остался бы в счётчике,
        /// а задача спала бы вечно.
        void signal_from_isr() noexcept {
            Lock l;
            give_locked();
        }

        /// Текущее значение (для отладки).
        /// Acquire: счётчик читается вне критической секции.
        uint32_t value() const noexcept {
            return count_.load(MemoryOrderAcquire);
        }

        /// Сколько задач ждёт (диагностика/тесты).
        Size waiters() const noexcept { return waiters_.size(); }
    };

}
