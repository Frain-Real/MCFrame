// ESP32-C3 runtime adapters: task watchdog, time, panic reset.
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char kTag[] = "mcframe";
} // namespace

extern "C" bool mcframe_c3_watchdog_add(void* task_handle) {
    return esp_task_wdt_add(static_cast<TaskHandle_t>(task_handle)) == ESP_OK;
}
extern "C" void mcframe_c3_watchdog_kick(void) {
    (void)esp_task_wdt_reset();
}
extern "C" uint64_t mcframe_c3_time_us(void) {
    return static_cast<uint64_t>(esp_timer_get_time());
}
extern "C" void mcframe_c3_panic_reset(const char* why) {
    ESP_LOGE(kTag, "panic: %s", why != nullptr ? why : "unknown");
    esp_restart();
}
