// hal/INetwork.hpp
// ---------------------------------------------------------------------------
// WiFi как интерфейс. Асинхронный: connect() только запускает, результат —
// через события. Реализация на C3 — esp_wifi/esp_event.
// ---------------------------------------------------------------------------
#pragma once

#include "os/common.hpp"

namespace embed::hal {

enum class NetState : uint8_t { Off, Idle, Connecting, Connected, Ap, Error };

struct NetworkConfig {
    const char* ssid     = nullptr;
    const char* password = nullptr;
};

struct ScanResult {
    char    ssid[33]{};
    int8_t  rssi = 0;
    uint8_t channel = 0u;
};

struct NetEvent {
    enum class Kind : uint8_t {
        Connected, Disconnected, GotIp, ScanDone, Error
    } kind = Kind::Error;
    int32_t code = 0;
};

using NetEventCallback = void (*)(void* ctx, const NetEvent& event);

enum NetCaps : uint32_t {
    NetCapNone   = 0u,
    NetCapSta    = 1u << 0,
    NetCapAp     = 1u << 1,
    NetCapScan   = 1u << 2,
    NetCapEvents = 1u << 3,
};

class INetwork {
public:
    virtual ~INetwork() = default;

    virtual Status init(const NetworkConfig& cfg) noexcept = 0;
    virtual Status reconfigure(const NetworkConfig& cfg) noexcept {
        (void)cfg; return Status::NotSupported;
    }
    virtual Status connect() noexcept = 0;
    virtual Status disconnect() noexcept = 0;
    virtual Status start_ap(const NetworkConfig& cfg) noexcept = 0;
    virtual Status scan(ScanResult* out, Size max, Size& found) noexcept = 0;
    virtual NetState state() const noexcept = 0;
    virtual Status subscribe(NetEventCallback cb, void* ctx) noexcept = 0;
    virtual uint32_t caps() const noexcept = 0;
};

} // namespace embed::hal
