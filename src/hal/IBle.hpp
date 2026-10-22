// hal/IBle.hpp
// ---------------------------------------------------------------------------
// BLE как интерфейс. Phase 1 — минимальный GATT-server API; расширяется в
// Phase 5 (NimBLE на C3).
// ---------------------------------------------------------------------------
#pragma once

#include "os/common.hpp"

namespace embed::hal {

struct BleConfig {
    const char* name = nullptr;
};

struct BleAdvData {
    const uint8_t* data = nullptr;
    Size           len  = 0u;
};

struct BleUuid {
    uint8_t bytes[16]{};
};

struct BleEvent {
    enum class Kind : uint8_t { Connected, Disconnected, NotificationSent, Error } kind;
    uint16_t conn_handle = 0u;
    int32_t  code = 0;
};

using BleEventCallback = void (*)(void* ctx, const BleEvent& event);

enum BleCaps : uint32_t {
    BleCapNone    = 0u,
    BleCapAdv     = 1u << 0,
    BleCapGatt    = 1u << 1,
    BleCapNotify  = 1u << 2,
    BleCapEvents  = 1u << 3,
};

class IBle {
public:
    virtual ~IBle() = default;

    virtual Status init(const BleConfig& cfg) noexcept = 0;
    virtual Status advertise(const BleAdvData& adv) noexcept = 0;
    virtual Status stop_advertise() noexcept = 0;
    virtual Status add_service(const BleUuid& uuid) noexcept = 0;
    virtual Status notify(uint16_t conn_handle, uint16_t attr_handle,
                          const void* data, Size len) noexcept = 0;
    virtual Status subscribe(BleEventCallback cb, void* ctx) noexcept = 0;
    virtual uint32_t caps() const noexcept = 0;
};

} // namespace embed::hal
