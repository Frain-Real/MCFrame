// boards/esp32c3/ble_esp.hpp
#pragma once
#include "hal/IBle.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;
struct ble_gatt_error;
struct ble_gatt_svc;
struct ble_gatt_attr;
struct ble_gatt_chr;
struct ble_gatt_dsc;

namespace embed::esp32c3 {

using hal::BleAddr;
using hal::BleAdvData;
using hal::BleConfig;
using hal::BleEvent;
using hal::BleEventCallback;
using hal::BleGattService;
using hal::BleGattChar;
using hal::BleRole;
using hal::BleScanCallback;
using hal::BleUuid;
using hal::BleWriteCallback;
using hal::BleGattServiceCallback;
using hal::BleGattReadCallback;
using hal::BleGattWriteCallback;
using hal::BleGattCharCallback;
using hal::BleGattNotifyCallback;

class BleEsp final : public hal::IBle {
public:
    Status init(const BleConfig& cfg) noexcept override;
    Status set_role(BleRole role) noexcept override;
    Status advertise(const BleAdvData& adv) noexcept override;
    Status stop_advertise() noexcept override;
    Status add_service(const BleUuid& uuid) noexcept override;
    Status add_gatt_service(const BleGattService& svc) noexcept override;
    Status notify(uint16_t conn_handle, uint16_t attr_handle,
                  const void* data, Size len) noexcept override;
    Status subscribe(BleEventCallback cb, void* ctx) noexcept override;
    Status scan_start(BleScanCallback cb, void* ctx) noexcept override;
    Status scan_stop() noexcept override;
    Status connect(const BleAddr& addr) noexcept override;
    Status disconnect(uint16_t conn_handle) noexcept override;
    Status on_write(BleWriteCallback cb, void* ctx) noexcept override;
    Status discover_services(uint16_t conn, BleGattServiceCallback cb, void* ctx) noexcept override;
    Status discover_chars(uint16_t conn, uint16_t start_handle, uint16_t end_handle,
                          BleGattCharCallback cb, void* ctx) noexcept override;
    Status read_char(uint16_t conn, uint16_t handle,
                     BleGattReadCallback cb, void* ctx) noexcept override;
    Status write_char(uint16_t conn, uint16_t handle, const void* data, Size len,
                      bool with_response, BleGattWriteCallback cb, void* ctx) noexcept override;
    Status subscribe_notify(uint16_t conn, uint16_t char_handle, bool enable,
                            BleGattNotifyCallback cb, void* ctx) noexcept override;
    Status set_value(uint16_t handle, const void* data, Size len) noexcept override;
    uint32_t caps() const noexcept override;

private:
    static void host_task(void* arg);
    static void on_sync();
    static int gap_event(::ble_gap_event* event, void* arg);
    static int gatt_access(uint16_t conn, uint16_t attr,
                           ::ble_gatt_access_ctxt* ctxt, void* arg);
    static int custom_access(uint16_t conn, uint16_t attr,
                             ::ble_gatt_access_ctxt* ctxt, void* arg);
    static int svc_disc_cb(uint16_t conn, const struct ble_gatt_error* err,
                           const struct ble_gatt_svc* svc, void* arg);
    static int chr_disc_cb(uint16_t conn, const struct ble_gatt_error* err,
                           const struct ble_gatt_chr* chr, void* arg);
    static int dsc_disc_cb(uint16_t conn, const struct ble_gatt_error* err,
                           uint16_t chr_val_handle, const struct ble_gatt_dsc* dsc,
                           void* arg);
    static int read_cb(uint16_t conn, const struct ble_gatt_error* err,
                       struct ble_gatt_attr* attr, void* arg);
    static int write_cb(uint16_t conn, const struct ble_gatt_error* err,
                        struct ble_gatt_attr* attr, void* arg);
    static void dispatch_task(void* arg) noexcept;
    void post(const BleEvent& e) noexcept;

    bool inited_ = false;
    BleRole role_ = BleRole::Peripheral;
    char name_[32]{};
    BleEventCallback cb_ = nullptr;
    void* cb_ctx_ = nullptr;
    BleWriteCallback write_cb_ = nullptr;
    void* write_ctx_ = nullptr;
    BleScanCallback scan_cb_ = nullptr;
    void* scan_ctx_ = nullptr;
    uint16_t conn_ = 0u;
    bool advertising_ = false;
    BleGattServiceCallback svc_cb_ = nullptr;
    void* svc_ctx_ = nullptr;
    BleGattCharCallback chr_cb_ = nullptr;
    void* chr_ctx_ = nullptr;
    uint16_t cccd_handle_ = 0u;
    bool cccd_enable_ = false;
    BleGattNotifyCallback notify_cb_ = nullptr;
    void* notify_ctx_ = nullptr;
    BleGattReadCallback read_cb_ = nullptr;
    void* read_ctx_ = nullptr;
    BleGattWriteCallback gatt_write_cb_ = nullptr;
    void* gatt_write_ctx_ = nullptr;

    struct CustomChar {
        ble_uuid_any_t uuid{};
        uint8_t value[32]{};
        uint8_t len = 0u;
        uint16_t handle = 0u;
    };
    struct CustomService {
        ble_uuid_any_t uuid{};
        CustomChar chars[4]{};
        ble_gatt_chr_def defs[5]{};
        ble_gatt_svc_def svc[2]{};
        bool used = false;
    };
    static constexpr uint8_t kMaxCustomServices = 4u;
    CustomService custom_[kMaxCustomServices]{};
    uint8_t custom_count_ = 0u;
    char ssid_[33]{};
    char pass_[65]{};
    QueueHandle_t queue_ = nullptr;
    StaticQueue_t queue_storage_{};
    BleEvent queue_buf_[8]{};
    TaskHandle_t task_ = nullptr;
    StaticTask_t task_storage_{};
    StackType_t task_stack_[4096]{};
};

} // namespace embed::esp32c3
