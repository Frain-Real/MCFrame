// embed/types.hpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace embed {
    enum MemoryOrder {
        MemoryOrderRelaxed = __ATOMIC_RELAXED,
        MemoryOrderAcquire = __ATOMIC_ACQUIRE,
        MemoryOrderRelease = __ATOMIC_RELEASE,
        MemoryOrderAcqRel  = __ATOMIC_ACQ_REL,
        MemoryOrderSeqCst  = __ATOMIC_SEQ_CST
    };

    // Atomic wrapper. Только load/store — больше ничего не нужно.
    template <typename T>
    class atomic_t {
        volatile T v_;

    public:
        atomic_t() noexcept = default;
        explicit atomic_t(T desired) noexcept : v_(desired) {}

        T load(MemoryOrder mo = MemoryOrderSeqCst) const noexcept {
            return __atomic_load_n(&v_, static_cast<int>(mo));
        }

        void store(T desired, MemoryOrder mo = MemoryOrderSeqCst) noexcept {
            __atomic_store_n(&v_, desired, static_cast<int>(mo));
        }
    };

}
