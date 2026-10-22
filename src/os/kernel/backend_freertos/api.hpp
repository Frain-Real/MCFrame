// os/kernel/backend_freertos/api.hpp
// ---------------------------------------------------------------------------
// ESP32-C3: kernel API поверх FreeRTOS/ESP-IDF.
//
// Здесь НЕТ планировщика MCFrame: на C3 WiFi/BLE требуют FreeRTOS (ADR-004).
// Все объекты статические (configSUPPORT_STATIC_ALLOCATION=1).
// Приоритеты MCFrame 0..7 отображаются в 12..5; верхние приоритеты IDF
// (WiFi/BLE/таймеры) не трогаем.
// ---------------------------------------------------------------------------
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "../../common.hpp"
#include "../../core/primitives/semaphore.hpp"   // sem_type

namespace embed::kernel {

inline TickType_t to_ticks(Timeout timeout) noexcept {
    if (timeout == 0u || timeout > 0x7FFFFFFFu) return portMAX_DELAY;
    return static_cast<TickType_t>(timeout);
}

inline UBaseType_t map_priority(uint32_t priority) noexcept {
    constexpr UBaseType_t kMaxApp = (configMAX_PRIORITIES > 13)
        ? static_cast<UBaseType_t>(12) : static_cast<UBaseType_t>(configMAX_PRIORITIES - 2);
    if (priority > 7u) priority = 7u;
    return static_cast<UBaseType_t>(kMaxApp - priority);
}

class task {
public:
    task() noexcept = default;
    task(const task&) = delete;
    task& operator=(const task&) = delete;

    Status init(void (*fn)(void*), void* ctx, const char* name,
                StackType_t* stack, uint32_t stack_words,
                uint32_t priority = 5u) noexcept {
        if (fn == nullptr || stack == nullptr || stack_words < 64u) {
            return Status::InvalidArg;
        }
        stack_ = stack;
        handle_ = xTaskCreateStatic(fn, name, stack_words, ctx,
                                    map_priority(priority), stack_, &tcb_);
        return (handle_ != nullptr) ? Status::Ok : Status::NoMemory;
    }

    bool valid() const noexcept { return handle_ != nullptr; }
    TaskHandle_t handle() const noexcept { return handle_; }
    void suspend() noexcept { if (handle_ != nullptr) vTaskSuspend(handle_); }
    void resume() noexcept { if (handle_ != nullptr) vTaskResume(handle_); }

private:
    StaticTask_t tcb_{};
    StackType_t* stack_ = nullptr;
    TaskHandle_t handle_ = nullptr;
};

class mutex {
public:
    Status init() noexcept {
        handle_ = xSemaphoreCreateMutexStatic(&storage_);
        return (handle_ != nullptr) ? Status::Ok : Status::NoMemory;
    }
    Status lock(uint32_t ticks = 0xFFFFFFFFu) noexcept {
        if (handle_ == nullptr) return Status::Unavailable;
        return (xSemaphoreTake(handle_, to_ticks(ticks)) == pdTRUE)
            ? Status::Ok : Status::Timeout;
    }
    Status try_lock() noexcept {
        if (handle_ == nullptr) return Status::Unavailable;
        return (xSemaphoreTake(handle_, 0u) == pdTRUE) ? Status::Ok : Status::Busy;
    }
    void unlock() noexcept { if (handle_ != nullptr) (void)xSemaphoreGive(handle_); }
private:
    StaticSemaphore_t storage_{};
    SemaphoreHandle_t handle_ = nullptr;
};

template <sem_type Type = sem_type::binary, uint32_t MaxCount = 1u>
class semaphore {
public:
    Status init(uint32_t initial = 0u) noexcept {
        if (Type == sem_type::binary) {
            handle_ = xSemaphoreCreateBinaryStatic(&storage_);
            if (handle_ != nullptr && initial != 0u) (void)xSemaphoreGive(handle_);
        } else {
            handle_ = xSemaphoreCreateCountingStatic(MaxCount, initial, &storage_);
        }
        return (handle_ != nullptr) ? Status::Ok : Status::NoMemory;
    }
    Status wait(uint32_t ticks = 0xFFFFFFFFu) noexcept {
        if (handle_ == nullptr) return Status::Unavailable;
        return (xSemaphoreTake(handle_, to_ticks(ticks)) == pdTRUE)
            ? Status::Ok : Status::Timeout;
    }
    Status signal() noexcept {
        if (handle_ == nullptr) return Status::Unavailable;
        return (xSemaphoreGive(handle_) == pdTRUE) ? Status::Ok : Status::Busy;
    }
    bool signal_from_isr() noexcept {
        if (handle_ == nullptr) return false;
        BaseType_t woken = pdFALSE;
        const bool ok = xSemaphoreGiveFromISR(handle_, &woken) == pdTRUE;
        portYIELD_FROM_ISR(woken);
        return ok;
    }
    uint32_t value() const noexcept {
        return (handle_ != nullptr) ? uxSemaphoreGetCount(handle_) : 0u;
    }
private:
    StaticSemaphore_t storage_{};
    SemaphoreHandle_t handle_ = nullptr;
};

template <typename T, Size N>
class message_queue {
public:
    Status init() noexcept {
        handle_ = xQueueCreateStatic(N, sizeof(T), storage_, &storage_struct_);
        return (handle_ != nullptr) ? Status::Ok : Status::NoMemory;
    }
    Status push(const T& value, uint32_t ticks = 0u) noexcept {
        if (handle_ == nullptr) return Status::Unavailable;
        return (xQueueSend(handle_, &value, to_ticks(ticks)) == pdTRUE)
            ? Status::Ok : Status::Timeout;
    }
    Status pop(T& out, uint32_t ticks = 0u) noexcept {
        if (handle_ == nullptr) return Status::Unavailable;
        return (xQueueReceive(handle_, &out, to_ticks(ticks)) == pdTRUE)
            ? Status::Ok : Status::Timeout;
    }
    bool try_push(const T& value) noexcept {
        return handle_ != nullptr && xQueueSend(handle_, &value, 0u) == pdTRUE;
    }
    bool try_pop(T& out) noexcept {
        return handle_ != nullptr && xQueueReceive(handle_, &out, 0u) == pdTRUE;
    }
    bool push_from_isr(const T& value) noexcept {
        BaseType_t woken = pdFALSE;
        const bool ok = xQueueSendFromISR(handle_, &value, &woken) == pdTRUE;
        portYIELD_FROM_ISR(woken);
        return ok;
    }
    bool pop_from_isr(T& out) noexcept {
        BaseType_t woken = pdFALSE;
        const bool ok = xQueueReceiveFromISR(handle_, &out, &woken) == pdTRUE;
        portYIELD_FROM_ISR(woken);
        return ok;
    }
    Size size() const noexcept { return (handle_ != nullptr) ? uxQueueMessagesWaiting(handle_) : 0u; }
    bool empty() const noexcept { return size() == 0u; }
    static constexpr Size capacity() noexcept { return N; }
private:
    StaticQueue_t storage_struct_{};
    alignas(T) uint8_t storage_[N * sizeof(T)]{};
    QueueHandle_t handle_ = nullptr;
};

class event_flags {
public:
    Status init() noexcept {
        handle_ = xEventGroupCreateStatic(&storage_);
        return (handle_ != nullptr) ? Status::Ok : Status::NoMemory;
    }
    Status set(uint32_t bits) noexcept {
        if (handle_ == nullptr) return Status::Unavailable;
        (void)xEventGroupSetBits(handle_, bits);
        return Status::Ok;
    }
    Status set_from_isr(uint32_t bits) noexcept {
        if (handle_ == nullptr) return Status::Unavailable;
        BaseType_t woken = pdFALSE;
        (void)xEventGroupSetBitsFromISR(handle_, bits, &woken);
        portYIELD_FROM_ISR(woken);
        return Status::Ok;
    }
    Status clear(uint32_t bits) noexcept {
        if (handle_ == nullptr) return Status::Unavailable;
        (void)xEventGroupClearBits(handle_, bits);
        return Status::Ok;
    }
    Status wait(uint32_t mask, bool all, bool clear, uint32_t ticks = 0xFFFFFFFFu) noexcept {
        if (handle_ == nullptr) return Status::Unavailable;
        const EventBits_t r = xEventGroupWaitBits(
            handle_, mask, clear ? pdTRUE : pdFALSE, all ? pdTRUE : pdFALSE,
            to_ticks(ticks));
        const bool ok = all ? ((r & mask) == mask) : ((r & mask) != 0u);
        return ok ? Status::Ok : Status::Timeout;
    }
    uint32_t value() const noexcept {
        if (handle_ == nullptr) return 0u;
        return static_cast<uint32_t>(xEventGroupGetBits(handle_));
    }
    bool any(uint32_t mask) const noexcept { return (value() & mask) != 0u; }
    bool all(uint32_t mask) const noexcept { return (value() & mask) == mask; }
private:
    StaticEventGroup_t storage_{};
    EventGroupHandle_t handle_ = nullptr;
};

inline Status sleep_for(uint32_t ticks) noexcept {
    vTaskDelay(ticks == 0u ? 1u : static_cast<TickType_t>(ticks));
    return Status::Ok;
}
inline Status sleep() noexcept {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return Status::Ok;
}
inline Status wakeup(task* t) noexcept {
    if (t == nullptr || t->handle() == nullptr) return Status::InvalidArg;
    (void)xTaskNotifyGive(t->handle());
    return Status::Ok;
}
inline uint64_t ticks() noexcept { return static_cast<uint64_t>(xTaskGetTickCount()); }

} // namespace embed::kernel
