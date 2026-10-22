// boards/esp32c3/wifi_esp.hpp
#pragma once
#include "hal/INetwork.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

namespace embed::esp32c3 {

using hal::NetworkConfig;
using hal::NetEvent;
using hal::NetEventCallback;
using hal::NetState;
using hal::ScanResult;

class WifiEsp final : public hal::INetwork {
public:
    Status init(const NetworkConfig& cfg) noexcept override;
    Status reconfigure(const NetworkConfig& cfg) noexcept override;
    Status connect() noexcept override;
    Status disconnect() noexcept override;
    Status start_ap(const NetworkConfig& cfg) noexcept override;
    Status scan(ScanResult* out, Size max, Size& found) noexcept override;
    NetState state() const noexcept override { return state_; }
    Status subscribe(NetEventCallback cb, void* ctx) noexcept override;
    uint32_t caps() const noexcept override;

private:
    static void event_handler(void* arg, const char* base, int32_t id, void* data) noexcept;
    static void dispatch_task(void* arg) noexcept;
    void post(const NetEvent& e) noexcept;
    void copy_cfg(const NetworkConfig& cfg) noexcept;

    bool inited_ = false;
    volatile NetState state_ = NetState::Off;
    char ssid_[33]{};
    char pass_[65]{};
    NetEventCallback cb_ = nullptr;
    void* cb_ctx_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    StaticQueue_t queue_storage_{};
    NetEvent queue_buf_[8]{};
    TaskHandle_t task_ = nullptr;
    StaticTask_t task_storage_{};
    StackType_t task_stack_[2048]{};
};

} // namespace embed::esp32c3
