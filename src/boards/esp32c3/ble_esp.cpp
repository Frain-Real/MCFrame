#include "boards/esp32c3/ble_esp.hpp"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"
#include <cstring>
#include <cstdint>

namespace embed::esp32c3 {
using hal::BleCapAdv;
using hal::BleCapBroadcast;
using hal::BleCapCentral;
using hal::BleAddr;
using hal::BleCapEvents;
using hal::BleCapGatt;
using hal::BleCapNotify;
using hal::BleCapScan;
using hal::BleScanResult;
namespace {

constexpr char kTag[] = "ble";
BleEsp* g_ble = nullptr;

const ble_uuid128_t kSvcUuid = BLE_UUID128_INIT(
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x6d,0x63,0x66,0x72);
const ble_uuid128_t kSsidUuid = BLE_UUID128_INIT(
    0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x6d,0x63,0x66,0x72);
const ble_uuid128_t kPassUuid = BLE_UUID128_INIT(
    0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x6d,0x63,0x66,0x72);
const ble_uuid128_t kCmdUuid = BLE_UUID128_INIT(
    0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x6d,0x63,0x66,0x72);
const ble_uuid128_t kStatusUuid = BLE_UUID128_INIT(
    0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x6d,0x63,0x66,0x72);

uint16_t h_ssid = 0u;
uint16_t h_pass = 0u;
uint16_t h_cmd = 0u;
uint16_t h_status = 0u;

void copy_str(char* dst, Size cap, const char* src) noexcept {
    if (dst == nullptr || cap == 0u) return;
    Size i = 0u;
    if (src != nullptr) while (src[i] != '\0' && i + 1u < cap) { dst[i] = src[i]; ++i; }
    dst[i] = '\0';
}

} // namespace

Status BleEsp::init(const BleConfig& cfg) noexcept {
    if (inited_) return Status::AlreadyExists;
    copy_str(name_, sizeof(name_), cfg.name != nullptr ? cfg.name : "MCFrame");
    g_ble = this;

    queue_ = xQueueCreateStatic(8u, sizeof(BleEvent), reinterpret_cast<uint8_t*>(queue_buf_),
                                &queue_storage_);
    if (queue_ == nullptr) return Status::NoMemory;
    task_ = xTaskCreateStatic(&BleEsp::dispatch_task, "ble-ev", 4096u, this, 9u,
                              task_stack_, &task_storage_);
    if (task_ == nullptr) return Status::NoMemory;

    if (nimble_port_init() != ESP_OK) return Status::HardwareError;
    ble_hs_cfg.sync_cb = &BleEsp::on_sync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_legacy = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    static struct ble_gatt_chr_def chars[5]{};
    chars[0].uuid = &kSsidUuid.u;   chars[0].access_cb = &BleEsp::gatt_access;
    chars[0].arg = reinterpret_cast<void*>(1); chars[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
    chars[0].val_handle = &h_ssid;
    chars[1].uuid = &kPassUuid.u;   chars[1].access_cb = &BleEsp::gatt_access;
    chars[1].arg = reinterpret_cast<void*>(2); chars[1].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
    chars[1].val_handle = &h_pass;
    chars[2].uuid = &kCmdUuid.u;    chars[2].access_cb = &BleEsp::gatt_access;
    chars[2].arg = reinterpret_cast<void*>(3); chars[2].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
    chars[2].val_handle = &h_cmd;
    chars[3].uuid = &kStatusUuid.u; chars[3].access_cb = &BleEsp::gatt_access;
    chars[3].arg = reinterpret_cast<void*>(4);
    chars[3].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY;
    chars[3].val_handle = &h_status;
    static struct ble_gatt_svc_def svcs[2]{};
    svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    svcs[0].uuid = &kSvcUuid.u;
    svcs[0].characteristics = chars;
    if (ble_gatts_count_cfg(svcs) != 0) return Status::HardwareError;
    if (ble_gatts_add_svcs(svcs) != 0) return Status::HardwareError;

    nimble_port_freertos_init(&BleEsp::host_task);
    inited_ = true;
    return Status::Ok;
}

void BleEsp::host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void BleEsp::on_sync() {
    if (g_ble == nullptr) return;
    (void)ble_svc_gap_device_name_set(g_ble->name_);
    if (g_ble->role_ == BleRole::Peripheral) {
        (void)g_ble->advertise(BleAdvData{});
    }
}

Status BleEsp::set_role(BleRole role) noexcept {
    if (!inited_) return Status::Unavailable;
    role_ = role;
    if (role == BleRole::Central) {
        (void)stop_advertise();
        (void)scan_stop();
        return Status::Ok;
    }
    if (role == BleRole::Observer) {
        (void)stop_advertise();
        return scan_start(scan_cb_, scan_ctx_);
    }
    (void)scan_stop();
    return advertise(BleAdvData{});
}

Status BleEsp::advertise(const BleAdvData&) noexcept {
    if (!inited_) return Status::Unavailable;
    struct ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = reinterpret_cast<const uint8_t*>(name_);
    fields.name_len = static_cast<uint8_t>(std::strlen(name_));
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return Status::HardwareError;

    struct ble_gap_adv_params params{};
    params.conn_mode = (role_ == BleRole::Broadcaster)
        ? BLE_GAP_CONN_MODE_NON : BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER,
                           &params, &BleEsp::gap_event, this);
    if (rc != 0 && rc != BLE_HS_EALREADY) return Status::HardwareError;
    advertising_ = true;
    return Status::Ok;
}

Status BleEsp::stop_advertise() noexcept {
    if (!inited_) return Status::Unavailable;
    (void)ble_gap_adv_stop();
    advertising_ = false;
    return Status::Ok;
}

Status BleEsp::add_service(const BleUuid&) noexcept { return inited_ ? Status::Ok : Status::Unavailable; }
Status BleEsp::add_gatt_service(const BleGattService&) noexcept { return Status::NotSupported; }

Status BleEsp::notify(uint16_t conn, uint16_t attr, const void* data, Size len) noexcept {
    if (!inited_ || data == nullptr) return Status::InvalidArg;
    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, static_cast<uint16_t>(len));
    if (om == nullptr) return Status::NoMemory;
    const int rc = ble_gatts_notify_custom(conn, attr, om);
    if (rc != 0) return Status::HardwareError;
    BleEvent e{BleEvent::Kind::NotificationSent, conn, 0};
    post(e);
    return Status::Ok;
}

Status BleEsp::subscribe(BleEventCallback cb, void* ctx) noexcept {
    cb_ = cb; cb_ctx_ = ctx; return Status::Ok;
}
Status BleEsp::connect(const BleAddr& addr) noexcept {
    if (!inited_) return Status::Unavailable;
    ble_addr_t peer{};
    peer.type = BLE_ADDR_PUBLIC;
    std::memcpy(peer.val, addr.bytes, 6);
    struct ble_gap_conn_params params{};
    params.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
    params.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
    params.latency = 0;
    params.supervision_timeout = 0x0100;
    params.min_ce_len = 0;
    params.max_ce_len = 0;
    role_ = BleRole::Central;
    const int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, 10000, &params,
                                   &BleEsp::gap_event, this);
    return rc == 0 ? Status::Ok : Status::HardwareError;
}
Status BleEsp::disconnect(uint16_t conn) noexcept {
    if (!inited_) return Status::Unavailable;
    return ble_gap_terminate(conn != 0u ? conn : conn_, BLE_ERR_REM_USER_CONN_TERM) == 0
        ? Status::Ok : Status::HardwareError;
}

Status BleEsp::on_write(BleWriteCallback cb, void* ctx) noexcept {
    write_cb_ = cb; write_ctx_ = ctx; return Status::Ok;
}

Status BleEsp::scan_start(BleScanCallback cb, void* ctx) noexcept {
    if (!inited_) return Status::Unavailable;
    scan_cb_ = cb; scan_ctx_ = ctx;
    struct ble_gap_disc_params params{};
    params.passive = 0;
    params.filter_duplicates = 1;
    const int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params,
                                &BleEsp::gap_event, this);
    return (rc == 0 || rc == BLE_HS_EALREADY) ? Status::Ok : Status::HardwareError;
}
Status BleEsp::scan_stop() noexcept {
    if (!inited_) return Status::Unavailable;
    (void)ble_gap_disc_cancel();
    return Status::Ok;
}
uint32_t BleEsp::caps() const noexcept {
    return BleCapAdv | BleCapGatt | BleCapNotify | BleCapEvents |
           BleCapScan | BleCapBroadcast | BleCapCentral;
}

void BleEsp::post(const BleEvent& e) noexcept {
    if (queue_ != nullptr) (void)xQueueSend(queue_, &e, 0u);
}
void BleEsp::dispatch_task(void* arg) {
    auto* self = static_cast<BleEsp*>(arg);
    BleEvent e{};
    for (;;) {
        if (xQueueReceive(self->queue_, &e, portMAX_DELAY) == pdTRUE) {
            if (self->cb_ != nullptr) self->cb_(self->cb_ctx_, e);
        }
    }
}

int BleEsp::gatt_access(uint16_t conn, uint16_t, ::ble_gatt_access_ctxt* ctxt,
                        void* arg) {
    auto* self = g_ble;
    const int id = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    if (self == nullptr || ctxt == nullptr) return BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        char tmp[65]{};
        const uint16_t n = (len < sizeof(tmp) - 1u) ? len : sizeof(tmp) - 1u;
        if (os_mbuf_copydata(ctxt->om, 0, n, tmp) != 0) return BLE_ATT_ERR_UNLIKELY;
        tmp[n] = '\0';
        if (id == 1) copy_str(self->ssid_, sizeof(self->ssid_), tmp);
        else if (id == 2) copy_str(self->pass_, sizeof(self->pass_), tmp);
        if (self->write_cb_ != nullptr) self->write_cb_(self->write_ctx_, id, tmp, n);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && id == 4) {
        const char* ok = "ok";
        return os_mbuf_append(ctxt->om, ok, 2u) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    (void)conn;
    return BLE_ATT_ERR_UNLIKELY;
}

int BleEsp::gap_event(::ble_gap_event* event, void* arg) {
    auto* self = static_cast<BleEsp*>(arg);
    if (self == nullptr) return 0;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                self->conn_ = event->connect.conn_handle;
                BleEvent e{BleEvent::Kind::Connected, self->conn_, 0};
                self->post(e);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT: {
            self->conn_ = 0u;
            BleEvent e{BleEvent::Kind::Disconnected, event->disconnect.conn.conn_handle, 0};
            self->post(e);
            break;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE:
            self->advertising_ = false;
            break;
        case BLE_GAP_EVENT_DISC: {
            BleScanResult r{};
            std::memcpy(r.addr.bytes, event->disc.addr.val, 6);
            r.rssi = event->disc.rssi;
            const uint8_t* d = event->disc.data;
            const uint8_t n = event->disc.length_data;
            uint8_t i = 0u;
            while (i + 1u < n) {
                const uint8_t len = d[i];
                if (len == 0u || i + len >= n) break;
                const uint8_t type = d[i + 1u];
                if ((type == 0x08u || type == 0x09u) && len > 1u) {
                    const uint8_t sl = (len - 1u < sizeof(r.name) - 1u) ? len - 1u : sizeof(r.name) - 1u;
                    std::memcpy(r.name, &d[i + 2u], sl);
                    r.name[sl] = '\0';
                }
                i = static_cast<uint8_t>(i + len + 1u);
            }
            if (self->scan_cb_ != nullptr) self->scan_cb_(self->scan_ctx_, r);
            break;
        }
        default: break;
    }
    return 0;
}

} // namespace embed::esp32c3
