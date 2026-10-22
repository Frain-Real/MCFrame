// ESP32-C3 application: NVS, WiFi AP+STA, BLE peripheral GATT, UART shell.
#include "board.hpp"
#include "boards/common/shell_board.hpp"
#include "os/kernel/kernel_api.hpp"
#include "os/shell/shell.hpp"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include <cstring>

extern "C" bool mcframe_c3_watchdog_add(void* task_handle);
extern "C" bool mcframe_c3_power_init(void);

namespace {

constexpr char kTag[] = "mcframe";
constexpr char kDefaultSsid[] = "MCFrame-C3";
constexpr char kDefaultPass[] = "mcframe123";

embed::shell::Console g_shell;
embed::kernel::task   g_shell_task;
StackType_t           g_shell_stack[4096];

char g_ssid[33]{};
char g_pass[65]{};

void shell_write(void* /*ctx*/, const char* text) noexcept {
    if (text == nullptr) return;
    embed::Size len = 0u;
    while (text[len] != '\0') ++len;
    embed::Size written = 0u;
    (void)board::console().write(text, len, written, 100u);
}

void load_credentials() noexcept {
    embed::Size len = 0u;
    if (board::nvs().read("wifi_ssid", g_ssid, sizeof(g_ssid) - 1u, len) == embed::Status::Ok) {
        g_ssid[len] = '\0';
    } else {
        embed::Size n = 0u;
        while (kDefaultSsid[n] != '\0') { g_ssid[n] = kDefaultSsid[n]; ++n; }
        g_ssid[n] = '\0';
    }
    if (board::nvs().read("wifi_pass", g_pass, sizeof(g_pass) - 1u, len) == embed::Status::Ok) {
        g_pass[len] = '\0';
    } else {
        embed::Size n = 0u;
        while (kDefaultPass[n] != '\0') { g_pass[n] = kDefaultPass[n]; ++n; }
        g_pass[n] = '\0';
    }
}

void save_credentials(const char* ssid, const char* pass) noexcept {
    if (ssid != nullptr) (void)board::nvs().write("wifi_ssid", ssid, std::strlen(ssid));
    if (pass != nullptr) (void)board::nvs().write("wifi_pass", pass, std::strlen(pass));
}

void ble_write(void* /*ctx*/, uint16_t attr, const void* data, embed::Size len) noexcept {
    if (attr == 3u) {
        const char* ssid = g_ssid;
        const char* pass = g_pass;
        save_credentials(ssid, pass);
        embed::hal::NetworkConfig cfg{};
        cfg.ssid = ssid;
        cfg.password = pass;
        (void)board::wifi().reconfigure(cfg);
        ESP_LOGI(kTag, "provisioning applied via BLE");
        return;
    }
    if (data == nullptr || len == 0u) return;
    const char* s = static_cast<const char*>(data);
    const embed::Size n = (len < 32u) ? len : 32u;
    if (attr == 1u) {
        for (embed::Size i = 0u; i < n; ++i) g_ssid[i] = s[i];
        g_ssid[n] = '\0';
    } else if (attr == 2u) {
        const embed::Size pn = (len < 64u) ? len : 64u;
        for (embed::Size i = 0u; i < pn; ++i) g_pass[i] = s[i];
        g_pass[pn] = '\0';
    }
}

void wifi_event(void* /*ctx*/, const embed::hal::NetEvent& e) noexcept {
    if (e.kind == embed::hal::NetEvent::Kind::GotIp) ESP_LOGI(kTag, "WiFi got IP");
    if (e.kind == embed::hal::NetEvent::Kind::Disconnected) ESP_LOGW(kTag, "WiFi disconnected");
}

void shell_task(void* /*ctx*/) noexcept {
    for (;;) {
        char c = '\0';
        embed::Size n = 0u;
        if (board::console().read(&c, 1u, n, 20u) == embed::Status::Ok && n == 1u) {
            g_shell.feed(c);
        }
        (void)esp_task_wdt_reset();
    }
}

void confirm_ota() noexcept {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) return;
    esp_ota_img_states_t st{};
    if (esp_ota_get_state_partition(running, &st) != ESP_OK) return;
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
            ESP_LOGE(kTag, "OTA confirm failed");
        } else {
            ESP_LOGI(kTag, "OTA image confirmed");
        }
    }
}

void nvs_boot() noexcept {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(kTag, "nvs init failed: %d", static_cast<int>(err));
}

} // namespace

extern "C" void app_main(void) {
    nvs_boot();
    board::init();
    (void)mcframe_c3_power_init();
    load_credentials();

    embed::hal::NetworkConfig net{};
    net.ssid = g_ssid;
    net.password = g_pass;
    (void)board::wifi().init(net);
    (void)board::wifi().subscribe(&wifi_event, nullptr);
    if (g_ssid[0] != '\0') (void)board::wifi().connect();

    embed::hal::BleConfig bcfg{};
    bcfg.name = "MCFrame-C3";
    (void)board::ble().init(bcfg);
    (void)board::ble().on_write(&ble_write, nullptr);
    (void)board::ble().set_role(embed::hal::BleRole::Peripheral);

    g_shell.init(&shell_write, nullptr);
    board_shell::add_board_commands(g_shell);
    g_shell.prompt();

    const embed::Status st = g_shell_task.init(
        &shell_task, nullptr, "shell", g_shell_stack,
        sizeof(g_shell_stack) / sizeof(g_shell_stack[0]), 0u);
    if (st == embed::Status::Ok) {
        (void)mcframe_c3_watchdog_add(g_shell_task.handle());
    } else {
        ESP_LOGE(kTag, "shell task init failed: %d", static_cast<int>(st));
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    confirm_ota();
    ESP_LOGI(kTag, "MCFrame C3 ready");
}
