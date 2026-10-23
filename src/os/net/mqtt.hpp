// os/net/mqtt.hpp
// ---------------------------------------------------------------------------
// MQTT 3.1.1 client: QoS 0/1/2, keepalive, LWT, reconnect, subscribe.
//
// Клиент синхронный: приложение вызывает connect/publish/poll из своей
// задачи. Все буферы статические, размер задаётся шаблоном Capacity.
// ---------------------------------------------------------------------------
#pragma once

#include "socket.hpp"
#include "../config/time_config.hpp"

namespace embed::net::mqtt {

struct Config {
    const char* host        = nullptr;
    uint16_t    port        = 1883u;
    const char* client_id   = "mcframe";
    const char* username    = nullptr;
    const char* password    = nullptr;
    uint16_t    keepalive_s = 60u;

    const char* will_topic   = nullptr;
    const void* will_payload = nullptr;
    Size        will_len     = 0u;
    uint8_t     will_qos     = 0u;
    bool        will_retain  = false;

    bool        clean_session = true;
    bool        auto_reconnect = true;
    uint32_t    reconnect_backoff_ms = 1000u;
};

struct Message {
    const char* topic   = nullptr;   ///< валиден во время callback
    const uint8_t* payload = nullptr;
    Size        len     = 0u;
    uint8_t     qos     = 0u;
    bool        retain  = false;
};

using MessageCallback = void (*)(void* ctx, const Message& msg);

template <Size Capacity = 768u>
class Client {
public:
    Client() noexcept = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ~Client() noexcept { close(); }

    Status init(Factory& factory, const Config& cfg) noexcept {
        if (cfg.host == nullptr || cfg.client_id == nullptr) return Status::InvalidArg;
        if (cfg.will_qos > 2u) return Status::InvalidArg;
        factory_ = &factory;
        cfg_ = cfg;
        connected_ = false;
        packet_id_ = 0u;
        next_reconnect_ms_ = 0u;
        pending_qos2_ = false;
        return Status::Ok;
    }

    void set_callback(MessageCallback cb, void* ctx) noexcept {
        cb_ = cb;
        cb_ctx_ = ctx;
    }

    bool connected() const noexcept { return connected_; }
    uint16_t packet_id() const noexcept { return packet_id_; }

    Status connect(uint32_t timeout_ms = 5000u) noexcept {
        if (factory_ == nullptr) return Status::Unavailable;
        close();

        Socket* s = nullptr;
        Status st = factory_->create(Protocol::Tcp, s);
        if (st != Status::Ok || s == nullptr) return (st == Status::Ok) ? Status::NoMemory : st;

        Endpoint ep{};
        ep.port = cfg_.port;
        st = factory_->resolve(cfg_.host, ep);
        if (st != Status::Ok) { s->close(); return st; }
        st = s->connect(ep, timeout_ms);
        if (st != Status::Ok) { s->close(); return st; }
        sock_ = s;

        uint8_t body[Capacity]{};
        Size n = 0u;
        n = put_string(body, n, sizeof(body), "MQTT");
        if (n == 0u || n + 4u > sizeof(body)) { close(); return Status::NoMemory; }
        body[n++] = 4u;                       // protocol level 3.1.1
        uint8_t flags = cfg_.clean_session ? 0x02u : 0u;
        const bool has_will = cfg_.will_topic != nullptr;
        if (has_will) {
            flags |= 0x04u;
            flags |= static_cast<uint8_t>((cfg_.will_qos & 0x03u) << 3u);
            if (cfg_.will_retain) flags |= 0x20u;
        }
        if (cfg_.username != nullptr) flags |= 0x80u;
        if (cfg_.password != nullptr) flags |= 0x40u;
        body[n++] = flags;
        body[n++] = static_cast<uint8_t>(cfg_.keepalive_s >> 8);
        body[n++] = static_cast<uint8_t>(cfg_.keepalive_s & 0xFFu);
        n = put_string(body, n, sizeof(body), cfg_.client_id);
        if (n == 0u) { close(); return Status::NoMemory; }
        if (has_will) {
            n = put_string(body, n, sizeof(body), cfg_.will_topic);
            if (n == 0u || n + 2u > sizeof(body)) { close(); return Status::NoMemory; }
            if (cfg_.will_len > 65535u || n + 2u + cfg_.will_len > sizeof(body)) {
                close();
                return Status::NoMemory;
            }
            body[n++] = static_cast<uint8_t>(cfg_.will_len >> 8);
            body[n++] = static_cast<uint8_t>(cfg_.will_len & 0xFFu);
            if (cfg_.will_len != 0u) {
                if (cfg_.will_payload == nullptr) { close(); return Status::InvalidArg; }
                const auto* p = static_cast<const uint8_t*>(cfg_.will_payload);
                for (Size i = 0u; i < cfg_.will_len; ++i) body[n++] = p[i];
            }
        }
        if (cfg_.username != nullptr) n = put_string(body, n, sizeof(body), cfg_.username);
        if (cfg_.password != nullptr) n = put_string(body, n, sizeof(body), cfg_.password);
        if (n == 0u || n > Capacity - 4u) { close(); return Status::NoMemory; }

        uint8_t pkt[Capacity]{};
        Size len = 0u;
        encode_fixed(pkt, len, 0x10u, n);
        for (Size i = 0u; i < n; ++i) pkt[len++] = body[i];
        st = send_all(pkt, len);
        if (st != Status::Ok) { close(); return st; }

        st = read_packet(timeout_ms);
        if (st != Status::Ok) { close(); return st; }
        if (rx_len_ < 2u || (rx_type_ >> 4) != 2u || rx_[1] != 0u) {
            close();
            return Status::NotSupported;
        }
        connected_ = true;
        pending_qos2_ = false;
        next_reconnect_ms_ = 0u;
        last_tx_ms_ = now_ms();
        last_rx_ms_ = last_tx_ms_;
        return Status::Ok;
    }

    Status reconnect(uint32_t timeout_ms = 5000u) noexcept {
        close();
        const Status st = connect(timeout_ms);
        if (st != Status::Ok) schedule_reconnect();
        return st;
    }

    Status publish(const char* topic, const void* payload, Size payload_len,
                   uint8_t qos = 0u, bool retain = false,
                   uint32_t timeout_ms = 5000u) noexcept {
        if (!connected_ || topic == nullptr) return Status::Unavailable;
        if (qos > 2u) return Status::NotSupported;
        const Size tlen = len_of(topic);
        if (tlen == 0u || tlen > 65535u) return Status::InvalidArg;

        uint8_t body[Capacity]{};
        Size n = 0u;
        n = put_string(body, n, sizeof(body), topic);
        if (n == 0u || n + 2u > sizeof(body)) return Status::NoMemory;
        uint16_t id = 0u;
        if (qos > 0u) {
            id = next_id();
            body[n++] = static_cast<uint8_t>(id >> 8);
            body[n++] = static_cast<uint8_t>(id & 0xFFu);
        }
        if (payload_len > 0u) {
            if (payload == nullptr || n + payload_len > sizeof(body)) return Status::NoMemory;
            const auto* p = static_cast<const uint8_t*>(payload);
            for (Size i = 0u; i < payload_len; ++i) body[n++] = p[i];
        }
        if (n > Capacity - 4u) return Status::NoMemory;

        uint8_t pkt[Capacity]{};
        Size len = 0u;
        const uint8_t hdr = static_cast<uint8_t>(0x30u | (qos << 1) | (retain ? 1u : 0u));
        encode_fixed(pkt, len, hdr, n);
        for (Size i = 0u; i < n; ++i) pkt[len++] = body[i];
        Status st = send_all(pkt, len);
        if (st != Status::Ok) return st;

        if (qos == 1u) return wait_id(4u, id, timeout_ms);
        if (qos == 2u) {
            st = wait_id(5u, id, timeout_ms);       // PUBREC
            if (st != Status::Ok) return st;
            uint8_t rel[4] = {0x62u, 0x02u,
                              static_cast<uint8_t>(id >> 8),
                              static_cast<uint8_t>(id & 0xFFu)};
            st = send_all(rel, sizeof(rel));
            if (st != Status::Ok) return st;
            return wait_id(7u, id, timeout_ms);     // PUBCOMP
        }
        return Status::Ok;
    }

    Status subscribe(const char* topic, uint8_t qos = 0u,
                     uint32_t timeout_ms = 5000u) noexcept {
        if (!connected_ || topic == nullptr) return Status::Unavailable;
        if (qos > 2u) return Status::InvalidArg;
        const uint16_t id = next_id();
        uint8_t body[Capacity]{};
        Size n = 0u;
        body[n++] = static_cast<uint8_t>(id >> 8);
        body[n++] = static_cast<uint8_t>(id & 0xFFu);
        n = put_string(body, n, sizeof(body), topic);
        if (n == 0u || n >= sizeof(body)) return Status::NoMemory;
        body[n++] = qos;
        if (n > Capacity - 4u) return Status::NoMemory;

        uint8_t pkt[Capacity]{};
        Size len = 0u;
        encode_fixed(pkt, len, 0x82u, n);
        for (Size i = 0u; i < n; ++i) pkt[len++] = body[i];
        Status st = send_all(pkt, len);
        if (st != Status::Ok) return st;
        return wait_id(9u, id, timeout_ms);
    }

    /// Принять один пакет, обслужить keepalive/reconnect. Возврат Timeout —
    /// данных не было; это нормальный результат периодического вызова.
    Status poll(uint32_t timeout_ms = 1000u) noexcept {
        if (!connected_) {
            if (!cfg_.auto_reconnect || !timer_available()) return Status::Unavailable;
            const uint64_t now = now_ms();
            if (now < next_reconnect_ms_) return Status::Unavailable;
            return reconnect(timeout_ms);
        }

        if (keepalive_due()) {
            const Status ps = send_ping();
            if (ps != Status::Ok) {
                close();
                schedule_reconnect();
                return ps;
            }
        }

        const Status st = read_packet(timeout_ms);
        if (st != Status::Ok) {
            if (st == Status::Timeout) {
                if (keepalive_expired()) {
                    close();
                    schedule_reconnect();
                    return Status::Timeout;
                }
                return Status::Timeout;
            }
            close();
            schedule_reconnect();
            return st;
        }

        const uint8_t type = static_cast<uint8_t>(rx_type_ >> 4);
        if (type == 3u) return handle_publish();
        if (type == 6u) return handle_pubrel();
        if (type == 13u) return Status::Ok;    // PINGRESP
        if (type == 4u || type == 5u || type == 7u || type == 9u) return Status::Ok;
        return Status::Ok;
    }

    Status ping(uint32_t timeout_ms = 5000u) noexcept {
        if (!connected_) return Status::Unavailable;
        Status st = send_ping();
        if (st != Status::Ok) return st;
        for (uint8_t i = 0u; i < 8u; ++i) {
            st = read_packet(timeout_ms);
            if (st != Status::Ok) return st;
            const uint8_t type = static_cast<uint8_t>(rx_type_ >> 4);
            if (type == 13u) return Status::Ok;
            if (type == 3u) {
                st = handle_publish();
                if (st != Status::Ok) return st;
            }
        }
        return Status::Aborted;
    }

    Status disconnect(uint32_t timeout_ms = 1000u) noexcept {
        (void)timeout_ms;
        if (!connected_) return Status::Ok;
        const uint8_t pkt[2] = {0xE0u, 0x00u};
        (void)send_all(pkt, sizeof(pkt));
        close();
        return Status::Ok;
    }

    void close() noexcept {
        if (sock_ != nullptr) {
            sock_->close();
            sock_ = nullptr;
        }
        connected_ = false;
    }

private:
    static Size len_of(const char* s) noexcept {
        Size n = 0u;
        if (s != nullptr) while (s[n] != '\0') ++n;
        return n;
    }

    static Size put_string(uint8_t* out, Size off, Size cap, const char* s) noexcept {
        const Size len = len_of(s);
        if (len > 65535u || off + 2u + len > cap) return 0u;
        out[off++] = static_cast<uint8_t>(len >> 8);
        out[off++] = static_cast<uint8_t>(len & 0xFFu);
        for (Size i = 0u; i < len; ++i) out[off++] = static_cast<uint8_t>(s[i]);
        return off;
    }

    static void encode_fixed(uint8_t* out, Size& off, uint8_t hdr, Size rem) noexcept {
        out[off++] = hdr;
        do {
            uint8_t b = static_cast<uint8_t>(rem % 128u);
            rem /= 128u;
            if (rem > 0u) b |= 0x80u;
            out[off++] = b;
        } while (rem > 0u);
    }

    static bool timer_available() noexcept { return config::OsTimer::TimeAvailable(); }
    static uint64_t now_ms() noexcept {
        return timer_available() ? config::OsTimer::NowMs() : 0u;
    }

    uint16_t next_id() noexcept {
        ++packet_id_;
        if (packet_id_ == 0u) packet_id_ = 1u;
        return packet_id_;
    }

    bool keepalive_due() const noexcept {
        if (cfg_.keepalive_s == 0u || !timer_available()) return false;
        const uint64_t period = static_cast<uint64_t>(cfg_.keepalive_s) * 1000u;
        return (now_ms() - last_tx_ms_) >= period;
    }

    bool keepalive_expired() const noexcept {
        if (cfg_.keepalive_s == 0u || !timer_available()) return false;
        const uint64_t period = static_cast<uint64_t>(cfg_.keepalive_s) * 1000u;
        return (now_ms() - last_rx_ms_) >= (period * 3u) / 2u;
    }

    Status send_ping() noexcept {
        const uint8_t pkt[2] = {0xC0u, 0x00u};
        return send_all(pkt, sizeof(pkt));
    }

    void schedule_reconnect() noexcept {
        if (!cfg_.auto_reconnect || !timer_available()) return;
        next_reconnect_ms_ = now_ms() + cfg_.reconnect_backoff_ms;
    }

    Status send_all(const void* data, Size len) noexcept {
        if (sock_ == nullptr) return Status::Unavailable;
        const auto* p = static_cast<const uint8_t*>(data);
        Size done = 0u;
        while (done < len) {
            Size sent = 0u;
            const Status st = sock_->send(p + done, len - done, sent);
            if (st != Status::Ok) return st;
            if (sent == 0u) return Status::Aborted;
            done += sent;
        }
        last_tx_ms_ = now_ms();
        return Status::Ok;
    }

    Status recv_exact(void* data, Size len, uint32_t timeout_ms) noexcept {
        auto* p = static_cast<uint8_t*>(data);
        Size done = 0u;
        while (done < len) {
            Size got = 0u;
            const Status st = sock_->recv(p + done, len - done, got, timeout_ms);
            if (st != Status::Ok) return st;
            if (got == 0u) return Status::Unavailable;
            done += got;
        }
        return Status::Ok;
    }

    Status read_packet(uint32_t timeout_ms) noexcept {
        if (sock_ == nullptr) return Status::Unavailable;
        uint8_t hdr = 0u;
        Status st = recv_exact(&hdr, 1u, timeout_ms);
        if (st != Status::Ok) return st;
        rx_type_ = hdr;

        Size rem = 0u, mult = 1u;
        for (uint8_t i = 0u; i < 4u; ++i) {
            uint8_t b = 0u;
            st = recv_exact(&b, 1u, timeout_ms);
            if (st != Status::Ok) return st;
            rem += static_cast<Size>(b & 0x7Fu) * mult;
            if ((b & 0x80u) == 0u) break;
            mult *= 128u;
            if (i == 3u) return Status::Corruption;
        }
        if (rem > Capacity) return Status::NoMemory;
        if (rem > 0u) {
            st = recv_exact(rx_, rem, timeout_ms);
            if (st != Status::Ok) return st;
        }
        rx_len_ = rem;
        last_rx_ms_ = now_ms();
        return Status::Ok;
    }

    Status wait_id(uint8_t want_type, uint16_t id, uint32_t timeout_ms) noexcept {
        for (uint8_t i = 0u; i < 8u; ++i) {
            const Status st = read_packet(timeout_ms);
            if (st != Status::Ok) return st;
            const uint8_t type = static_cast<uint8_t>(rx_type_ >> 4);
            if (type == 3u) {
                const Status hs = handle_publish();
                if (hs != Status::Ok) return hs;
                continue;
            }
            if (type == 6u) {
                const Status hs = handle_pubrel();
                if (hs != Status::Ok) return hs;
                continue;
            }
            if (type != want_type) continue;
            if (want_type == 9u) return Status::Ok;      // SUBACK
            if (want_type == 13u) return Status::Ok;     // PINGRESP
            if (rx_len_ < 2u) return Status::Corruption;
            const uint16_t got = static_cast<uint16_t>((static_cast<uint16_t>(rx_[0]) << 8) |
                                                       rx_[1]);
            if (got == id) return Status::Ok;
        }
        return Status::Aborted;
    }

    Status handle_pubrel() noexcept {
        if (rx_len_ < 2u) return Status::Corruption;
        const uint16_t id = static_cast<uint16_t>((static_cast<uint16_t>(rx_[0]) << 8) | rx_[1]);
        if (pending_qos2_ && id == qos2_id_) {
            pending_qos2_ = false;
            Message msg{};
            msg.topic = qos2_topic_;
            msg.payload = qos2_payload_;
            msg.len = qos2_len_;
            msg.qos = 2u;
            if (cb_ != nullptr) cb_(cb_ctx_, msg);
        }
        uint8_t comp[4] = {0x70u, 0x02u,
                           static_cast<uint8_t>(id >> 8),
                           static_cast<uint8_t>(id & 0xFFu)};
        return send_all(comp, sizeof(comp));
    }

    Status handle_publish() noexcept {
        if (rx_len_ < 2u) return Status::Corruption;
        const Size tlen = (static_cast<Size>(rx_[0]) << 8) | rx_[1];
        if (2u + tlen > rx_len_) return Status::Corruption;
        const uint8_t qos = static_cast<uint8_t>((rx_type_ >> 1) & 0x03u);
        Size off = 2u + tlen;
        uint16_t id = 0u;
        if (qos > 0u) {
            if (off + 2u > rx_len_) return Status::Corruption;
            id = static_cast<uint16_t>((static_cast<uint16_t>(rx_[off]) << 8) | rx_[off + 1u]);
            off += 2u;
        }
        if (tlen >= sizeof(qos2_topic_)) return Status::NoMemory;
        for (Size i = 0u; i < tlen; ++i) qos2_topic_[i] = static_cast<char>(rx_[2u + i]);
        qos2_topic_[tlen] = '\0';

        if (qos == 2u) {
            uint8_t rec[4] = {0x50u, 0x02u,
                              static_cast<uint8_t>(id >> 8),
                              static_cast<uint8_t>(id & 0xFFu)};
            const Status st = send_all(rec, sizeof(rec));
            if (st != Status::Ok) return st;
            const Size plen = rx_len_ - off;
            if (plen > sizeof(qos2_payload_)) return Status::NoMemory;
            for (Size i = 0u; i < plen; ++i) qos2_payload_[i] = rx_[off + i];
            qos2_id_ = id;
            qos2_len_ = plen;
            pending_qos2_ = true;
            return Status::Ok;
        }

        Message msg{};
        msg.topic = qos2_topic_;
        msg.payload = rx_ + off;
        msg.len = rx_len_ - off;
        msg.qos = qos;
        msg.retain = (rx_type_ & 0x01u) != 0u;
        if (cb_ != nullptr) cb_(cb_ctx_, msg);

        if (qos == 1u) {
            uint8_t ack[4] = {0x40u, 0x02u,
                              static_cast<uint8_t>(id >> 8),
                              static_cast<uint8_t>(id & 0xFFu)};
            return send_all(ack, sizeof(ack));
        }
        return Status::Ok;
    }

    Factory* factory_ = nullptr;
    Socket*  sock_ = nullptr;
    Config   cfg_{};
    bool     connected_ = false;
    uint16_t packet_id_ = 0u;
    MessageCallback cb_ = nullptr;
    void* cb_ctx_ = nullptr;
    uint8_t rx_[Capacity]{};
    Size    rx_len_ = 0u;
    uint8_t rx_type_ = 0u;

    uint64_t last_tx_ms_ = 0u;
    uint64_t last_rx_ms_ = 0u;
    uint64_t next_reconnect_ms_ = 0u;

    bool     pending_qos2_ = false;
    uint16_t qos2_id_ = 0u;
    Size     qos2_len_ = 0u;
    char     qos2_topic_[192]{};
    uint8_t  qos2_payload_[Capacity]{};
};

} // namespace embed::net::mqtt
