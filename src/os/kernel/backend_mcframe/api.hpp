// os/kernel/backend_mcframe/api.hpp
// ---------------------------------------------------------------------------
// Реализация kernel API поверх существующего планировщика MCFrame.
// Никакой второй реализации: это тонкие алиасы и функции, чтобы приложение
// зависело от embed::kernel, а не от конкретных классов.
// ---------------------------------------------------------------------------
#pragma once

#include "../../core/task/task.hpp"
#include "../../core/task/sheduler.hpp"
#include "../../core/primitives/mutex.hpp"
#include "../../core/primitives/semaphore.hpp"
#include "../../core/ipc/message_queue.hpp"
#include "../../core/ipc/mailbox.hpp"
#include "../../core/ipc/event_flags.hpp"

namespace embed::kernel {

using task        = ::embed::task;
using scheduler   = ::embed::scheduler;
using mutex       = ::embed::mutex;
template <::embed::sem_type Type = ::embed::sem_type::binary, uint32_t MaxCount = 1u>
using semaphore = ::embed::semaphore<Type, MaxCount>;
using event_flags = ::embed::EventFlags;
using wait_queue  = ::embed::wait_queue;

template <typename T, Size N>
using message_queue = ::embed::MessageQueue<T, N>;

template <typename T>
using mailbox = ::embed::Mailbox<T>;

inline Status sleep_for(uint32_t ticks) noexcept {
    return scheduler::instance().sleep_for(ticks);
}

inline task* current() noexcept {
    return scheduler::instance().current_task();
}

inline uint64_t ticks() noexcept {
    return scheduler::instance().tick_count();
}

} // namespace embed::kernel
