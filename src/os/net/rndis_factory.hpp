// os/net/rndis_factory.hpp
// ---------------------------------------------------------------------------
// net::Factory поверх USB RNDIS с компактным IPv4-стеком:
// ARP, ICMP echo (для link), UDP, TCP client, DNS A-запрос.
//
// RNDIS — L2; этот файл добавляет достаточный IP-стек, чтобы MQTT/HTTP/SNTP
// работали через тот же net::Socket, что и lwIP/W5500.
//
// Приложение обязано:
//   * передавать bulk OUT кадры в Rndis::receive();
//   * вызывать RndisFactory::poll() (flush TX + pump);
//   * задать Pump, если контроллер не умеет ep_read (F4 OTG callback).
// ---------------------------------------------------------------------------
#pragma once

#include "socket.hpp"
#include "dhcp.hpp"
#include "dns.hpp"
#include "../usb/rndis.hpp"
#include "../usb/usb.hpp"
#include "../config/time_config.hpp"

namespace embed::net {

struct RndisNetConfig {
    uint8_t  mac[6]  = {0x02, 0x4D, 0x43, 0x00, 0x00, 0x03};
    uint8_t  ip[4]   = {192, 168, 137, 2};
    uint8_t  gw[4]   = {192, 168, 137, 1};
    uint8_t  mask[4] = {255, 255, 255, 0};
    uint8_t  dns[4]  = {192, 168, 137, 1};
    uint16_t mtu     = 1500u;
};

class RndisFactory;

class RndisSocket final : public Socket {
public:
    RndisSocket() noexcept = default;

    Status begin(RndisFactory* owner, Protocol proto, uint16_t local_port) noexcept;
    Status connect(const Endpoint& remote, uint32_t timeout_ms) noexcept override;
    Status send(const void* data, Size len, Size& sent) noexcept override;
    Status recv(void* data, Size cap, Size& received, uint32_t timeout_ms) noexcept override;
    Status send_to(const Endpoint& remote, const void* data, Size len, Size& sent) noexcept override;
    Status recv_from(Endpoint& remote, void* data, Size cap, Size& received,
                     uint32_t timeout_ms) noexcept override;
    void close() noexcept override;
    bool open() const noexcept override { return open_; }

private:
    friend class RndisFactory;
    enum class State : uint8_t { Closed, SynSent, Established, FinWait, ClosedByPeer };
    void on_ip(const uint8_t* ip, Size len) noexcept;
    void on_udp(const uint8_t* src, uint16_t sport, const uint8_t* data, Size len) noexcept;
    void on_tcp(const uint8_t* src, uint16_t sport, const uint8_t* tcp, Size len) noexcept;
    Status send_tcp(uint8_t flags, const void* data, Size len) noexcept;
    Status send_udp(const Endpoint& dst, const void* data, Size len) noexcept;
    Status wait_established(uint32_t timeout_ms) noexcept;
    uint16_t alloc_port() noexcept;

    RndisFactory* owner_ = nullptr;
    Protocol proto_ = Protocol::Tcp;
    State state_ = State::Closed;
    Endpoint remote_{};
    uint16_t local_port_ = 0u;
    uint32_t seq_ = 0x10000u;
    uint32_t ack_ = 0u;
    bool open_ = false;
    uint8_t rx_[1536]{};
    Size rx_len_ = 0u;
    Size rx_pos_ = 0u;
    uint8_t udp_rx_[1536]{};
    Size udp_len_ = 0u;
    uint32_t dropped_ = 0u;
    Endpoint udp_from_{};
};

class RndisFactory final : public Factory {
public:
    using Pump = Status (*)(void* ctx) noexcept;

    RndisFactory(usb::rndis::Rndis& rndis, usb::Hal& hal,
                 const RndisNetConfig& cfg = RndisNetConfig{}) noexcept
        : rndis_(&rndis), hal_(&hal), cfg_(cfg) {}

    void set_pump(Pump pump, void* ctx) noexcept { pump_ = pump; pump_ctx_ = ctx; }

    Status init() noexcept {
        rndis_->set_sink(&RndisFactory::frame_sink, this);
        inited_ = true;
        return Status::Ok;
    }

    Status create(Protocol proto, Socket*& out) noexcept override {
        if (!inited_) return Status::Unavailable;
        for (Size i = 0u; i < kSockets; ++i) {
            if (!pool_[i].open()) {
                const Status st = pool_[i].begin(this, proto, 0u);
                if (st != Status::Ok) return st;
                out = &pool_[i];
                return Status::Ok;
            }
        }
        return Status::NoMemory;
    }

    Status resolve(const char* host, Endpoint& out) noexcept override {
        uint8_t ip[4]{};
        if (parse_ipv4(host, ip)) {
            for (uint8_t i = 0u; i < 4u; ++i) out.addr[i] = ip[i];
            return Status::Ok;
        }
        // DNS A через временный UDP-сокет.
        Socket* s = nullptr;
        Status st = create(Protocol::Udp, s);
        if (st != Status::Ok) return st;
        Endpoint dns{};
        for (uint8_t i = 0u; i < 4u; ++i) dns.addr[i] = cfg_.dns[i];
        dns.port = 53u;
        uint8_t q[260]{};
        const Size qn = build_dns_query(q, sizeof(q), host);
        if (qn == 0u) { s->close(); return Status::InvalidArg; }
        Size sent = 0u;
        st = s->send_to(dns, q, qn, sent);
        if (st != Status::Ok) { s->close(); return st; }
        uint8_t rx[512]{};
        Size got = 0u;
        Endpoint from{};
        st = s->recv_from(from, rx, sizeof(rx), got, 3000u);
        s->close();
        if (st != Status::Ok) return st;
        if (!DnsClient::parse_answer(rx, got, ip)) return Status::NotFound;
        for (uint8_t i = 0u; i < 4u; ++i) out.addr[i] = ip[i];
        return Status::Ok;
    }

    /// DHCP DISCOVER/REQUEST через RNDIS (broadcast, UDP 68→67).
    Status dhcp(uint32_t timeout_ms) noexcept {
        if (!inited_) return Status::Unavailable;
        Socket* raw = nullptr;
        Status st = create(Protocol::Udp, raw);
        if (st != Status::Ok || raw == nullptr) return st;
        auto* s = static_cast<RndisSocket*>(raw);
        s->local_port_ = 68u;
        const uint32_t xid = 0x4D434632u ^ static_cast<uint32_t>(config::OsTimer::NowMs());
        uint8_t pkt[300]{};
        Size n = DhcpClient::build_discover(pkt, sizeof(pkt), xid, cfg_.mac);
        Endpoint bcast{255,255,255,255,67u};
        Size sent = 0u;
        st = s->send_to(bcast, pkt, n, sent);
        if (st != Status::Ok) { s->close(); return st; }
        DhcpLease lease{};
        bool offered = false;
        const uint64_t start = config::OsTimer::TimeAvailable() ? config::OsTimer::NowMs() : 0u;
        while (!offered) {
            uint8_t rx[512]{};
            Size got = 0u;
            Endpoint from{};
            st = s->recv_from(from, rx, sizeof(rx), got, 500u);
            if (st == Status::Timeout) {
                if (config::OsTimer::TimeAvailable() &&
                    config::OsTimer::NowMs() - start >= timeout_ms) break;
                continue;
            }
            if (st != Status::Ok) break;
            uint8_t type = 0u;
            if (!DhcpClient::parse_reply(rx, got, xid, type, lease)) continue;
            if (type == 2u || type == 5u) offered = true;
        }
        if (!offered) { s->close(); return Status::Timeout; }
        n = DhcpClient::build_request(pkt, sizeof(pkt), xid, cfg_.mac, lease);
        st = s->send_to(bcast, pkt, n, sent);
        if (st != Status::Ok) { s->close(); return st; }
        bool acked = false;
        while (!acked) {
            uint8_t rx[512]{};
            Size got = 0u;
            Endpoint from{};
            st = s->recv_from(from, rx, sizeof(rx), got, 500u);
            if (st == Status::Timeout) {
                if (config::OsTimer::TimeAvailable() &&
                    config::OsTimer::NowMs() - start >= timeout_ms) break;
                continue;
            }
            if (st != Status::Ok) break;
            uint8_t type = 0u;
            DhcpLease a{};
            if (!DhcpClient::parse_reply(rx, got, xid, type, a)) continue;
            if (type == 5u) { lease = a; acked = true; }
        }
        s->close();
        if (!acked) return Status::Timeout;
        for (uint8_t i = 0u; i < 4u; ++i) {
            cfg_.ip[i] = lease.ip[i];
            cfg_.gw[i] = lease.gw[i];
            cfg_.mask[i] = lease.mask[i];
            if (lease.dns[i] != 0u) cfg_.dns[i] = lease.dns[i];
        }
        return Status::Ok;
    }

    /// Обслужить USB: pump (если задан), затем отправить накопленные кадры.
    Status poll() noexcept {
        if (!inited_) return Status::Unavailable;
        if (pump_ != nullptr) {
            const Status st = pump_(pump_ctx_);
            if (st != Status::Ok && st != Status::Timeout) return st;
        }
        (void)rndis_->flush(*hal_);
        return Status::Ok;
    }

    const RndisNetConfig& config() const noexcept { return cfg_; }
    uint32_t dropped() const noexcept {
        uint32_t n = 0u;
        for (Size i = 0u; i < kSockets; ++i) n += pool_[i].dropped_;
        return n;
    }
    RndisNetConfig& config() noexcept { return cfg_; }

    static void frame_sink(void* ctx, const uint8_t* frame, Size len) noexcept {
        auto* self = static_cast<RndisFactory*>(ctx);
        if (self != nullptr) self->on_frame(frame, len);
    }

private:
    friend class RndisSocket;
#ifndef MCFRAME_RNDIS_FACTORY_SOCKETS
#define MCFRAME_RNDIS_FACTORY_SOCKETS 4u
#endif
    static constexpr Size kSockets = MCFRAME_RNDIS_FACTORY_SOCKETS;
    static_assert(kSockets >= 1u && kSockets <= 8u, "RNDIS factory: 1..8 sockets");

    static Status pump_none(void*) { return Status::Ok; }

    static uint16_t csum(const void* data, Size len) noexcept {
        const auto* p = static_cast<const uint8_t*>(data);
        uint32_t sum = 0u;
        for (Size i = 0u; i + 1u < len; i += 2u) {
            sum += static_cast<uint16_t>((p[i] << 8) | p[i + 1u]);
        }
        if ((len & 1u) != 0u) sum += static_cast<uint16_t>(p[len - 1u] << 8);
        while ((sum >> 16) != 0u) sum = (sum & 0xFFFFu) + (sum >> 16);
        return static_cast<uint16_t>(~sum);
    }

    bool same_subnet(const uint8_t a[4], const uint8_t b[4]) const noexcept {
        for (uint8_t i = 0u; i < 4u; ++i) {
            if ((a[i] & cfg_.mask[i]) != (b[i] & cfg_.mask[i])) return false;
        }
        return true;
    }

    Status resolve_mac(const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms) noexcept {
            if (arp_valid_ && same_ip(ip, arp_ip_)) {
            for (uint8_t i = 0u; i < 6u; ++i) mac[i] = arp_mac_[i];
            return Status::Ok;
        }
        const uint8_t* target = same_subnet(ip, cfg_.ip) ? ip : cfg_.gw;
        if (arp_valid_ && same_ip(target, arp_ip_)) {
            for (uint8_t i = 0u; i < 6u; ++i) mac[i] = arp_mac_[i];
            return Status::Ok;
        }
        (void)send_arp_request(target);
        const bool timed = config::OsTimer::TimeAvailable();
        const uint64_t start = timed ? config::OsTimer::NowMs() : 0u;
        for (uint32_t n = 0u; n < 2000u; ++n) {
            (void)poll();
            if (arp_valid_ && same_ip(arp_ip_, target)) {
                for (uint8_t i = 0u; i < 6u; ++i) mac[i] = arp_mac_[i];
                return Status::Ok;
            }
            if (timed && config::OsTimer::NowMs() - start >= timeout_ms) break;
        }
        return Status::Timeout;
    }

    Status emit(const uint8_t dst_mac[6], uint16_t ethertype,
                const uint8_t* payload, Size len) noexcept {
        if (len + 14u > 1536u) return Status::NoMemory;
        uint8_t frame[1536]{};
        for (uint8_t i = 0u; i < 6u; ++i) frame[i] = dst_mac[i];
        for (uint8_t i = 0u; i < 6u; ++i) frame[6u + i] = cfg_.mac[i];
        frame[12] = static_cast<uint8_t>(ethertype >> 8);
        frame[13] = static_cast<uint8_t>(ethertype);
        for (Size i = 0u; i < len; ++i) frame[14u + i] = payload[i];
        const Status st = rndis_->send_frame(frame, 14u + len);
        if (st != Status::Ok) return st;
        return rndis_->flush(*hal_) > 0u ? Status::Ok : Status::Busy;
    }

    Status send_arp_request(const uint8_t target[4]) noexcept {
        uint8_t a[28]{};
        a[0] = 0u; a[1] = 1u; a[2] = 8u; a[3] = 0u; a[4] = 6u; a[5] = 4u;
        a[6] = 0u; a[7] = 1u;                         // request
        for (uint8_t i = 0u; i < 6u; ++i) a[8u + i] = cfg_.mac[i];
        for (uint8_t i = 0u; i < 4u; ++i) a[14u + i] = cfg_.ip[i];
        for (uint8_t i = 0u; i < 6u; ++i) a[18u + i] = 0u;
        for (uint8_t i = 0u; i < 4u; ++i) a[24u + i] = target[i];
        const uint8_t bc[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        return emit(bc, 0x0806u, a, sizeof(a));
    }

    void reply_arp(const uint8_t* req) noexcept {
        uint8_t r[28]{};
        r[0] = 0u; r[1] = 1u; r[2] = 8u; r[3] = 0u; r[4] = 6u; r[5] = 4u;
        r[6] = 0u; r[7] = 2u;
        for (uint8_t i = 0u; i < 6u; ++i) r[8u + i] = cfg_.mac[i];
        for (uint8_t i = 0u; i < 4u; ++i) r[14u + i] = cfg_.ip[i];
        for (uint8_t i = 0u; i < 6u; ++i) r[18u + i] = req[8u + i];
        for (uint8_t i = 0u; i < 4u; ++i) r[24u + i] = req[14u + i];
        uint8_t dst[6]{};
        for (uint8_t i = 0u; i < 6u; ++i) dst[i] = req[8u + i];
        (void)emit(dst, 0x0806u, r, sizeof(r));
    }

    void on_frame(const uint8_t* frame, Size len) noexcept {
        if (frame == nullptr || len < 14u) return;
        const uint16_t type = static_cast<uint16_t>((frame[12] << 8) | frame[13]);
        if (type == 0x0806u && len >= 42u) {
            const uint8_t* a = frame + 14u;
            if (a[6] == 0u && a[7] == 2u) {             // reply
                arp_valid_ = true;
                for (uint8_t i = 0u; i < 4u; ++i) arp_ip_[i] = a[14u + i];
                for (uint8_t i = 0u; i < 6u; ++i) arp_mac_[i] = a[8u + i];
            } else if (a[6] == 0u && a[7] == 1u) {      // request
                bool ours = true;
                for (uint8_t i = 0u; i < 4u; ++i) if (a[24u + i] != cfg_.ip[i]) ours = false;
                if (ours) reply_arp(a);
            }
            return;
        }
        if (type != 0x0800u) return;
        const uint8_t* ip = frame + 14u;
        const Size iplen = len - 14u;
        if (iplen < 20u || (ip[0] >> 4) != 4u) return;
        const Size ihl = static_cast<Size>(ip[0] & 0x0Fu) * 4u;
        const Size total = static_cast<Size>((ip[2] << 8) | ip[3]);
        if (ihl < 20u || total < ihl || total > iplen) return;
        for (Size i = 0u; i < kSockets; ++i) {
            if (pool_[i].open()) pool_[i].on_ip(ip, total);
        }
    }

    static bool same_ip(const uint8_t a[4], const uint8_t b[4]) noexcept {
        for (uint8_t i = 0u; i < 4u; ++i) if (a[i] != b[i]) return false;
        return true;
    }

    static Size build_dns_query(uint8_t* out, Size cap, const char* host) noexcept {
        if (out == nullptr || cap < 18u || host == nullptr) return 0u;
        for (Size i = 0u; i < 12u; ++i) out[i] = 0u;
        out[0] = 0x12u; out[1] = 0x34u; out[2] = 0x01u;
        out[5] = 0x01u;
        Size n = 12u;
        Size label = 0u;
        for (const char* p = host;; ++p) {
            if (*p == '.' || *p == '\0') {
                if (label == 0u || label > 63u || n + 1u + label + 5u > cap) return 0u;
                out[n++] = static_cast<uint8_t>(label);
                for (Size i = 0u; i < label; ++i) out[n++] = static_cast<uint8_t>(p[i - label]);
                label = 0u;
                if (*p == '\0') break;
            } else if (++label > 63u) return 0u;
        }
        out[n++] = 0u; out[n++] = 0x00u; out[n++] = 0x01u;
        out[n++] = 0x00u; out[n++] = 0x01u;
        return n;
    }

    usb::rndis::Rndis* rndis_ = nullptr;
    usb::Hal* hal_ = nullptr;
    RndisNetConfig cfg_{};
    RndisSocket pool_[kSockets];
    bool inited_ = false;
    bool arp_valid_ = false;
    uint8_t arp_ip_[4]{};
    uint8_t arp_mac_[6]{};
    Pump pump_ = nullptr;
    void* pump_ctx_ = nullptr;
};

inline Status RndisSocket::begin(RndisFactory* owner, Protocol proto, uint16_t local_port) noexcept {
    owner_ = owner;
    proto_ = proto;
    local_port_ = local_port != 0u ? local_port : alloc_port();
    state_ = State::Closed;
    rx_len_ = 0u; rx_pos_ = 0u; udp_len_ = 0u;
    open_ = true;
    return Status::Ok;
}
inline uint16_t RndisSocket::alloc_port() noexcept {
    static uint16_t next = 49152u;
    ++next;
    return next;
}
inline void RndisSocket::close() noexcept {
    if (open_ && proto_ == Protocol::Tcp && state_ == State::Established) {
        (void)send_tcp(0x11u, nullptr, 0u);              // FIN
    }
    open_ = false;
    state_ = State::Closed;
    rx_len_ = rx_pos_ = udp_len_ = 0u;
}
inline Status RndisSocket::connect(const Endpoint& remote, uint32_t timeout_ms) noexcept {
    if (!open_ || owner_ == nullptr) return Status::Unavailable;
    remote_ = remote;
    if (proto_ == Protocol::Udp) return Status::Ok;
    uint8_t mac[6]{};
    Status st = owner_->resolve_mac(remote.addr, mac, timeout_ms);
    if (st != Status::Ok) return st;
    st = send_tcp(0x02u, nullptr, 0u);                   // SYN
    if (st != Status::Ok) return st;
    state_ = State::SynSent;
    return wait_established(timeout_ms);
}
inline Status RndisSocket::wait_established(uint32_t timeout_ms) noexcept {
    const bool timed = config::OsTimer::TimeAvailable();
    const uint64_t start = timed ? config::OsTimer::NowMs() : 0u;
    for (uint32_t i = 0u; i < 5000u; ++i) {
        (void)owner_->poll();
        if (state_ == State::Established) return Status::Ok;
        if (state_ == State::Closed) return Status::Unavailable;
        if (timed && config::OsTimer::NowMs() - start >= timeout_ms) return Status::Timeout;
    }
    return Status::Timeout;
}
inline Status RndisSocket::send_tcp(uint8_t flags, const void* data, Size len) noexcept {
    if (owner_ == nullptr || len > 536u) return Status::InvalidArg;
    uint8_t mac[6]{};
    Status st = owner_->resolve_mac(remote_.addr, mac, 1000u);
    if (st != Status::Ok) return st;
    uint8_t tcp[20 + 536]{};
    const uint16_t src = local_port_;
    const uint16_t dst = remote_.port;
    tcp[0] = static_cast<uint8_t>(src >> 8); tcp[1] = static_cast<uint8_t>(src);
    tcp[2] = static_cast<uint8_t>(dst >> 8); tcp[3] = static_cast<uint8_t>(dst);
    const uint32_t seq = seq_;
    const uint32_t ack = ack_;
    tcp[4] = static_cast<uint8_t>(seq >> 24); tcp[5] = static_cast<uint8_t>(seq >> 16);
    tcp[6] = static_cast<uint8_t>(seq >> 8); tcp[7] = static_cast<uint8_t>(seq);
    tcp[8] = static_cast<uint8_t>(ack >> 24); tcp[9] = static_cast<uint8_t>(ack >> 16);
    tcp[10] = static_cast<uint8_t>(ack >> 8); tcp[11] = static_cast<uint8_t>(ack);
    tcp[12] = 5u << 4u; tcp[13] = flags;
    tcp[14] = 0x20u; tcp[15] = 0x00u;                    // window 8192
    const Size seg = 20u + len;
    const auto* p = static_cast<const uint8_t*>(data);
    for (Size i = 0u; i < len; ++i) tcp[20u + i] = p[i];
    // pseudo-header + TCP checksum.
    uint8_t pseudo[12 + 536 + 20]{};
    for (uint8_t i = 0u; i < 4u; ++i) pseudo[i] = owner_->cfg_.ip[i];
    for (uint8_t i = 0u; i < 4u; ++i) pseudo[4u + i] = remote_.addr[i];
    pseudo[8] = 0u; pseudo[9] = 6u;
    pseudo[10] = static_cast<uint8_t>(seg >> 8); pseudo[11] = static_cast<uint8_t>(seg);
    for (Size i = 0u; i < seg; ++i) pseudo[12u + i] = tcp[i];
    const uint16_t c = RndisFactory::csum(pseudo, 12u + seg);
    tcp[16] = static_cast<uint8_t>(c >> 8); tcp[17] = static_cast<uint8_t>(c);
    uint8_t ip[20 + 536]{};
    ip[0] = 0x45u;
    ip[2] = static_cast<uint8_t>(seg >> 8); ip[3] = static_cast<uint8_t>(seg);
    ip[4] = 0x12u; ip[5] = 0x34u; ip[6] = 0x40u; ip[7] = 0u; ip[8] = 64u;
    ip[9] = 6u;
    for (uint8_t i = 0u; i < 4u; ++i) ip[12u + i] = owner_->cfg_.ip[i];
    for (uint8_t i = 0u; i < 4u; ++i) ip[16u + i] = remote_.addr[i];
    const uint16_t ic = RndisFactory::csum(ip, 20u);
    ip[10] = static_cast<uint8_t>(ic >> 8); ip[11] = static_cast<uint8_t>(ic);
    for (Size i = 0u; i < seg; ++i) ip[20u + i] = tcp[i];
    st = owner_->emit(mac, 0x0800u, ip, 20u + seg);
    if (st == Status::Ok && (flags & 0x02u) != 0u) seq_ += 1u;   // SYN consumes seq
    else if (st == Status::Ok && len != 0u) seq_ += static_cast<uint32_t>(len);
    if (st == Status::Ok && (flags & 0x01u) != 0u) seq_ += 1u;   // FIN
    return st;
}
inline Status RndisSocket::send(const void* data, Size len, Size& sent) noexcept {
    sent = 0u;
    if (!open_) return Status::Unavailable;
    if (proto_ == Protocol::Udp) {
        const Status st = send_udp(remote_, data, len);
        if (st == Status::Ok) sent = len;
        return st;
    }
    Size off = 0u;
    const auto* p = static_cast<const uint8_t*>(data);
    while (off < len) {
        Size n = len - off;
        if (n > 536u) n = 536u;
        const Status st = send_tcp(0x18u, p + off, n);   // PSH|ACK
        if (st != Status::Ok) return st;
        off += n;
    }
    sent = off;
    return Status::Ok;
}
inline Status RndisSocket::send_udp(const Endpoint& dst, const void* data, Size len) noexcept {
    if (owner_ == nullptr || len > 1472u) return Status::InvalidArg;
    uint8_t mac[6]{};
    Status st = Status::Ok;
    if (dst.addr[0] == 255u && dst.addr[1] == 255u && dst.addr[2] == 255u &&
        dst.addr[3] == 255u) {
        for (uint8_t i = 0u; i < 6u; ++i) mac[i] = 0xFFu;
    } else {
        st = owner_->resolve_mac(dst.addr, mac, 1000u);
        if (st != Status::Ok) return st;
    }
    uint8_t udp[8 + 1472]{};
    udp[0] = static_cast<uint8_t>(local_port_ >> 8); udp[1] = static_cast<uint8_t>(local_port_);
    udp[2] = static_cast<uint8_t>(dst.port >> 8); udp[3] = static_cast<uint8_t>(dst.port);
    const uint16_t ulen = static_cast<uint16_t>(8u + len);
    udp[4] = static_cast<uint8_t>(ulen >> 8); udp[5] = static_cast<uint8_t>(ulen);
    const auto* p = static_cast<const uint8_t*>(data);
    for (Size i = 0u; i < len; ++i) udp[8u + i] = p[i];
    uint8_t ip[20 + 8 + 1472]{};
    ip[0] = 0x45u;
    const uint16_t total = static_cast<uint16_t>(20u + ulen);
    ip[2] = static_cast<uint8_t>(total >> 8); ip[3] = static_cast<uint8_t>(total);
    ip[4] = 0x12u; ip[5] = 0x34u; ip[6] = 0x40u; ip[8] = 64u; ip[9] = 17u;
    for (uint8_t i = 0u; i < 4u; ++i) ip[12u + i] = owner_->cfg_.ip[i];
    for (uint8_t i = 0u; i < 4u; ++i) ip[16u + i] = dst.addr[i];
    const uint16_t ic = RndisFactory::csum(ip, 20u);
    ip[10] = static_cast<uint8_t>(ic >> 8); ip[11] = static_cast<uint8_t>(ic);
    for (Size i = 0u; i < ulen; ++i) ip[20u + i] = udp[i];
    return owner_->emit(mac, 0x0800u, ip, total);
}
inline Status RndisSocket::send_to(const Endpoint& remote, const void* data, Size len,
                                   Size& sent) noexcept {
    sent = 0u;
    if (proto_ != Protocol::Udp) return Status::NotSupported;
    remote_ = remote;
    const Status st = send_udp(remote, data, len);
    if (st == Status::Ok) sent = len;
    return st;
}
inline Status RndisSocket::recv(void* data, Size cap, Size& received,
                                uint32_t timeout_ms) noexcept {
    received = 0u;
    if (!open_) return Status::Unavailable;
    if (proto_ == Protocol::Udp) {
        if (udp_len_ != 0u) {
            Size n = udp_len_ > cap ? cap : udp_len_;
            for (Size i = 0u; i < n; ++i) static_cast<uint8_t*>(data)[i] = udp_rx_[i];
            received = n; udp_len_ = 0u;
            return Status::Ok;
        }
        const bool timed_u = config::OsTimer::TimeAvailable();
        const uint64_t start_u = timed_u ? config::OsTimer::NowMs() : 0u;
        for (uint32_t i = 0u; i < 3000u; ++i) {
            (void)owner_->poll();
            if (udp_len_ != 0u) {
                Size n = udp_len_ > cap ? cap : udp_len_;
                for (Size k = 0u; k < n; ++k) static_cast<uint8_t*>(data)[k] = udp_rx_[k];
                received = n; udp_len_ = 0u;
                return Status::Ok;
            }
            if (timed_u && config::OsTimer::NowMs() - start_u >= timeout_ms) return Status::Timeout;
        }
        return Status::Timeout;
    }
    if (rx_pos_ < rx_len_) {
        Size n = rx_len_ - rx_pos_;
        if (n > cap) n = cap;
        for (Size i = 0u; i < n; ++i) static_cast<uint8_t*>(data)[i] = rx_[rx_pos_ + i];
        rx_pos_ += n; received = n;
        return Status::Ok;
    }
    const bool timed = config::OsTimer::TimeAvailable();
    const uint64_t start = timed ? config::OsTimer::NowMs() : 0u;
    for (uint32_t i = 0u; i < 5000u; ++i) {
        (void)owner_->poll();
        if (rx_pos_ < rx_len_) {
            Size n = rx_len_ - rx_pos_; if (n > cap) n = cap;
            for (Size k = 0u; k < n; ++k) static_cast<uint8_t*>(data)[k] = rx_[rx_pos_ + k];
            rx_pos_ += n; received = n; return Status::Ok;
        }
        if (state_ == State::ClosedByPeer) return Status::Unavailable;
        if (timed && config::OsTimer::NowMs() - start >= timeout_ms) return Status::Timeout;
    }
    return Status::Timeout;
}
inline Status RndisSocket::recv_from(Endpoint& remote, void* data, Size cap,
                                     Size& received, uint32_t timeout_ms) noexcept {
    const Status st = recv(data, cap, received, timeout_ms);
    if (st == Status::Ok) remote = udp_from_;
    return st;
}
inline void RndisSocket::on_ip(const uint8_t* ip, Size len) noexcept {
    if (ip == nullptr || len < 20u) return;
    const uint8_t proto = ip[9];
    const Size ihl = static_cast<Size>(ip[0] & 0x0Fu) * 4u;
    if (ihl < 20u || ihl > len) return;
    if (proto == 6u) on_tcp(ip + 12u, 0u, ip, len);
    else if (proto == 17u) on_udp(ip + 12u, 0u, ip, len);
}
inline void RndisSocket::on_udp(const uint8_t* src, uint16_t, const uint8_t* data, Size len) noexcept {
    if (data == nullptr || len < 20u || proto_ != Protocol::Udp) return;
    const uint8_t* udp = data + 20u;
    const Size ulen = len - 20u;
    if (ulen < 8u) return;
    const uint16_t dport = static_cast<uint16_t>((udp[2] << 8) | udp[3]);
    if (dport != local_port_) return;
    const uint16_t sport = static_cast<uint16_t>((udp[0] << 8) | udp[1]);
    const Size n = ulen - 8u;
    if (n > sizeof(udp_rx_)) return;
    for (Size i = 0u; i < n; ++i) udp_rx_[i] = udp[8u + i];
    udp_len_ = n;
    for (uint8_t i = 0u; i < 4u; ++i) udp_from_.addr[i] = data[12u + i];
    udp_from_.port = sport;
}
inline void RndisSocket::on_tcp(const uint8_t* src, uint16_t, const uint8_t* data, Size len) noexcept {
    if (data == nullptr || len < 40u || proto_ != Protocol::Tcp) return;
    const uint8_t* tcp = data + 20u;
    const Size tlen = len - 20u;
    if (tlen < 20u) return;
    const uint16_t dport = static_cast<uint16_t>((tcp[2] << 8) | tcp[3]);
    const uint16_t sport = static_cast<uint16_t>((tcp[0] << 8) | tcp[1]);
    if (dport != local_port_ || sport != remote_.port) return;
    const Size ihl = static_cast<Size>((tcp[12] >> 4) & 0x0Fu) * 4u;
    const uint8_t flags = tcp[13];
    const uint32_t seq = (static_cast<uint32_t>(tcp[4]) << 24) |
                         (static_cast<uint32_t>(tcp[5]) << 16) |
                         (static_cast<uint32_t>(tcp[6]) << 8) | tcp[7];
    const uint32_t ack = (static_cast<uint32_t>(tcp[8]) << 24) |
                         (static_cast<uint32_t>(tcp[9]) << 16) |
                         (static_cast<uint32_t>(tcp[10]) << 8) | tcp[11];
    if (state_ == State::SynSent && (flags & 0x12u) == 0x12u) {
        ack_ = seq + 1u;
        state_ = State::Established;
        (void)send_tcp(0x10u, nullptr, 0u);              // ACK
        return;
    }
    if ((flags & 0x01u) != 0u) {
        ack_ = seq + 1u + static_cast<uint32_t>(tlen - ihl);
        (void)send_tcp(0x10u, nullptr, 0u);
        state_ = State::ClosedByPeer;
        return;
    }
    const Size dlen = tlen - ihl;
    if (dlen > 0u) {
        ack_ = seq + static_cast<uint32_t>(dlen);
        if (dlen <= sizeof(rx_) && rx_pos_ >= rx_len_) {
            for (Size i = 0u; i < dlen; ++i) rx_[i] = tcp[ihl + i];
            rx_len_ = dlen; rx_pos_ = 0u;
        } else {
            ++dropped_;                               // backpressure: старый кадр цел
        }
        (void)send_tcp(0x10u, nullptr, 0u);
    }
    (void)ack;
}

} // namespace embed::net
