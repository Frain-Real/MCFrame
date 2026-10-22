// boards/esp32c3/ble_esp.hpp
#pragma once
#include "hal/IBle.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace embed::esp32c3 {

using hal::BleAddr;
using hal::BleAdvData;
using hal::BleConfig;
using hal::BleEvent;
using hal::BleEventCallback;
using hal::BleGattService;
using hal::BleRole;
using hal::BleScanCallback;
using hal::BleUuid;
using hal::BleWriteCallback;

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
    uint32_t caps() const noexcept override;

private:
    static void host_task(void* arg);
    static void on_sync();
    static int gap_event(::ble_gap_event* event, void* arg);
    static int gatt_access(uint16_t conn, uint16_t attr,
                           ::ble_gatt_access_ctxt* ctxt, void* arg);
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
