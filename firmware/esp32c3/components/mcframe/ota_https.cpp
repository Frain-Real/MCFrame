// ESP32-C3: HTTPS OTA with TLS (certificate bundle).
#include "isrg_root_x1.hpp"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include <cstddef>

namespace {
constexpr char kTag[] = "ota";
}

extern "C" bool mcframe_c3_ota_https(const char* url) {
    if (url == nullptr || url[0] == '\0') return false;
    esp_http_client_config_t http{};
    http.url = url;
    http.cert_pem = kIsrgRootX1Pem;   // TLS: pin to ISRG Root X1
    http.timeout_ms = 15000;
    http.keep_alive_enable = true;

    esp_https_ota_config_t cfg{};
    cfg.http_config = &http;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "begin failed: %s", esp_err_to_name(err));
        return false;
    }
    for (;;) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "perform failed: %s", esp_err_to_name(err));
        (void)esp_https_ota_abort(handle);
        return false;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        (void)esp_https_ota_abort(handle);
        return false;
    }
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "finish failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "OTA complete, restarting");
    esp_restart();
    return true;
}
