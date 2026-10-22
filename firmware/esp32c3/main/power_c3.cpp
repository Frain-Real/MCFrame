// Phase 7: C3 power management (DFS + light sleep) and brown-out status.
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_log.h"

namespace {
constexpr char kTag[] = "power";
}

extern "C" bool mcframe_c3_power_init(void) {
    esp_pm_config_t cfg{};
    cfg.max_freq_mhz = 160;
    cfg.min_freq_mhz = 40;
    cfg.light_sleep_enable = true;
    const esp_err_t st = esp_pm_configure(&cfg);
    if (st != ESP_OK) {
        ESP_LOGE(kTag, "esp_pm_configure: %d", static_cast<int>(st));
        return false;
    }
    ESP_LOGI(kTag, "DFS 160/40 MHz + light sleep enabled");
    return true;
}
