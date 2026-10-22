// ESP32-C3 builtins: help, reboot, WiFi provisioning, BLE control, HTTPS OTA.
#include "os/shell/shell.hpp"
#include "board.hpp"
#include "esp_system.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

extern "C" bool mcframe_c3_ota_https(const char* url);

namespace embed::shell {
namespace {
using embed::hal::BleRole;
using embed::hal::BleScanResult;
using embed::hal::NetworkConfig;

void cmd_help(Console& c, const char*) noexcept { c.help(); }
void cmd_reboot(Console&, const char*) noexcept { esp_restart(); }

bool starts_with(const char* s, const char* p) noexcept {
    while (*p != '\0') { if (*s != *p) return false; ++s; ++p; }
    return true;
}

void cmd_wifi(Console& c, const char* args) noexcept {
    if (args == nullptr) { c.write("usage: wifi <ssid> <password>\n"); return; }
    while (*args == ' ') ++args;
    const char* sp = args;
    while (*sp != '\0' && *sp != ' ') ++sp;
    if (*sp != ' ') { c.write("usage: wifi <ssid> <password>\n"); return; }
    char ssid[33]{};
    char pass[65]{};
    Size n = static_cast<Size>(sp - args);
    if (n >= sizeof(ssid)) n = sizeof(ssid) - 1u;
    for (Size i = 0u; i < n; ++i) ssid[i] = args[i];
    const char* p = sp;
    while (*p == ' ') ++p;
    Size m = 0u;
    while (p[m] != '\0' && m + 1u < sizeof(pass)) { pass[m] = p[m]; ++m; }

    NetworkConfig cfg{};
    cfg.ssid = ssid;
    cfg.password = pass;
    if (board::wifi().reconfigure(cfg) != Status::Ok) {
        c.write("wifi: reconfigure failed\n");
        return;
    }
    (void)board::nvs().write("wifi_ssid", ssid, std::strlen(ssid));
    (void)board::nvs().write("wifi_pass", pass, std::strlen(pass));
    c.write("wifi: credentials applied\n");
}

void scan_cb(void* ctx, const BleScanResult& r) noexcept {
    auto* c = static_cast<Console*>(ctx);
    if (c == nullptr) return;
    c->write("ble: ");
    c->write(r.name[0] != '\0' ? r.name : "(no name)");
    c->write(" rssi=");
    c->write_u32(static_cast<uint32_t>(static_cast<int32_t>(r.rssi)));
    c->write("\n");
}

void cmd_ble(Console& c, const char* args) noexcept {
    if (args == nullptr || *args == '\0') {
        c.write("ble: caps=");
        c.write_hex(board::ble().caps());
        c.write("\nusage: ble scan | ble adv | ble stop\n");
        return;
    }
    if (starts_with(args, "scan")) {
        (void)board::ble().set_role(BleRole::Observer);
        const Status st = board::ble().scan_start(&scan_cb, &c);
        c.write(st == Status::Ok ? "ble: scanning\n" : "ble: scan failed\n");
    } else if (starts_with(args, "adv")) {
        (void)board::ble().set_role(BleRole::Peripheral);
        c.write("ble: advertising\n");
    } else if (starts_with(args, "stop")) {
        (void)board::ble().scan_stop();
        (void)board::ble().stop_advertise();
        c.write("ble: stopped\n");
    } else {
        c.write("usage: ble scan | ble adv | ble stop\n");
    }
}

void cmd_heap(Console& c, const char*) noexcept {
    c.write("heap: free=");
    c.write_u32(esp_get_free_heap_size());
    c.write(" min=");
    c.write_u32(esp_get_minimum_free_heap_size());
    c.write("\n");
}

void cmd_tasks(Console& c, const char*) noexcept {
    TaskStatus_t st[24]{};
    const UBaseType_t n = uxTaskGetSystemState(st, 24u, nullptr);
    for (UBaseType_t i = 0u; i < n; ++i) {
        c.write(st[i].pcTaskName != nullptr ? st[i].pcTaskName : "?");
        c.write(" prio=");
        c.write_u32(st[i].uxCurrentPriority);
        c.write(" stack_free=");
        c.write_u32(st[i].usStackHighWaterMark);
        c.write("\n");
    }
}

void cmd_reset(Console& c, const char*) noexcept {
    c.write("reset reason: ");
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: c.write("power-on"); break;
        case ESP_RST_SW: c.write("software"); break;
        case ESP_RST_PANIC: c.write("panic"); break;
        case ESP_RST_INT_WDT: c.write("interrupt-wdt"); break;
        case ESP_RST_TASK_WDT: c.write("task-wdt"); break;
        case ESP_RST_BROWNOUT: c.write("brownout"); break;
        default: c.write("other"); break;
    }
    c.write("\n");
}

void cmd_power(Console& c, const char*) noexcept {
    esp_pm_config_t cfg{};
    if (esp_pm_get_configuration(&cfg) == ESP_OK) {
        c.write("power: max=");
        c.write_u32(static_cast<uint32_t>(cfg.max_freq_mhz));
        c.write(" min=");
        c.write_u32(static_cast<uint32_t>(cfg.min_freq_mhz));
        c.write(" light_sleep=");
        c.write(cfg.light_sleep_enable ? "on\n" : "off\n");
    } else {
        c.write("power: PM disabled\n");
    }
}

void cmd_ota(Console& c, const char* args) noexcept {
    if (args == nullptr || *args == '\0') { c.write("usage: ota <https-url>\n"); return; }
    c.write("ota: starting TLS download...\n");
    if (!mcframe_c3_ota_https(args)) c.write("ota: failed\n");
}

const Command kBuiltins[] = {
    {"help",   "list commands", &cmd_help},
    {"reboot", "restart ESP32-C3", &cmd_reboot},
    {"wifi",   "wifi <ssid> <password>", &cmd_wifi},
    {"ble",    "ble scan|adv|stop", &cmd_ble},
    {"ota",    "ota <https-url>", &cmd_ota},
    {"heap",   "free heap", &cmd_heap},
    {"tasks",  "task list", &cmd_tasks},
    {"reset",  "reset reason", &cmd_reset},
    {"power",  "power config", &cmd_power},
};

} // namespace

const Command* builtins(Size& count) noexcept {
    count = sizeof(kBuiltins) / sizeof(kBuiltins[0]);
    return kBuiltins;
}

} // namespace embed::shell
