// hal/mock/mock_hal.hpp
// ---------------------------------------------------------------------------
// Mock-реализации HAL: приложение и драйверы тестируются без платы.
// Это не «пустышки»: SPI делает loopback, GPIO помнит уровень и умеет
// дёргать IRQ, NVS/Flash хранят данные, Network/BLE отдают события.
// ---------------------------------------------------------------------------
#pragma once

#include "../IGpio.hpp"
#include "../ISpi.hpp"
#include "../IPwm.hpp"
#include "../IUart.hpp"
#include "../II2c.hpp"
#include "../IAdc.hpp"
#include "../IFlash.hpp"
#include "../INvs.hpp"
#include "../INetwork.hpp"
#include "../IBle.hpp"

namespace embed::hal::mock {

inline bool str_eq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return a == b;
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return *a == *b;
}

inline Size str_len(const char* s) noexcept {
    Size n = 0u;
    if (s != nullptr) while (s[n] != '\0') ++n;
    return n;
}

class MockGpioPin final : public IGpioPin {
public:
    Status configure(const GpioConfig& cfg) noexcept override {
        cfg_ = cfg;
        level_ = cfg.initial;
        configured_ = true;
        return Status::Ok;
    }
    void set(bool level) noexcept override { level_ = level; }
    bool get() const noexcept override { return level_; }
    void toggle() noexcept override { level_ = !level_; }

    Status attach_irq(GpioEdge edge, GpioIrqCallback cb, void* ctx) noexcept override {
        edge_ = edge;
        cb_ = cb;
        ctx_ = ctx;
        return Status::Ok;
    }
    void detach_irq() noexcept override { cb_ = nullptr; ctx_ = nullptr; }

    // Тестовый триггер: как будто пришёл фронт.
    void trigger() noexcept { if (cb_ != nullptr) cb_(ctx_); }

    bool configured() const noexcept { return configured_; }
    const GpioConfig& config() const noexcept { return cfg_; }

private:
    GpioConfig cfg_{};
    bool level_ = false;
    bool configured_ = false;
    GpioEdge edge_ = GpioEdge::Any;
    GpioIrqCallback cb_ = nullptr;
    void* ctx_ = nullptr;
};

class MockSpi final : public ISpi {
public:
    Status configure(const SpiConfig& cfg) noexcept override {
        cfg_ = cfg;
        configured_ = true;
        return Status::Ok;
    }

    Status transfer(const void* tx, void* rx, Size n, Timeout) noexcept override {
        if (!configured_) return Status::Unavailable;
        if (n != 0u && tx == nullptr && rx == nullptr) return Status::InvalidArg;
        const uint8_t* t = static_cast<const uint8_t*>(tx);
        uint8_t* r = static_cast<uint8_t*>(rx);
        for (Size i = 0u; i < n; ++i) {
            if (r != nullptr) r[i] = (t != nullptr) ? t[i] : 0u;
        }
        last_n_ = n;
        ++transfers_;
        return Status::Ok;
    }

    Status lock(Timeout) noexcept override { ++locks_; return Status::Ok; }
    void unlock() noexcept override { ++unlocks_; }
    uint32_t caps() const noexcept override { return SpiCapIrq | SpiCapDma | SpiCapCs; }

    Size last_n() const noexcept { return last_n_; }
    uint32_t transfers() const noexcept { return transfers_; }
    uint32_t locks() const noexcept { return locks_; }
    uint32_t unlocks() const noexcept { return unlocks_; }

private:
    SpiConfig cfg_{};
    bool configured_ = false;
    Size last_n_ = 0u;
    uint32_t transfers_ = 0u;
    uint32_t locks_ = 0u;
    uint32_t unlocks_ = 0u;
};

class MockPwm final : public IPwm {
public:
    Status configure(const PwmConfig& cfg) noexcept override {
        cfg_ = cfg;
        configured_ = true;
        return Status::Ok;
    }
    Status set_duty(uint8_t duty) noexcept override {
        if (duty > 100u) return Status::InvalidArg;
        cfg_.duty_percent = duty;
        return Status::Ok;
    }
    Status start() noexcept override { started_ = true; return Status::Ok; }
    Status stop() noexcept override { started_ = false; return Status::Ok; }
    uint32_t caps() const noexcept override { return 1u; }

    uint8_t duty() const noexcept { return cfg_.duty_percent; }
    bool started() const noexcept { return started_; }
    const PwmConfig& config() const noexcept { return cfg_; }

private:
    PwmConfig cfg_{};
    bool configured_ = false;
    bool started_ = false;
};

class MockUart final : public IUart {
public:
    Status configure(const UartConfig& cfg) noexcept override {
        cfg_ = cfg;
        return Status::Ok;
    }
    Status open() noexcept override { open_ = true; return Status::Ok; }
    Status close() noexcept override { open_ = false; return Status::Ok; }

    Status read(void* buffer, Size capacity, Size& bytes_read, Timeout) noexcept override {
        bytes_read = 0u;
        if (!open_) return Status::Unavailable;
        if (buffer == nullptr || capacity == 0u) return Status::InvalidArg;
        if (rx_count_ == 0u) return Status::Timeout;
        Size n = (capacity < rx_count_) ? capacity : rx_count_;
        auto* out = static_cast<uint8_t*>(buffer);
        for (Size i = 0u; i < n; ++i) out[i] = rx_[(rx_head_ + i) % kBuf];
        rx_head_ = (rx_head_ + n) % kBuf;
        rx_count_ -= n;
        bytes_read = n;
        return Status::Ok;
    }

    Status write(const void* data, Size len, Size& written, Timeout) noexcept override {
        written = 0u;
        if (!open_) return Status::Unavailable;
        if (data == nullptr || len == 0u) return Status::InvalidArg;
        const auto* in = static_cast<const uint8_t*>(data);
        for (Size i = 0u; i < len; ++i) {
            if (tx_count_ >= kBuf) return Status::NoMemory;
            tx_[(tx_head_ + tx_count_) % kBuf] = in[i];
            ++tx_count_;
            ++written;
        }
        return Status::Ok;
    }

    void push_rx(const void* data, Size len) noexcept {
        const auto* in = static_cast<const uint8_t*>(data);
        for (Size i = 0u; i < len && rx_count_ < kBuf; ++i) {
            rx_[(rx_head_ + rx_count_) % kBuf] = in[i];
            ++rx_count_;
        }
    }

    Size tx_size() const noexcept { return tx_count_; }
    uint8_t tx_at(Size i) const noexcept { return tx_[(tx_head_ + i) % kBuf]; }

private:
    static constexpr Size kBuf = 256u;
    UartConfig cfg_{};
    bool open_ = false;
    uint8_t rx_[kBuf]{};
    uint8_t tx_[kBuf]{};
    Size rx_head_ = 0u;
    Size rx_count_ = 0u;
    Size tx_head_ = 0u;
    Size tx_count_ = 0u;
};

class MockI2c final : public II2c {
public:
    Status configure(const I2cConfig& cfg) noexcept override { cfg_ = cfg; return Status::Ok; }
    Status write(uint8_t address, const void* data, Size len, Timeout) noexcept override {
        if (data == nullptr || len == 0u) return Status::InvalidArg;
        const auto* in = static_cast<const uint8_t*>(data);
        for (Size i = 0u; i < len && i < sizeof(buf_); ++i) buf_[i] = in[i];
        last_len_ = len;
        last_address_ = address;
        return Status::Ok;
    }
    Status read(uint8_t address, void* buffer, Size capacity, Size& bytes_read, Timeout) noexcept override {
        if (buffer == nullptr || capacity == 0u) return Status::InvalidArg;
        bytes_read = (capacity < last_len_) ? capacity : last_len_;
        auto* out = static_cast<uint8_t*>(buffer);
        for (Size i = 0u; i < bytes_read; ++i) out[i] = buf_[i];
        last_address_ = address;
        return Status::Ok;
    }
    Status lock(Timeout) noexcept override { return Status::Ok; }
    void unlock() noexcept override {}

private:
    I2cConfig cfg_{};
    uint8_t buf_[64]{};
    Size last_len_ = 0u;
    uint8_t last_address_ = 0u;
};

class MockAdc final : public IAdc {
public:
    Status configure(const AdcConfig& cfg) noexcept override { cfg_ = cfg; return Status::Ok; }
    Status read(uint8_t channel, uint16_t& value) noexcept override {
        value = static_cast<uint16_t>(channel * 100u);
        return Status::Ok;
    }
private:
    AdcConfig cfg_{};
};

class MockFlash final : public IFlash {
public:
    uint32_t page_size() const noexcept override { return 64u; }
    bool is_volatile() const noexcept override { return false; }

    Status read(uint32_t address, void* buffer, Size len) noexcept override {
        if (buffer == nullptr && len != 0u) return Status::InvalidArg;
        if (address + len > sizeof(data_)) return Status::InvalidArg;
        auto* out = static_cast<uint8_t*>(buffer);
        for (Size i = 0u; i < len; ++i) out[i] = data_[address + i];
        return Status::Ok;
    }
    Status write(uint32_t address, const void* data, Size len) noexcept override {
        if (data == nullptr && len != 0u) return Status::InvalidArg;
        if (address + len > sizeof(data_)) return Status::InvalidArg;
        const auto* in = static_cast<const uint8_t*>(data);
        for (Size i = 0u; i < len; ++i) data_[address + i] = in[i];
        return Status::Ok;
    }
    Status erase(uint32_t address, Size len) noexcept override {
        if (address + len > sizeof(data_)) return Status::InvalidArg;
        for (Size i = 0u; i < len; ++i) data_[address + i] = 0xFFu;
        return Status::Ok;
    }

private:
    uint8_t data_[1024]{};
};

class MockNvs final : public INvs {
public:
    Status init() noexcept override { initialized_ = true; return Status::Ok; }
    Status read(const char* key, void* buffer, Size capacity, Size& len) noexcept override {
        len = 0u;
        Entry* e = find(key);
        if (e == nullptr) return Status::NotFound;
        if (buffer == nullptr || capacity < e->len) return Status::InvalidArg;
        auto* out = static_cast<uint8_t*>(buffer);
        for (Size i = 0u; i < e->len; ++i) out[i] = e->data[i];
        len = e->len;
        return Status::Ok;
    }
    Status write(const char* key, const void* data, Size len) noexcept override {
        if (key == nullptr || data == nullptr || len > 32u) return Status::InvalidArg;
        Entry* e = find(key);
        if (e == nullptr) e = alloc(key);
        if (e == nullptr) return Status::NoMemory;
        const auto* in = static_cast<const uint8_t*>(data);
        for (Size i = 0u; i < len; ++i) e->data[i] = in[i];
        e->len = len;
        return Status::Ok;
    }
    Status erase(const char* key) noexcept override {
        Entry* e = find(key);
        if (e == nullptr) return Status::NotFound;
        e->used = false;
        e->len = 0u;
        return Status::Ok;
    }

private:
    struct Entry {
        char key[16]{};
        uint8_t data[32]{};
        Size len = 0u;
        bool used = false;
    };

    Entry* find(const char* key) noexcept {
        if (key == nullptr) return nullptr;
        for (auto& e : entries_) if (e.used && str_eq(e.key, key)) return &e;
        return nullptr;
    }
    Entry* alloc(const char* key) noexcept {
        for (auto& e : entries_) {
            if (!e.used) {
                e.used = true;
                e.len = 0u;
                Size i = 0u;
                for (; key[i] != '\0' && i + 1u < sizeof(e.key); ++i) e.key[i] = key[i];
                e.key[i] = '\0';
                return &e;
            }
        }
        return nullptr;
    }

    Entry entries_[8]{};
    bool initialized_ = false;
};

class MockNetwork final : public INetwork {
public:
    Status init(const NetworkConfig& cfg) noexcept override {
        cfg_ = cfg;
        state_ = NetState::Idle;
        return Status::Ok;
    }
    Status connect() noexcept override {
        state_ = NetState::Connected;
        NetEvent e{NetEvent::Kind::Connected, 0};
        if (cb_ != nullptr) cb_(ctx_, e);
        NetEvent ip{NetEvent::Kind::GotIp, 0};
        if (cb_ != nullptr) cb_(ctx_, ip);
        return Status::Ok;
    }
    Status disconnect() noexcept override {
        state_ = NetState::Idle;
        NetEvent e{NetEvent::Kind::Disconnected, 0};
        if (cb_ != nullptr) cb_(ctx_, e);
        return Status::Ok;
    }
    Status start_ap(const NetworkConfig& cfg) noexcept override {
        cfg_ = cfg;
        state_ = NetState::Ap;
        return Status::Ok;
    }
    Status scan(ScanResult* out, Size max, Size& found) noexcept override {
        (void)out;
        (void)max;
        found = 0u;
        NetEvent e{NetEvent::Kind::ScanDone, 0};
        if (cb_ != nullptr) cb_(ctx_, e);
        return Status::Ok;
    }
    NetState state() const noexcept override { return state_; }
    Status subscribe(NetEventCallback cb, void* ctx) noexcept override {
        cb_ = cb;
        ctx_ = ctx;
        return Status::Ok;
    }
    uint32_t caps() const noexcept override {
        return NetCapSta | NetCapAp | NetCapScan | NetCapEvents;
    }

private:
    NetworkConfig cfg_{};
    NetState state_ = NetState::Off;
    NetEventCallback cb_ = nullptr;
    void* ctx_ = nullptr;
};

class MockBle final : public IBle {
public:
    Status init(const BleConfig& cfg) noexcept override {
        cfg_ = cfg;
        ready_ = true;
        return Status::Ok;
    }
    Status advertise(const BleAdvData& adv) noexcept override {
        if (!ready_) return Status::Unavailable;
        adv_ = adv;
        advertising_ = true;
        return Status::Ok;
    }
    Status stop_advertise() noexcept override {
        advertising_ = false;
        return Status::Ok;
    }
    Status add_service(const BleUuid& uuid) noexcept override {
        service_ = uuid;
        has_service_ = true;
        return Status::Ok;
    }
    Status notify(uint16_t conn_handle, uint16_t attr_handle,
                  const void* data, Size len) noexcept override {
        if (!ready_ || data == nullptr) return Status::InvalidArg;
        last_conn_ = conn_handle;
        last_attr_ = attr_handle;
        last_len_ = len;
        BleEvent e{BleEvent::Kind::NotificationSent, conn_handle, 0};
        if (cb_ != nullptr) cb_(ctx_, e);
        return Status::Ok;
    }
    Status subscribe(BleEventCallback cb, void* ctx) noexcept override {
        cb_ = cb;
        ctx_ = ctx;
        return Status::Ok;
    }
    uint32_t caps() const noexcept override {
        return BleCapAdv | BleCapGatt | BleCapNotify | BleCapEvents;
    }

    bool advertising() const noexcept { return advertising_; }
    bool has_service() const noexcept { return has_service_; }
    uint16_t last_conn() const noexcept { return last_conn_; }
    uint16_t last_attr() const noexcept { return last_attr_; }
    Size last_len() const noexcept { return last_len_; }

private:
    BleConfig cfg_{};
    BleAdvData adv_{};
    BleUuid service_{};
    bool ready_ = false;
    bool advertising_ = false;
    bool has_service_ = false;
    uint16_t last_conn_ = 0u;
    uint16_t last_attr_ = 0u;
    Size last_len_ = 0u;
    BleEventCallback cb_ = nullptr;
    void* ctx_ = nullptr;
};

} // namespace embed::hal::mock
