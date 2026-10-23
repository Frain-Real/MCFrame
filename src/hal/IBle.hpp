// hal/IBle.hpp
// ---------------------------------------------------------------------------
// BLE как интерфейс. Режим выбирается приложением:
//   Peripheral  — GATT-server + advertising;
//   Observer    — сканирование эфира;
//   Broadcaster — non-connectable advertising;
//   Central     — подключение к чужому peripheral (заготовка).
// NimBLE на ESP32-C3.
// ---------------------------------------------------------------------------
#pragma once

#include "os/common.hpp"

namespace embed::hal {

enum class BleRole : uint8_t { Peripheral, Central, Observer, Broadcaster };

struct BleConfig {
    const char* name = nullptr;
    uint16_t    mtu  = 247u;
};

struct BleAddr {
    uint8_t bytes[6]{};
};

struct BleAdvData {
    const uint8_t* data = nullptr;
    Size           len  = 0u;
};

struct BleUuid {
    uint8_t bytes[16]{};
};

struct BleScanResult {
    BleAddr addr{};
    int8_t  rssi = 0;
    char    name[32]{};
};

struct BleEvent;
using BleEventCallback = void (*)(void* ctx, const BleEvent& event);
using BleScanCallback = void (*)(void* ctx, const BleScanResult& result);

struct BleGattChar {
    BleUuid uuid{};
    uint8_t properties = 0u;   // bit0 read, bit1 write, bit2 notify
    uint16_t handle = 0u;      // заполняет реализация
};

struct BleGattService {
    BleUuid      uuid{};
    uint16_t     start_handle = 0u;
    uint16_t     end_handle = 0u;
    BleGattChar  chars[4]{};
    uint8_t      count = 0u;
};

using BleGattServiceCallback = void (*)(void* ctx, const BleGattService& svc);
using BleGattCharCallback = void (*)(void* ctx, const BleGattChar& chr);
using BleGattReadCallback = void (*)(void* ctx, uint16_t handle, Status st,
                                     const void* data, Size len);
using BleGattWriteCallback = void (*)(void* ctx, uint16_t handle, Status st);
using BleGattNotifyCallback = void (*)(void* ctx, uint16_t handle,
                                       const void* data, Size len);

using BleWriteCallback = void (*)(void* ctx, uint16_t attr_handle,
                                  const void* data, Size len);

struct BleEvent {
    enum class Kind : uint8_t { Connected, Disconnected, NotificationSent,
                                ScanDone, Error } kind = Kind::Error;
    uint16_t conn_handle = 0u;
    int32_t  code = 0;
};

enum BleCaps : uint32_t {
    BleCapNone      = 0u,
    BleCapAdv       = 1u << 0,
    BleCapGatt      = 1u << 1,
    BleCapNotify    = 1u << 2,
    BleCapEvents    = 1u << 3,
    BleCapScan      = 1u << 4,
    BleCapBroadcast = 1u << 5,
    BleCapCentral   = 1u << 6,
};

class IBle {
public:
    virtual ~IBle() = default;

    virtual Status init(const BleConfig& cfg) noexcept = 0;
    virtual Status set_role(BleRole role) noexcept { (void)role; return Status::NotSupported; }
    virtual Status advertise(const BleAdvData& adv) noexcept = 0;
    virtual Status stop_advertise() noexcept = 0;
    virtual Status add_service(const BleUuid& uuid) noexcept = 0;
    virtual Status add_gatt_service(const BleGattService& svc) noexcept {
        (void)svc; return Status::NotSupported;
    }
    virtual Status notify(uint16_t conn_handle, uint16_t attr_handle,
                          const void* data, Size len) noexcept = 0;
    virtual Status subscribe(BleEventCallback cb, void* ctx) noexcept = 0;
    virtual Status scan_start(BleScanCallback cb, void* ctx) noexcept {
        (void)cb; (void)ctx; return Status::NotSupported;
    }
    virtual Status scan_stop() noexcept { return Status::NotSupported; }
    virtual Status connect(const BleAddr& addr) noexcept {
        (void)addr; return Status::NotSupported;
    }
    virtual Status disconnect(uint16_t conn_handle) noexcept {
        (void)conn_handle; return Status::NotSupported;
    }
    virtual Status on_write(BleWriteCallback cb, void* ctx) noexcept {
        (void)cb; (void)ctx; return Status::NotSupported;
    }
    /// GATT client: discovery сервисов (характеристики — по handle).
    virtual Status discover_services(uint16_t conn,
                                     BleGattServiceCallback cb, void* ctx) noexcept {
        (void)conn; (void)cb; (void)ctx; return Status::NotSupported;
    }
    virtual Status discover_chars(uint16_t conn, uint16_t start_handle,
                                  uint16_t end_handle, BleGattCharCallback cb,
                                  void* ctx) noexcept {
        (void)conn; (void)start_handle; (void)end_handle; (void)cb; (void)ctx;
        return Status::NotSupported;
    }
    virtual Status read_char(uint16_t conn, uint16_t handle,
                             BleGattReadCallback cb, void* ctx) noexcept {
        (void)conn; (void)handle; (void)cb; (void)ctx; return Status::NotSupported;
    }
    virtual Status write_char(uint16_t conn, uint16_t handle,
                              const void* data, Size len, bool with_response,
                              BleGattWriteCallback cb, void* ctx) noexcept {
        (void)conn; (void)handle; (void)data; (void)len; (void)with_response;
        (void)cb; (void)ctx;
        return Status::NotSupported;
    }
    /// Включить/выключить notify через CCCD (0x2902). Колбэк получает кадры
    /// BLE_GAP_EVENT_NOTIFY_RX; discovery CCCD выполняется асинхронно.
    virtual Status subscribe_notify(uint16_t conn, uint16_t char_handle,
                                    bool enable, BleGattNotifyCallback cb,
                                    void* ctx) noexcept {
        (void)conn; (void)char_handle; (void)enable; (void)cb; (void)ctx;
        return Status::NotSupported;
    }
    virtual uint32_t caps() const noexcept = 0;
};

} // namespace embed::hal
