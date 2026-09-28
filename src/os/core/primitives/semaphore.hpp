#pragma once

#include "types.hpp"

namespace embed {

    enum class sem_type {
        binary,
        counting 
    };

    template <sem_type Type, uint32_t MaxCount = 1>
    class semaphore {
        static_assert(MaxCount >= 1, "semaphore: MaxCount must be >= 1");

        uint32_t count_{0};

    public:
        semaphore() noexcept = default;
        semaphore(const semaphore&)            = delete;
        semaphore& operator=(const semaphore&) = delete;

        // Operations
        /// Опустить (ждать). Крутится пока count == 0.
        void wait() noexcept {
            while (true) {
                uint32_t c = __atomic_load_n(&count_, __ATOMIC_ACQUIRE);
                if (c == 0) continue;
                if (__atomic_compare_exchange_n(&count_, &c, c - 1, false,
                    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
                    break;
                }
            }
        }
        // Попробовать опустить без ожидания. true если удалось.
        bool try_wait() noexcept {
            uint32_t c = __atomic_load_n(&count_, __ATOMIC_ACQUIRE);
            do {
                if (c == 0) return false;
            } while (!__atomic_compare_exchange_n(&count_, &c, c - 1, false,
                __ATOMIC_RELEASE, __ATOMIC_RELAXED));
            return true;
        }
        // Поднять (сигнал).
        void signal() noexcept {
            if constexpr (Type == sem_type::binary) {
                __atomic_store_n(&count_, 1, __ATOMIC_RELEASE);
            } else {
                uint32_t c = __atomic_load_n(&count_, __ATOMIC_ACQUIRE);
                do {
                    if (c >= MaxCount) return;
                } while (!__atomic_compare_exchange_n(&count_, &c, c + 1, false,
                    __ATOMIC_RELEASE, __ATOMIC_RELAXED));
            }
        }
        // Текущее значение (для отладки).
        uint32_t value() const noexcept {
            return __atomic_load_n(&count_, __ATOMIC_ACQUIRE);
        }
    };

}