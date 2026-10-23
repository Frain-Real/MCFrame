// os/net/mqtt.hpp
// ---------------------------------------------------------------------------
// MQTT 3.1.1 client (QoS 0/1, subscribe, keepalive ping).
//
// Клиент намеренно синхронный: приложение вызывает connect/publish/poll из
// своей задачи. Это проще проверить и не требует собственного event-loop;
// таймауты передаются в сокет. Буферы статические, размер задаётся шаблоном
// Capacity (по умолчанию 768 байт).
// ---------------------------------------------------------------------------
#pragma once

#include "socket.hpp"

namespace embed::net::mqtt {

struct Config {
    const char* host        = nullptr;
    uint16_t    port        = 1883u;
    const char* client_id   = "mcframe";
    const char* username    = nullptr;
    const char* password    = nullptr;
    uint16_t    keepalive_s = 60u;
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
        factory_ = &factory;
        cfg_ = cfg;
        connected_ = false;
        packet_id_ = 0u;
        return Status::Ok;
    }

    void set_callback(MessageCallback cb, void* ctx) noexcept {
        cb_ = cb;
        cb_ctx_ = ctx;
    }

    bool connected() const noexcept { return connected_; }

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

        // CONNECT: protocol "MQTT", level 4, flags, keepalive, client id.
        uint8_t body[320]{};
        Size n = 0u;
        n = put_string(body, n, sizeof(body), "MQTT");
        body[n++] = 4u;                      // protocol level 3.1.1
        uint8_t flags = 0x02u;               // clean session
        if (cfg_.username != nullptr) flags |= 0x80u;
        if (cfg_.password != nullptr) flags |= 0x40u;
        body[n++] = flags;
        body[n++] = static_cast<uint8_t>(cfg_.keepalive_s >> 8);
        body[n++] = static_cast<uint8_t>(cfg_.keepalive_s & 0xFFu);
        n = put_string(body, n, sizeof(body), cfg_.client_id);
        if (cfg_.username != nullptr) n = put_string(body, n, sizeof(body), cfg_.username);
        if (cfg_.password != nullptr) n = put_string(body, n, sizeof(body), cfg_.password);
        if (n == 0u) { close(); return Status::NoMemory; }

        if (n > Capacity - 4u) { close(); return Status::NoMemory; }
        uint8_t pkt[Capacity]{};
        Size len = 0u;
        if (!encode_fixed(pkt, len, 0x10u, n)) { close(); return Status::NoMemory; }
        for (Size i = 0u; i < n; ++i) pkt[len++] = body[i];
        st = send_all(pkt, len);
        if (st != Status::Ok) { close(); return st; }

        st = read_packet(timeout_ms);
        if (st != Status::Ok) { close(); return st; }
        if (rx_len_ < 2u || (rx_type_ >> 4) != 2u || rx_[1] != 0u) {
            close();
            return Status::NotSupported;     // CONNACK с ошибкой
        }
        connected_ = true;
        return Status::Ok;
    }

    Status publish(const char* topic, const void* payload, Size payload_len,
                   uint8_t qos = 0u, bool retain = false,
                   uint32_t timeout_ms = 5000u) noexcept {
        if (!connected_ || topic == nullptr) return Status::Unavailable;
        if (qos > 1u) return Status::NotSupported;
        const Size tlen = len_of(topic);
        if (tlen == 0u || tlen > 65535u) return Status::InvalidArg;

        uint8_t body[Capacity]{};
        Size n = 0u;
        n = put_string(body, n, sizeof(body), topic);
        if (n == 0u) return Status::NoMemory;
        if (qos > 0u) {
            const uint16_t id = next_id();
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
        if (!encode_fixed(pkt, len, hdr, n)) return Status::NoMemory;
        for (Size i = 0u; i < n; ++i) pkt[len++] = body[i];
        Status st = send_all(pkt, len);
        if (st != Status::Ok) return st;

        if (qos == 1u) {
            st = read_packet(timeout_ms);
            if (st != Status::Ok) return st;
            if ((rx_type_ >> 4) != 4u) return Status::Aborted;
        }
        return Status::Ok;
    }

    Status subscribe(const char* topic, uint8_t qos = 0u,
                     uint32_t timeout_ms = 5000u) noexcept {
        if (!connected_ || topic == nullptr) return Status::Unavailable;
        if (qos > 1u) return Status::InvalidArg;
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
        if (!encode_fixed(pkt, len, 0x82u, n)) return Status::NoMemory;
        for (Size i = 0u; i < n; ++i) pkt[len++] = body[i];
        Status st = send_all(pkt, len);
        if (st != Status::Ok) return st;

        st = read_packet(timeout_ms);
        if (st != Status::Ok) return st;
        if ((rx_type_ >> 4) != 9u) return Status::Aborted;
        return Status::Ok;
    }

    /// Принять один пакет от брокера и разобрать PUBLISH/PINGRESP.
    Status poll(uint32_t timeout_ms = 1000u) noexcept {
        if (!connected_) return Status::Unavailable;
        const Status st = read_packet(timeout_ms);
        if (st != Status::Ok) return st;

        const uint8_t type = static_cast<uint8_t>(rx_type_ >> 4);
        if (type == 3u) {                      // PUBLISH
            return handle_publish();
        }
        if (type == 13u) return Status::Ok;    // PINGRESP
        if (type == 4u) return Status::Ok;     // PUBACK
        return Status::Ok;                     // прочие — игнорируем
    }

    Status ping(uint32_t timeout_ms = 5000u) noexcept {
        if (!connected_) return Status::Unavailable;
        const uint8_t pkt[2] = {0xC0u, 0x00u};
        Status st = send_all(pkt, sizeof(pkt));
        if (st != Status::Ok) return st;
        st = read_packet(timeout_ms);
        if (st != Status::Ok) return st;
        return ((rx_type_ >> 4) == 13u) ? Status::Ok : Status::Aborted;
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

    static bool encode_fixed(uint8_t* out, Size& off, uint8_t hdr, Size rem) noexcept {
        out[off++] = hdr;
        do {
            uint8_t b = static_cast<uint8_t>(rem % 128u);
            rem /= 128u;
            if (rem > 0u) b |= 0x80u;
            out[off++] = b;
        } while (rem > 0u);
        return true;
    }

    uint16_t next_id() noexcept {
        ++packet_id_;
        if (packet_id_ == 0u) packet_id_ = 1u;
        return packet_id_;
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
        return Status::Ok;
    }

    Status handle_publish() noexcept {
        if (rx_len_ < 2u) return Status::Corruption;
        const Size tlen = (static_cast<Size>(rx_[0]) << 8) | rx_[1];
        if (2u + tlen > rx_len_) return Status::Corruption;
        // Топик не копируем: он валиден до следующего poll().
        char* topic = reinterpret_cast<char*>(rx_ + 2u);
        const uint8_t qos = static_cast<uint8_t>((rx_type_ >> 1) & 0x03u);
        Size off = 2u + tlen;
        uint16_t id = 0u;
        if (qos > 0u) {
            if (off + 2u > rx_len_) return Status::Corruption;
            id = static_cast<uint16_t>((static_cast<uint16_t>(rx_[off]) << 8) | rx_[off + 1u]);
            off += 2u;
        }
        // Топик копируем в локальный буфер: MQTT не завершает его NUL, а
        // затирать первый байт payload ради строки нельзя.
        char topic_buf[192]{};
        if (tlen >= sizeof(topic_buf)) return Status::NoMemory;
        for (Size i = 0u; i < tlen; ++i) topic_buf[i] = topic[i];
        topic_buf[tlen] = '\0';

        Message msg{};
        msg.topic = topic_buf;
        msg.payload = rx_ + off;
        msg.len = rx_len_ - off;
        msg.qos = qos;
        msg.retain = (rx_type_ & 0x01u) != 0u;

        if (cb_ != nullptr) cb_(cb_ctx_, msg);

        if (qos == 1u) {
            uint8_t ack[4] = {0x40u, 0x02u,
                              static_cast<uint8_t>(id >> 8),
                              static_cast<uint8_t>(id & 0xFFu)};
            (void)send_all(ack, sizeof(ack));
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
};

} // namespace embed::net::mqtt
