// os/usb/rndis.hpp
// ---------------------------------------------------------------------------
// RNDIS device (Remote NDIS) — USB Ethernet для Windows/Linux.
//
// Класс отдаёт и принимает Ethernet frames через bulk endpoints. Сам по себе
// RNDIS не является TCP/IP-стеком: для MQTT/HTTP поверх него нужен lwIP или
// другой IP-стек. Хук FrameSink/ответы control — ниже.
// ---------------------------------------------------------------------------
#pragma once

#include "usb.hpp"

namespace embed::usb::rndis {

struct Config {
    uint8_t mac[6] = {0x02, 0x4D, 0x43, 0x00, 0x00, 0x02};
    uint16_t mtu = 1500u;
};

/// Получатель Ethernet-кадров от хоста (например, lwIP netif).
using FrameSink = void (*)(void* ctx, const uint8_t* frame, Size len) noexcept;

class Rndis final : public Class {
public:
    static constexpr uint8_t kEpNotify = 0x81u;
    static constexpr uint8_t kEpOut = 0x02u;
    static constexpr uint8_t kEpIn  = 0x83u;
    static constexpr Size kMaxFrame = 1536u;

    explicit Rndis(const Config& cfg = Config{}) noexcept : cfg_(cfg) {}

    const uint8_t* config_descriptor(Size& len) const noexcept override {
        len = sizeof(config_);
        return config_;
    }

    Status control(const Setup& s, const uint8_t* out, Size out_len,
                   const uint8_t*& in, Size& in_len) noexcept override {
        in = nullptr;
        in_len = 0u;
        if (s.request == 0x00u) {                 // SEND_ENCAPSULATED_COMMAND
            return handle_command(out, out_len);
        }
        if (s.request == 0x01u) {                 // GET_ENCAPSULATED_RESPONSE
            if (response_len_ == 0u) return Status::NotFound;
            in = response_;
            in_len = response_len_;
            response_len_ = 0u;
            return Status::Ok;
        }
        if (s.request == 0x02u) {                 // RESET
            reset_state();
            return Status::Ok;
        }
        return Status::NotSupported;
    }

    /// Ethernet-кадр от хоста (bulk OUT). Возврат NoMemory — очередь полна.
    Status receive(const uint8_t* data, Size len) noexcept {
        if (data == nullptr || len < 44u) return Status::InvalidArg;
        const uint32_t type = rd32(data);
        if (type != 0x00000001u) return Status::Corruption;   // PACKET_MSG
        const uint32_t msg_len = rd32(data + 4u);
        const uint32_t data_off = rd32(data + 8u);
        const uint32_t data_len = rd32(data + 12u);
        if (msg_len < 44u || msg_len > len || data_off > msg_len ||
            data_off + data_len > msg_len || data_len > kMaxFrame) {
            return Status::Corruption;
        }
        if (rx_count_ >= kRxDepth) return Status::NoMemory;
        const Size slot = (rx_head_ + rx_count_) % kRxDepth;
        for (uint32_t i = 0u; i < data_len; ++i) {
            rx_[slot][i] = data[data_off + i];
        }
        rx_len_[slot] = data_len;
        ++rx_count_;
        if (sink_ != nullptr) sink_(sink_ctx_, rx_[slot], data_len);
        return Status::Ok;
    }

    /// Кадр для отправки хостом (device→host). Копируется в очередь.
    Status send_frame(const uint8_t* frame, Size len) noexcept {
        if (frame == nullptr || len == 0u || len > kMaxFrame) return Status::InvalidArg;
        if (tx_count_ >= kTxDepth) return Status::NoMemory;
        const Size slot = (tx_head_ + tx_count_) % kTxDepth;
        const Size total = 44u + len;
        for (Size i = 0u; i < 44u; ++i) tx_[slot][i] = 0u;
        put32(tx_[slot], 0x00000001u);
        put32(tx_[slot] + 4u, static_cast<uint32_t>(total));
        put32(tx_[slot] + 8u, 44u);
        put32(tx_[slot] + 12u, static_cast<uint32_t>(len));
        for (Size i = 0u; i < len; ++i) tx_[slot][44u + i] = frame[i];
        tx_len_[slot] = total;
        ++tx_count_;
        return Status::Ok;
    }

    /// Отправить накопленные кадры через bulk IN. Возвращает число кадров.
    Size flush(Hal& hal) noexcept {
        Size done = 0u;
        while (tx_count_ != 0u) {
            const Status st = hal.ep_write(kEpIn, tx_[tx_head_], tx_len_[tx_head_]);
            if (st != Status::Ok) break;
            tx_head_ = (tx_head_ + 1u) % kTxDepth;
            --tx_count_;
            ++done;
        }
        return done;
    }

    void set_sink(FrameSink sink, void* ctx) noexcept { sink_ = sink; sink_ctx_ = ctx; }
    bool link_up() const noexcept { return link_up_; }
    uint8_t packet_filter() const noexcept { return filter_; }

    uint8_t rx_count() const noexcept { return rx_count_; }
    uint8_t tx_count() const noexcept { return tx_count_; }
    const uint8_t* rx_frame(Size& len) const noexcept {
        if (rx_count_ == 0u) { len = 0u; return nullptr; }
        len = rx_len_[rx_head_];
        return rx_[rx_head_];
    }
    void pop_rx() noexcept {
        if (rx_count_ == 0u) return;
        rx_head_ = (rx_head_ + 1u) % kRxDepth;
        --rx_count_;
    }

private:
    static uint32_t rd32(const uint8_t* p) noexcept {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }
    static void put32(uint8_t* p, uint32_t v) noexcept {
        p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
    }

    void reset_state() noexcept {
        response_len_ = 0u;
        rx_head_ = rx_count_ = 0u;
        tx_head_ = tx_count_ = 0u;
        filter_ = 0u;
        link_up_ = true;
    }

    Status handle_command(const uint8_t* data, Size len) noexcept {
        if (data == nullptr || len < 8u) return Status::InvalidArg;
        const uint32_t type = rd32(data);
        const uint32_t msg_len = rd32(data + 4u);
        if (msg_len < 8u || msg_len > len) return Status::Corruption;
        response_len_ = 0u;
        switch (type) {
            case 0x00000002u: {                     // INITIALIZE_MSG
                if (msg_len < 24u) return Status::Corruption;
                const uint32_t req_id = rd32(data + 8u);
                put32(response_, 0x80000002u);      // INITIALIZE_CMPLT
                put32(response_ + 4u, 52u);
                put32(response_ + 8u, req_id);
                put32(response_ + 12u, 0u);         // success
                put32(response_ + 16u, 1u);         // major
                put32(response_ + 20u, 0u);         // minor
                put32(response_ + 24u, 0u);         // device flags
                put32(response_ + 28u, 0u);         // medium
                put32(response_ + 32u, cfg_.mtu);
                put32(response_ + 36u, 1u);         // max packets
                put32(response_ + 40u, 1u);         // max transfer
                put32(response_ + 44u, 0u);
                put32(response_ + 48u, 0u);
                response_len_ = 52u;
                return Status::Ok;
            }
            case 0x00000004u: {                     // QUERY_MSG
                if (msg_len < 20u) return Status::Corruption;
                const uint32_t req_id = rd32(data + 8u);
                const uint32_t oid = rd32(data + 12u);
                uint8_t value[16]{};
                uint32_t vlen = 0u;
                query_oid(oid, value, vlen);
                put32(response_, 0x80000004u);      // QUERY_CMPLT
                put32(response_ + 4u, 16u + vlen);
                put32(response_ + 8u, req_id);
                put32(response_ + 12u, 0u);         // success
                put32(response_ + 16u, vlen);
                for (uint32_t i = 0u; i < vlen; ++i) response_[20u + i] = value[i];
                response_len_ = 16u + vlen;
                return Status::Ok;
            }
            case 0x00000005u: {                     // SET_MSG
                if (msg_len < 20u) return Status::Corruption;
                const uint32_t req_id = rd32(data + 8u);
                const uint32_t oid = rd32(data + 12u);
                const uint32_t vlen = rd32(data + 16u);
                if (20u + vlen > msg_len) return Status::Corruption;
                if (oid == 0x0001010Eu && vlen >= 4u) {   // packet filter
                    filter_ = static_cast<uint8_t>(rd32(data + 20u) & 0xFFu);
                }
                put32(response_, 0x80000005u);      // SET_CMPLT
                put32(response_ + 4u, 16u);
                put32(response_ + 8u, req_id);
                put32(response_ + 12u, 0u);
                response_len_ = 16u;
                return Status::Ok;
            }
            case 0x00000006u: {                     // RESET_MSG
                if (msg_len < 12u) return Status::Corruption;
                const uint32_t req_id = rd32(data + 8u);
                reset_state();
                put32(response_, 0x80000006u);      // RESET_CMPLT
                put32(response_ + 4u, 16u);
                put32(response_ + 8u, req_id);
                put32(response_ + 12u, 0u);
                response_len_ = 16u;
                return Status::Ok;
            }
            case 0x00000008u: {                     // KEEPALIVE_MSG
                if (msg_len < 12u) return Status::Corruption;
                const uint32_t req_id = rd32(data + 8u);
                put32(response_, 0x80000008u);
                put32(response_ + 4u, 16u);
                put32(response_ + 8u, req_id);
                put32(response_ + 12u, 0u);
                response_len_ = 16u;
                return Status::Ok;
            }
            default:
                return Status::NotSupported;
        }
    }

    void query_oid(uint32_t oid, uint8_t* out, uint32_t& len) const noexcept {
        switch (oid) {
            case 0x00010101u:                       // SUPPORTED_LIST
                put32(out, 0x00010106u); put32(out + 4u, 0x00010114u);
                put32(out + 8u, 0x01010102u); len = 12u; break;
            case 0x00010102u: put32(out, 0u); len = 4u; break;       // HW status
            case 0x00010103u:                       // MEDIA_SUPPORTED
            case 0x00010104u: put32(out, 0u); len = 4u; break;       // 802.3
            case 0x00010106u: put32(out, cfg_.mtu); len = 4u; break;
            case 0x00010107u: put32(out, 100000000u); len = 4u; break;
            case 0x00010114u: put32(out, link_up_ ? 0u : 1u); len = 4u; break;
            case 0x0001010Eu: put32(out, filter_); len = 4u; break;
            case 0x01010101u:                       // permanent MAC
            case 0x01010102u:                       // current MAC
                for (uint8_t i = 0u; i < 6u; ++i) out[i] = cfg_.mac[i];
                len = 6u; break;
            case 0x01010103u: put32(out, 1u); len = 4u; break;
            default: put32(out, 0u); len = 4u; break;
        }
    }

    Config cfg_{};
    bool link_up_ = true;
    uint8_t filter_ = 0u;
    uint8_t response_[64]{};
    Size response_len_ = 0u;
    FrameSink sink_ = nullptr;
    void* sink_ctx_ = nullptr;
    static constexpr Size kRxDepth = 4u;
    static constexpr Size kTxDepth = 4u;
    uint8_t rx_[kRxDepth][kMaxFrame]{};
    Size rx_len_[kRxDepth]{};
    Size rx_head_ = 0u;
    uint8_t rx_count_ = 0u;
    uint8_t tx_[kTxDepth][44u + kMaxFrame]{};
    Size tx_len_[kTxDepth]{};
    Size tx_head_ = 0u;
    uint8_t tx_count_ = 0u;

    static constexpr uint8_t config_[67] = {
        9u, 2u, 67u, 0u, 2u, 1u, 0u, 0x80u, 50u,
        9u, 4u, 0u, 0u, 2u, 2u, 1u, 0u, 0u,
        5u, 0x24u, 0x00u, 0x10u, 0x01u,
        5u, 0x24u, 0x01u, 0x00u, 1u,
        4u, 0x24u, 0x02u, 0x02u,
        5u, 0x24u, 0x06u, 0u, 1u,
        7u, 5u, kEpNotify, 3u, 16u, 0u, 16u,
        9u, 4u, 1u, 0u, 0u, 0x0Au, 0u, 0u, 0u,
        7u, 5u, kEpOut, 2u, 64u, 0u, 0u,
        7u, 5u, kEpIn, 2u, 64u, 0u, 0u
    };
};

} // namespace embed::usb::rndis
