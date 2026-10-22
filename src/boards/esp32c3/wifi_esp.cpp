#include "boards/esp32c3/wifi_esp.hpp"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <cstring>

namespace embed::esp32c3 {
using hal::NetCapAp;
using hal::NetCapEvents;
using hal::NetCapScan;
using hal::NetCapSta;
namespace {
constexpr char kTag[] = "wifi";
void copy_str(char* dst, Size cap, const char* src) noexcept {
    if (dst == nullptr || cap == 0u) return;
    Size i = 0u;
    if (src != nullptr) while (src[i] != '\0' && i + 1u < cap) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}
} // namespace

void WifiEsp::copy_cfg(const NetworkConfig& cfg) noexcept {
    copy_str(ssid_, sizeof(ssid_), cfg.ssid);
    copy_str(pass_, sizeof(pass_), cfg.password);
}

Status WifiEsp::init(const NetworkConfig& cfg) noexcept {
    if (inited_) return Status::AlreadyExists;
    copy_cfg(cfg);

    queue_ = xQueueCreateStatic(8u, sizeof(NetEvent), reinterpret_cast<uint8_t*>(queue_buf_),
                                &queue_storage_);
    if (queue_ == nullptr) return Status::NoMemory;
    task_ = xTaskCreateStatic(&WifiEsp::dispatch_task, "wifi-ev", 2048u, this, 9u,
                              task_stack_, &task_storage_);
    if (task_ == nullptr) return Status::NoMemory;

    if (esp_netif_init() != ESP_OK) return Status::HardwareError;
    esp_err_t st = esp_event_loop_create_default();
    if (st != ESP_OK && st != ESP_ERR_INVALID_STATE) return Status::HardwareError;
    (void)esp_netif_create_default_wifi_sta();
    (void)esp_netif_create_default_wifi_ap();

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&icfg) != ESP_OK) return Status::HardwareError;
    if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   &WifiEsp::event_handler, this) != ESP_OK) {
        return Status::HardwareError;
    }
    if (esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   &WifiEsp::event_handler, this) != ESP_OK) {
        return Status::HardwareError;
    }

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return Status::HardwareError;
    wifi_config_t sta{};
    copy_str(reinterpret_cast<char*>(sta.sta.ssid), sizeof(sta.sta.ssid), ssid_);
    copy_str(reinterpret_cast<char*>(sta.sta.password), sizeof(sta.sta.password), pass_);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) return Status::HardwareError;

    wifi_config_t ap{};
    copy_str(reinterpret_cast<char*>(ap.ap.ssid), sizeof(ap.ap.ssid), ssid_);
    ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(reinterpret_cast<const char*>(ap.ap.ssid)));
    copy_str(reinterpret_cast<char*>(ap.ap.password), sizeof(ap.ap.password), pass_);
    ap.ap.channel = 1u;
    ap.ap.max_connection = 4u;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return Status::HardwareError;

    if (esp_wifi_start() != ESP_OK) return Status::HardwareError;
    esp_wifi_set_ps(WIFI_PS_NONE);
    state_ = NetState::Idle;
    inited_ = true;
    return Status::Ok;
}

Status WifiEsp::reconfigure(const NetworkConfig& cfg) noexcept {
    if (!inited_) return init(cfg);
    copy_cfg(cfg);
    wifi_config_t sta{};
    copy_str(reinterpret_cast<char*>(sta.sta.ssid), sizeof(sta.sta.ssid), ssid_);
    copy_str(reinterpret_cast<char*>(sta.sta.password), sizeof(sta.sta.password), pass_);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) return Status::HardwareError;
    wifi_config_t ap{};
    copy_str(reinterpret_cast<char*>(ap.ap.ssid), sizeof(ap.ap.ssid), ssid_);
    ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(reinterpret_cast<const char*>(ap.ap.ssid)));
    copy_str(reinterpret_cast<char*>(ap.ap.password), sizeof(ap.ap.password), pass_);
    ap.ap.channel = 1u;
    ap.ap.max_connection = 4u;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return Status::HardwareError;
    if (ssid_[0] != '\0') return connect();
    return Status::Ok;
}

Status WifiEsp::connect() noexcept {
    if (!inited_) return Status::Unavailable;
    state_ = NetState::Connecting;
    return esp_wifi_connect() == ESP_OK ? Status::Ok : Status::HardwareError;
}
Status WifiEsp::disconnect() noexcept {
    if (!inited_) return Status::Unavailable;
    (void)esp_wifi_disconnect();
    state_ = NetState::Idle;
    return Status::Ok;
}
Status WifiEsp::start_ap(const NetworkConfig& cfg) noexcept {
    if (!inited_) return Status::Unavailable;
    copy_cfg(cfg);
    wifi_config_t ap{};
    copy_str(reinterpret_cast<char*>(ap.ap.ssid), sizeof(ap.ap.ssid), ssid_);
    ap.ap.ssid_len = static_cast<uint8_t>(std::strlen(reinterpret_cast<const char*>(ap.ap.ssid)));
    copy_str(reinterpret_cast<char*>(ap.ap.password), sizeof(ap.ap.password), pass_);
    ap.ap.channel = 1u;
    ap.ap.max_connection = 4u;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return Status::HardwareError;
    state_ = NetState::Ap;
    return Status::Ok;
}
Status WifiEsp::scan(ScanResult* out, Size max, Size& found) noexcept {
    found = 0u;
    if (!inited_ || out == nullptr || max == 0u) return Status::InvalidArg;
    wifi_scan_config_t sc{};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return Status::HardwareError;
    uint16_t count = 0u;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) return Status::HardwareError;
    wifi_ap_record_t rec[16]{};
    uint16_t take = (count > 16u) ? 16u : count;
    if (take > max) take = static_cast<uint16_t>(max);
    if (esp_wifi_scan_get_ap_records(&take, rec) != ESP_OK) return Status::HardwareError;
    for (uint16_t i = 0u; i < take; ++i) {
        copy_str(out[i].ssid, sizeof(out[i].ssid), reinterpret_cast<const char*>(rec[i].ssid));
        out[i].rssi = rec[i].rssi;
        out[i].channel = rec[i].primary;
    }
    found = take;
    NetEvent e{NetEvent::Kind::ScanDone, 0};
    post(e);
    return Status::Ok;
}
Status WifiEsp::subscribe(NetEventCallback cb, void* ctx) noexcept {
    cb_ = cb;
    cb_ctx_ = ctx;
    return Status::Ok;
}
uint32_t WifiEsp::caps() const noexcept {
    return NetCapSta | NetCapAp | NetCapScan | NetCapEvents;
}
void WifiEsp::post(const NetEvent& e) noexcept {
    if (queue_ != nullptr) (void)xQueueSend(queue_, &e, 0u);
}
void WifiEsp::dispatch_task(void* arg) noexcept {
    auto* self = static_cast<WifiEsp*>(arg);
    NetEvent e{};
    for (;;) {
        if (xQueueReceive(self->queue_, &e, portMAX_DELAY) == pdTRUE) {
            if (self->cb_ != nullptr) self->cb_(self->cb_ctx_, e);
        }
    }
}
void WifiEsp::event_handler(void* arg, const char* base, int32_t id, void*) noexcept {
    auto* self = static_cast<WifiEsp*>(arg);
    if (self == nullptr) return;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (self->ssid_[0] != '\0') (void)esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            self->state_ = NetState::Connecting;
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            self->state_ = NetState::Idle;
            NetEvent e{NetEvent::Kind::Disconnected, 0};
            self->post(e);
        } else if (id == WIFI_EVENT_AP_START) {
            self->state_ = NetState::Ap;
            NetEvent e{NetEvent::Kind::Connected, 1};
            self->post(e);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        self->state_ = NetState::Connected;
        NetEvent e{NetEvent::Kind::GotIp, 0};
        self->post(e);
        NetEvent c{NetEvent::Kind::Connected, 0};
        self->post(c);
    }
    ESP_LOGD(kTag, "event %s %d", base, static_cast<int>(id));
}

} // namespace embed::esp32c3
