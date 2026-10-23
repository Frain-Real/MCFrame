// os/net/w5500.hpp
// ---------------------------------------------------------------------------
// W5500 (SPI Ethernet, hardware TCP/IP) — net::Factory для STM32.
//
// W5500 держит TCP/UDP-сокеты в железе, поэтому MQTT/HTTP/SNTP из os/net
// работают без lwIP и без RNDIS. Драйвер не знает STM32: он получает
// hal::SpiDevice и (опционально) пин сброса.
//
// Ограничение: у W5500 нет DNS. resolve() принимает IPv4-литерал; имя нужно
// разрешить снаружи и передать адрес.
// ---------------------------------------------------------------------------
#pragma once

#include "socket.hpp"
#include "../../hal/SpiDevice.hpp"
#include "../../hal/IGpio.hpp"
#include "../config/time_config.hpp"

namespace embed::net {

struct W5500Config {
    uint8_t  mac[6]  = {0x02, 0x4D, 0x43, 0x00, 0x00, 0x01};
    uint8_t  ip[4]   = {192, 168, 1, 50};
    uint8_t  gw[4]   = {192, 168, 1, 1};
    uint8_t  mask[4] = {255, 255, 255, 0};
    uint8_t  dns[4]  = {8, 8, 8, 8};
    uint16_t local_port_base = 40000u;
    bool     use_dhcp = false;
};

/// Низкоуровневый доступ к регистрам W5500 через SPI.
class W5500Core {
public:
    static constexpr uint8_t kCom = 0x00u;
    static constexpr uint8_t kSock(uint8_t s) noexcept { return static_cast<uint8_t>(s * 4u + 1u); }
    static constexpr uint8_t kTx(uint8_t s) noexcept { return static_cast<uint8_t>(s * 4u + 2u); }
    static constexpr uint8_t kRx(uint8_t s) noexcept { return static_cast<uint8_t>(s * 4u + 3u); }

    W5500Core(hal::ISpi& bus, hal::IGpioPin& cs,
              hal::IGpioPin* reset = nullptr,
              hal::IGpioPin* irq = nullptr) noexcept
        : spi_(bus, cs), reset_(reset), irq_(irq) {}

    Status init(const W5500Config& cfg) noexcept {
        cfg_ = cfg;
        hal::SpiConfig sc{};
        sc.hz = 8'000'000u;
        sc.mode = 0u;
        sc.msb_first = true;
        Status st = spi_.bus().configure(sc);
        if (st != Status::Ok) return st;

        if (reset_ != nullptr) {
            hal::GpioConfig gc{};
            gc.mode = hal::GpioMode::Output;
            gc.initial = true;
            (void)reset_->configure(gc);
            reset_->set(false);
            delay_ms(2u);
            reset_->set(true);
            delay_ms(10u);
        }

        st = write8(kCom, 0x0000u, 0x80u);          // software reset
        if (st != Status::Ok) return st;
        delay_ms(2u);
        uint8_t version = 0u;
        st = read8(kCom, 0x0039u, version);
        if (st != Status::Ok) return st;
        if (version != 0x04u) return Status::HardwareError;

        st = write(kCom, 0x0009u, cfg_.mac, 6u);    // SHAR
        if (st != Status::Ok) return st;
        st = write(kCom, 0x000Fu, cfg_.ip, 4u);     // SIPR
        if (st != Status::Ok) return st;
        st = write(kCom, 0x0001u, cfg_.gw, 4u);     // GAR
        if (st != Status::Ok) return st;
        st = write(kCom, 0x0005u, cfg_.mask, 4u);   // SUBR
        if (st != Status::Ok) return st;

        for (uint8_t s = 0u; s < 8u; ++s) {
            (void)write8(kSock(s), 0x001Fu, 2u);    // TX 2K
            (void)write8(kSock(s), 0x001Eu, 2u);    // RX 2K
            (void)write8(kSock(s), 0x0001u, 0x10u); // CLOSE
        }
        inited_ = true;
        return Status::Ok;
    }

    bool initialized() const noexcept { return inited_; }
    const W5500Config& config() const noexcept { return cfg_; }

    /// PHY link (W5500 PHYCFGR bit LNK).
    Status link_up(bool& up) noexcept {
        uint8_t v = 0u;
        const Status st = read8(kCom, 0x002Eu, v);
        up = (st == Status::Ok) && ((v & 0x01u) != 0u);
        return st;
    }

    Status wait_link(bool want_up, uint32_t timeout_ms) noexcept {
        const uint64_t start = config::OsTimer::TimeAvailable()
            ? config::OsTimer::NowMs() : 0u;
        uint32_t guard = 200000u;
        for (;;) {
            bool up = false;
            const Status st = link_up(up);
            if (st != Status::Ok) return st;
            if (up == want_up) return Status::Ok;
            if (config::OsTimer::TimeAvailable()) {
                if (config::OsTimer::NowMs() - start >= timeout_ms) return Status::Timeout;
            } else {
                if (--guard == 0u) return Status::Timeout;
            }
            delay_ms(10u);
        }
    }

    bool irq_pin() noexcept {
        return (irq_ != nullptr) && !irq_->get();   // W5500 INT active-low
    }

    Status set_network(const uint8_t ip[4], const uint8_t gw[4],
                       const uint8_t mask[4]) noexcept {
        if (ip == nullptr || gw == nullptr || mask == nullptr) return Status::InvalidArg;
        Status st = write(kCom, 0x000Fu, ip, 4u);
        if (st == Status::Ok) st = write(kCom, 0x0001u, gw, 4u);
        if (st == Status::Ok) st = write(kCom, 0x0005u, mask, 4u);
        if (st == Status::Ok) {
            for (uint8_t i = 0u; i < 4u; ++i) {
                cfg_.ip[i] = ip[i];
                cfg_.gw[i] = gw[i];
                cfg_.mask[i] = mask[i];
            }
        }
        return st;
    }

    Status read(uint8_t bsb, uint16_t addr, void* data, Size len) noexcept {
        auto* out = static_cast<uint8_t*>(data);
        while (len > 0u) {
            const Size chunk = (len > 256u) ? 256u : len;
            uint8_t tx[3 + 256]{};
            uint8_t rx[3 + 256]{};
            tx[0] = static_cast<uint8_t>((bsb << 3) | 0x00u);
            tx[1] = static_cast<uint8_t>(addr >> 8);
            tx[2] = static_cast<uint8_t>(addr);
            const Status st = spi_.transfer(tx, rx, 3u + chunk);
            if (st != Status::Ok) return st;
            for (Size i = 0u; i < chunk; ++i) out[i] = rx[3u + i];
            out += chunk;
            addr = static_cast<uint16_t>(addr + chunk);
            len -= chunk;
        }
        return Status::Ok;
    }

    Status write(uint8_t bsb, uint16_t addr, const void* data, Size len) noexcept {
        const auto* in = static_cast<const uint8_t*>(data);
        while (len > 0u) {
            const Size chunk = (len > 256u) ? 256u : len;
            uint8_t tx[3 + 256]{};
            tx[0] = static_cast<uint8_t>((bsb << 3) | 0x04u);
            tx[1] = static_cast<uint8_t>(addr >> 8);
            tx[2] = static_cast<uint8_t>(addr);
            for (Size i = 0u; i < chunk; ++i) tx[3u + i] = in[i];
            const Status st = spi_.transfer(tx, nullptr, 3u + chunk);
            if (st != Status::Ok) return st;
            in += chunk;
            addr = static_cast<uint16_t>(addr + chunk);
            len -= chunk;
        }
        return Status::Ok;
    }

    Status read8(uint8_t bsb, uint16_t addr, uint8_t& v) noexcept {
        return read(bsb, addr, &v, 1u);
    }
    Status write8(uint8_t bsb, uint16_t addr, uint8_t v) noexcept {
        return write(bsb, addr, &v, 1u);
    }
    Status read16(uint8_t bsb, uint16_t addr, uint16_t& v) noexcept {
        uint8_t b[2]{};
        const Status st = read(bsb, addr, b, 2u);
        v = static_cast<uint16_t>(static_cast<uint16_t>(b[0]) |
                                  (static_cast<uint16_t>(b[1]) << 8));
        return st;
    }
    Status write16(uint8_t bsb, uint16_t addr, uint16_t v) noexcept {
        const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
        return write(bsb, addr, b, 2u);
    }

private:
    static void delay_ms(uint32_t ms) noexcept {
        if (!config::OsTimer::TimeAvailable()) {
            for (volatile uint32_t i = 0u; i < ms * 8000u; ++i) { }
            return;
        }
        const uint64_t start = config::OsTimer::NowMs();
        while (config::OsTimer::NowMs() - start < ms) { }
    }

    hal::SpiDevice spi_;
    hal::IGpioPin* reset_ = nullptr;
    hal::IGpioPin* irq_ = nullptr;
    W5500Config cfg_{};
    bool inited_ = false;
};

/// Один аппаратный сокет W5500.
class W5500Socket final : public Socket {
public:
    W5500Socket() noexcept = default;

    Status start(W5500Core* core, uint8_t index, Protocol proto,
                 uint16_t local_port = 0u) noexcept {
        core_ = core;
        idx_ = index;
        proto_ = proto;
        local_port_ = local_port;
        open_ = false;
        const Status st = reset_socket();
        open_ = (st == Status::Ok);
        return st;
    }

    Status connect(const Endpoint& remote, uint32_t timeout_ms) noexcept override {
        if (!open_ || core_ == nullptr) return Status::Unavailable;
        Status st = set_dest(remote);
        if (st != Status::Ok) return st;
        if (proto_ == Protocol::Tcp) {
            st = command(0x04u);                    // CONNECT
            if (st != Status::Ok) return st;
            return wait_state(0x17u, timeout_ms);   // ESTABLISHED
        }
        return Status::Ok;                          // UDP: адрес уже записан
    }

    Status listen(uint16_t port) noexcept override {
        if (core_ == nullptr || proto_ != Protocol::Tcp) return Status::Unavailable;
        if (port != 0u) local_port_ = port;
        const uint16_t p = (local_port_ != 0u)
            ? local_port_
            : static_cast<uint16_t>(core_->config().local_port_base + idx_);
        Status st = core_->write16(W5500Core::kSock(idx_), 0x0004u, p);
        if (st != Status::Ok) return st;
        st = command(0x02u);                       // LISTEN
        if (st != Status::Ok) return st;
        st = wait_state(0x14u, 2000u);             // LISTEN
        open_ = (st == Status::Ok);
        return st;
    }

    Status accept(Socket*& out) noexcept override {
        out = nullptr;
        if (!open_ || core_ == nullptr || proto_ != Protocol::Tcp) {
            return Status::Unavailable;
        }
        uint8_t sr = 0u;
        const Status st = core_->read8(W5500Core::kSock(idx_), 0x0003u, sr);
        if (st != Status::Ok) return st;
        if (sr != 0x17u) return Status::Busy;      // ESTABLISHED
        // W5500 не имеет отдельного LISTEN-сокета: этот же аппаратный сокет
        // становится принятым соединением. TcpServer после close() заново
        // вызывает listen(); см. shell/net_shell.hpp.
        out = this;
        return Status::Ok;
    }

    Status send(const void* data, Size len, Size& sent) noexcept override {
        sent = 0u;
        if (!open_ || core_ == nullptr) return Status::Unavailable;
        const auto* p = static_cast<const uint8_t*>(data);
        Size off = 0u;
        while (off < len) {
            uint16_t free_size = 0u;
            Status st = core_->read16(W5500Core::kSock(idx_), 0x0020u, free_size);
            if (st != Status::Ok) return st;
            if (free_size == 0u) return (off > 0u) ? Status::Ok : Status::Busy;
            Size n = len - off;
            if (n > free_size) n = free_size;
            if (n > 2048u) n = 2048u;

            uint16_t wr = 0u;
            st = core_->read16(W5500Core::kSock(idx_), 0x0024u, wr);
            if (st != Status::Ok) return st;
            st = core_->write(W5500Core::kTx(idx_), wr, p + off, n);
            if (st != Status::Ok) return st;
            st = core_->write16(W5500Core::kSock(idx_), 0x0024u,
                                static_cast<uint16_t>(wr + n));
            if (st != Status::Ok) return st;
            // Старый SEND_OK не должен приниматься за новый.
            (void)core_->write8(W5500Core::kSock(idx_), 0x0002u, 0x20u);
            st = command(0x20u);                    // SEND
            if (st != Status::Ok) return st;
            st = wait_send_ok(1000u);
            if (st != Status::Ok) return st;
            off += n;
        }
        sent = off;
        return Status::Ok;
    }

    Status recv(void* data, Size cap, Size& received, uint32_t) noexcept override {
        received = 0u;
        if (!open_ || core_ == nullptr) return Status::Unavailable;
        uint16_t rsr = 0u;
        Status st = core_->read16(W5500Core::kSock(idx_), 0x0026u, rsr);
        if (st != Status::Ok) return st;
        if (rsr == 0u) {
            uint8_t sr = 0u;
            if (core_->read8(W5500Core::kSock(idx_), 0x0003u, sr) == Status::Ok &&
                (sr == 0x00u || sr == 0x1Cu)) {
                open_ = false;
                return Status::Unavailable;
            }
            return Status::Timeout;
        }
        Size n = (rsr > cap) ? cap : rsr;
        uint16_t rd = 0u;
        st = core_->read16(W5500Core::kSock(idx_), 0x0028u, rd);
        if (st != Status::Ok) return st;
        st = core_->read(W5500Core::kRx(idx_), rd, data, n);
        if (st != Status::Ok) return st;
        st = core_->write16(W5500Core::kSock(idx_), 0x0028u,
                            static_cast<uint16_t>(rd + n));
        if (st != Status::Ok) return st;
        st = command(0x40u);                        // RECV
        if (st != Status::Ok) return st;
        received = n;
        return Status::Ok;
    }

    Status send_to(const Endpoint& remote, const void* data, Size len,
                   Size& sent) noexcept override {
        if (proto_ != Protocol::Udp) return Status::NotSupported;
        const Status st = set_dest(remote);
        if (st != Status::Ok) return st;
        return send(data, len, sent);
    }

    Status recv_from(Endpoint& remote, void* data, Size cap,
                     Size& received, uint32_t timeout_ms) noexcept override {
        if (proto_ != Protocol::Udp) return Status::NotSupported;
        const Status st = recv(data, cap, received, timeout_ms);
        if (st != Status::Ok) return st;
        uint8_t ip[4]{};
        uint16_t port = 0u;
        (void)core_->read(W5500Core::kSock(idx_), 0x000Cu, ip, 4u);
        (void)core_->read16(W5500Core::kSock(idx_), 0x0010u, port);
        for (uint8_t i = 0u; i < 4u; ++i) remote.addr[i] = ip[i];
        remote.port = port;
        return Status::Ok;
    }

    void close() noexcept override {
        if (open_ && core_ != nullptr) {
            (void)core_->write8(W5500Core::kSock(idx_), 0x0001u, 0x10u);
        }
        open_ = false;
        // core_ сохраняется: listener можно снова открыть через listen().
    }
    bool open() const noexcept override { return open_; }

private:
    Status reset_socket() noexcept {
        if (core_ == nullptr) return Status::Unavailable;
        (void)core_->write8(W5500Core::kSock(idx_), 0x0001u, 0x10u);   // CLOSE
        delay();
        const uint8_t mr = (proto_ == Protocol::Tcp) ? 0x01u : 0x02u;
        Status st = core_->write8(W5500Core::kSock(idx_), 0x0000u, mr);
        if (st != Status::Ok) return st;
        const uint16_t port = (local_port_ != 0u)
            ? local_port_
            : static_cast<uint16_t>(core_->config().local_port_base + idx_);
        st = core_->write16(W5500Core::kSock(idx_), 0x0004u, port);
        if (st != Status::Ok) return st;
        st = command(0x01u);                                        // OPEN
        if (st != Status::Ok) return st;
        return wait_state(0x13u, 1000u);                            // INIT
    }
    Status set_dest(const Endpoint& remote) noexcept {
        Status st = core_->write(W5500Core::kSock(idx_), 0x000Cu, remote.addr, 4u);
        if (st != Status::Ok) return st;
        return core_->write16(W5500Core::kSock(idx_), 0x0010u, remote.port);
    }
    Status command(uint8_t cmd) noexcept {
        return core_->write8(W5500Core::kSock(idx_), 0x0001u, cmd);
    }
    Status wait_state(uint8_t want, uint32_t timeout_ms) noexcept {
        const bool timed = config::OsTimer::TimeAvailable();
        const uint64_t start = timed ? config::OsTimer::NowMs() : 0u;
        uint32_t guard = 2000000u;
        for (;;) {
            uint8_t sr = 0u;
            const Status st = core_->read8(W5500Core::kSock(idx_), 0x0003u, sr);
            if (st != Status::Ok) return st;
            if (sr == want) return Status::Ok;
            if (sr == 0x00u) return Status::Unavailable;
            if (timed) {
                if (config::OsTimer::NowMs() - start > timeout_ms) return Status::Timeout;
            } else if (--guard == 0u) {
                return Status::Timeout;
            }
        }
    }
    Status wait_send_ok(uint32_t timeout_ms) noexcept {
        const bool timed = config::OsTimer::TimeAvailable();
        const uint64_t start = timed ? config::OsTimer::NowMs() : 0u;
        uint32_t guard = 200000u;
        for (;;) {
            uint8_t ir = 0u;
            const Status st = core_->read8(W5500Core::kSock(idx_), 0x0002u, ir);
            if (st != Status::Ok) return st;
            if ((ir & 0x20u) != 0u) {               // SEND_OK
                (void)core_->write8(W5500Core::kSock(idx_), 0x0002u, 0x20u);
                return Status::Ok;
            }
            if (timed) {
                if (config::OsTimer::NowMs() - start >= timeout_ms) return Status::Timeout;
            } else if (--guard == 0u) {
                return Status::Timeout;
            }
        }
    }
    static void delay() noexcept {
        for (volatile uint32_t i = 0u; i < 8000u; ++i) { }
    }

    W5500Core* core_ = nullptr;
    Protocol proto_ = Protocol::Tcp;
    uint8_t idx_ = 0u;
    uint16_t local_port_ = 0u;
    bool open_ = false;
};


// ─────────────────────────── DHCP / DNS ─────────────────────────────────────

struct DhcpLease {
    uint8_t ip[4]{};
    uint8_t gw[4]{};
    uint8_t mask[4]{};
    uint8_t dns[4]{};
    uint8_t server[4]{};
    uint32_t lease_s = 0u;
};

/// DHCP-клиент поверх W5500 UDP. Работает на выделенном слоте 7 (порт 68),
/// поэтому не занимает пул приложения.
class DhcpClient {
public:
    Status acquire(W5500Core& core, const uint8_t mac[6],
                   W5500Config& cfg, uint32_t timeout_ms) noexcept {
        if (mac == nullptr) return Status::InvalidArg;
        W5500Socket sock;
        Status st = sock.start(&core, 7u, Protocol::Udp, 68u);
        if (st != Status::Ok) return st;

        const uint32_t xid = 0x4D434631u ^ static_cast<uint32_t>(now_ms());
        uint8_t pkt[300]{};
        Size n = build_discover(pkt, sizeof(pkt), xid, mac);
        if (n == 0u) { sock.close(); return Status::NoMemory; }
        Endpoint bcast{255, 255, 255, 255, 67u};
        Size sent = 0u;
        st = sock.send_to(bcast, pkt, n, sent);
        if (st != Status::Ok) { sock.close(); return st; }

        DhcpLease lease{};
        bool offered = false;
        const uint64_t start = now_ms();
        while (elapsed(start) < timeout_ms) {
            Endpoint from{};
            uint8_t rx[512]{};
            Size got = 0u;
            st = sock.recv_from(from, rx, sizeof(rx), got, 500u);
            if (st == Status::Timeout) continue;
            if (st != Status::Ok) { sock.close(); return st; }
            uint8_t type = 0u;
            if (!parse_reply(rx, got, xid, type, lease)) continue;
            if (type == 2u) { offered = true; break; }   // OFFER
            if (type == 5u) { offered = true; break; }   // ACK без OFFER
        }
        if (!offered) { sock.close(); return Status::Timeout; }

        if (lease.ip[0] == 0u && lease.ip[1] == 0u &&
            lease.ip[2] == 0u && lease.ip[3] == 0u) {
            sock.close();
            return Status::NotFound;
        }
        // REQUEST выбранного адреса.
        n = build_request(pkt, sizeof(pkt), xid, mac, lease);
        if (n == 0u) { sock.close(); return Status::NoMemory; }
        st = sock.send_to(bcast, pkt, n, sent);
        if (st != Status::Ok) { sock.close(); return st; }

        bool acked = false;
        while (elapsed(start) < timeout_ms) {
            Endpoint from{};
            uint8_t rx[512]{};
            Size got = 0u;
            st = sock.recv_from(from, rx, sizeof(rx), got, 500u);
            if (st == Status::Timeout) continue;
            if (st != Status::Ok) { sock.close(); return st; }
            uint8_t type = 0u;
            DhcpLease a{};
            if (!parse_reply(rx, got, xid, type, a)) continue;
            if (type == 5u) { lease = a; acked = true; break; }
        }
        sock.close();
        if (!acked) return Status::Timeout;

        const Status ns = core.set_network(lease.ip, lease.gw, lease.mask);
        if (ns != Status::Ok) return ns;
        bool have_dns = false;
        for (uint8_t i = 0u; i < 4u; ++i) {
            cfg.ip[i] = lease.ip[i];
            cfg.gw[i] = lease.gw[i];
            cfg.mask[i] = lease.mask[i];
            if (lease.dns[i] != 0u) have_dns = true;
        }
        if (have_dns) {
            for (uint8_t i = 0u; i < 4u; ++i) cfg.dns[i] = lease.dns[i];
        }
        return Status::Ok;
    }

    static Size build_discover(uint8_t* out, Size cap, uint32_t xid,
                               const uint8_t mac[6]) noexcept {
        return build(out, cap, xid, mac, 1u, nullptr);
    }
    static Size build_request(uint8_t* out, Size cap, uint32_t xid,
                              const uint8_t mac[6], const DhcpLease& lease) noexcept {
        return build(out, cap, xid, mac, 3u, &lease);
    }

    /// Разобрать DHCP OFFER/ACK. type — 2/5; lease заполняется частично.
    static bool parse_reply(const uint8_t* pkt, Size len, uint32_t xid,
                            uint8_t& type, DhcpLease& lease) noexcept {
        if (pkt == nullptr || len < 240u || pkt[0] != 2u || pkt[1] != 1u || pkt[2] != 6u) {
            return false;
        }
        if (rd32(pkt + 4u) != xid) return false;
        const uint32_t cookie = rd32be(pkt + 236u);
        if (cookie != 0x63825363u) return false;
        for (uint8_t i = 0u; i < 4u; ++i) lease.ip[i] = pkt[16u + i];
        Size pos = 240u;
        bool have_type = false;
        while (pos < len) {
            const uint8_t code = pkt[pos++];
            if (code == 0u) continue;                  // pad
            if (code == 255u) break;                   // end
            if (pos >= len) return false;
            const uint8_t olen = pkt[pos++];
            if (pos + olen > len) return false;
            if (code == 53u && olen >= 1u) {
                type = pkt[pos];
                have_type = true;
            } else if (code == 1u && olen >= 4u) {
                for (uint8_t i = 0u; i < 4u; ++i) lease.mask[i] = pkt[pos + i];
            } else if (code == 3u && olen >= 4u) {
                for (uint8_t i = 0u; i < 4u; ++i) lease.gw[i] = pkt[pos + i];
            } else if (code == 6u && olen >= 4u) {
                for (uint8_t i = 0u; i < 4u; ++i) lease.dns[i] = pkt[pos + i];
            } else if (code == 54u && olen >= 4u) {
                for (uint8_t i = 0u; i < 4u; ++i) lease.server[i] = pkt[pos + i];
            } else if (code == 51u && olen >= 4u) {
                lease.lease_s = rd32be(pkt + pos);
            }
            pos = static_cast<Size>(pos + olen);
        }
        return have_type && (type == 2u || type == 5u);
    }

private:
    static uint32_t now_ms() noexcept {
        return config::OsTimer::TimeAvailable()
            ? static_cast<uint32_t>(config::OsTimer::NowMs()) : 0u;
    }
    static uint32_t elapsed(uint64_t start) noexcept {
        return config::OsTimer::TimeAvailable()
            ? static_cast<uint32_t>(config::OsTimer::NowMs() - start) : 0xFFFFFFFFu;
    }
    static uint32_t rd32be(const uint8_t* p) noexcept {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }
    static uint32_t rd32(const uint8_t* p) noexcept {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
    static void put32(uint8_t* p, uint32_t v) noexcept {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    }
    static Size build(uint8_t* out, Size cap, uint32_t xid, const uint8_t mac[6],
                      uint8_t type, const DhcpLease* lease) noexcept {
        if (out == nullptr || cap < 280u || mac == nullptr) return 0u;
        for (Size i = 0u; i < 240u; ++i) out[i] = 0u;
        out[0] = 1u; out[1] = 1u; out[2] = 6u;
        put32(out + 4u, xid);
        out[10] = 0x80u; out[11] = 0x00u;             // broadcast
        for (uint8_t i = 0u; i < 6u; ++i) out[28u + i] = mac[i];
        put32(out + 236u, 0x63825363u);
        Size n = 240u;
        out[n++] = 53u; out[n++] = 1u; out[n++] = type;
        if (lease != nullptr) {
            out[n++] = 50u; out[n++] = 4u;
            for (uint8_t i = 0u; i < 4u; ++i) out[n++] = lease->ip[i];
            out[n++] = 54u; out[n++] = 4u;
            for (uint8_t i = 0u; i < 4u; ++i) out[n++] = lease->server[i];
        }
        out[n++] = 55u; out[n++] = 3u; out[n++] = 1u; out[n++] = 3u; out[n++] = 6u;
        out[n++] = 255u;
        return n;
    }
};

/// DNS-клиент поверх W5500 UDP. Поддерживает A-запросы и сжатые имена.
class DnsClient {
public:
    Status resolve(W5500Core& core, const uint8_t dns[4],
                   const char* host, Endpoint& out) noexcept {
        if (host == nullptr || host[0] == '\0') return Status::InvalidArg;
        uint8_t ip[4]{};
        if (parse_ipv4(host, ip)) {
            for (uint8_t i = 0u; i < 4u; ++i) out.addr[i] = ip[i];
            return Status::Ok;
        }
        W5500Socket sock;
        Status st = sock.start(&core, 6u, Protocol::Udp);
        if (st != Status::Ok) return st;
        uint8_t q[260]{};
        const Size qn = build_query(q, sizeof(q), host);
        if (qn == 0u) { sock.close(); return Status::InvalidArg; }
        Endpoint server{};
        if (dns == nullptr) { sock.close(); return Status::InvalidArg; }
        for (uint8_t i = 0u; i < 4u; ++i) server.addr[i] = dns[i];
        server.port = 53u;
        Size sent = 0u;
        st = sock.send_to(server, q, qn, sent);
        if (st != Status::Ok) { sock.close(); return st; }
        const uint64_t start = config::OsTimer::TimeAvailable()
            ? config::OsTimer::NowMs() : 0u;
        for (uint8_t attempt = 0u; attempt < 8u; ++attempt) {
            Endpoint from{};
            uint8_t rx[512]{};
            Size got = 0u;
            st = sock.recv_from(from, rx, sizeof(rx), got, 1000u);
            if (st == Status::Timeout) {
                if (config::OsTimer::TimeAvailable() &&
                    config::OsTimer::NowMs() - start > 5000u) break;
                continue;
            }
            if (st != Status::Ok) { sock.close(); return st; }
            if (parse_answer(rx, got, ip)) {
                sock.close();
                for (uint8_t i = 0u; i < 4u; ++i) out.addr[i] = ip[i];
                return Status::Ok;
            }
        }
        sock.close();
        return Status::Timeout;
    }

    /// Разобрать первый A-record. Compression pointers поддерживаются.
    static bool parse_answer(const uint8_t* pkt, Size len, uint8_t ip[4]) noexcept {
        if (pkt == nullptr || len < 12u || ip == nullptr) return false;
        if (pkt[0] != 0x12u || pkt[1] != 0x34u) return false;   // transaction id
        const uint16_t qd = static_cast<uint16_t>((pkt[4] << 8) | pkt[5]);
        const uint16_t an = static_cast<uint16_t>((pkt[6] << 8) | pkt[7]);
        if (an == 0u) return false;
        Size pos = 12u;
        for (uint16_t i = 0u; i < qd; ++i) {
            if (!skip_name(pkt, len, pos)) return false;
            if (pos + 4u > len) return false;
            pos += 4u;
        }
        for (uint16_t i = 0u; i < an; ++i) {
            if (!skip_name(pkt, len, pos)) return false;
            if (pos + 10u > len) return false;
            const uint16_t type = static_cast<uint16_t>((pkt[pos] << 8) | pkt[pos + 1u]);
            const uint16_t clazz = static_cast<uint16_t>((pkt[pos + 2u] << 8) | pkt[pos + 3u]);
            const uint16_t rdlen = static_cast<uint16_t>((pkt[pos + 8u] << 8) | pkt[pos + 9u]);
            pos += 10u;
            if (pos + rdlen > len) return false;
            if (type == 1u && clazz == 1u && rdlen == 4u) {
                for (uint8_t k = 0u; k < 4u; ++k) ip[k] = pkt[pos + k];
                return true;
            }
            pos = static_cast<Size>(pos + rdlen);
        }
        return false;
    }

private:
    static Size build_query(uint8_t* out, Size cap, const char* host) noexcept {
        if (out == nullptr || cap < 18u || host == nullptr) return 0u;
        for (Size i = 0u; i < 12u; ++i) out[i] = 0u;
        out[0] = 0x12u; out[1] = 0x34u;
        out[2] = 0x01u; out[3] = 0x00u;             // RD
        out[4] = 0x00u; out[5] = 0x01u;             // QDCOUNT
        Size n = 12u;
        Size label = 0u;
        for (const char* p = host; ; ++p) {
            if (*p == '.' || *p == '\0') {
                if (label == 0u || label > 63u || n + 1u + label + 1u > cap) return 0u;
                out[n++] = static_cast<uint8_t>(label);
                for (Size i = 0u; i < label; ++i) out[n++] = static_cast<uint8_t>(p[i - label]);
                label = 0u;
                if (*p == '\0') break;
            } else {
                ++label;
                if (label > 63u) return 0u;
            }
        }
        if (n + 5u > cap) return 0u;
        out[n++] = 0u;
        out[n++] = 0x00u; out[n++] = 0x01u;         // QTYPE A
        out[n++] = 0x00u; out[n++] = 0x01u;         // QCLASS IN
        return n;
    }

    static bool skip_name(const uint8_t* pkt, Size len, Size& pos) noexcept {
        for (uint8_t guard = 0u; guard < 64u; ++guard) {
            if (pos >= len) return false;
            const uint8_t b = pkt[pos++];
            if (b == 0u) return true;
            if ((b & 0xC0u) == 0xC0u) {
                if (pos >= len) return false;
                ++pos;
                return true;
            }
            if ((b & 0xC0u) != 0u || pos + b > len) return false;
            pos = static_cast<Size>(pos + b);
        }
        return false;
    }
};


/// Фабрика W5500: до 8 аппаратных сокетов.
template <Size MaxSockets = 4u>
class W5500FactoryT final : public Factory {
public:
    static_assert(MaxSockets >= 1u && MaxSockets <= 6u,
                  "W5500: 1..6 sockets (слоты 6/7 — DNS/DHCP)");

    W5500FactoryT(hal::ISpi& bus, hal::IGpioPin& cs,
                  hal::IGpioPin* reset = nullptr,
                  hal::IGpioPin* irq = nullptr) noexcept
        : core_(bus, cs, reset, irq) {}

    Status init(const W5500Config& cfg) noexcept {
        cfg_ = cfg;
        if (cfg_.use_dhcp) {
            for (uint8_t i = 0u; i < 4u; ++i) { cfg_.ip[i] = 0u; cfg_.gw[i] = 0u; }
        }
        Status st = core_.init(cfg_);
        if (st != Status::Ok) return st;
        if (cfg_.use_dhcp) {
            st = core_.wait_link(true, 5000u);
            if (st != Status::Ok) return st;
            st = dhcp_.acquire(core_, cfg_.mac, cfg_, 10000u);
            if (st != Status::Ok) return st;
            return core_.set_network(cfg_.ip, cfg_.gw, cfg_.mask);
        }
        return Status::Ok;
    }

    Status create(Protocol proto, Socket*& out) noexcept override {
        if (!core_.initialized()) return Status::Unavailable;
        for (uint8_t i = 0u; i < MaxSockets; ++i) {
            if (!pool_[i].open()) {
                const Status st = pool_[i].start(&core_, i, proto);
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
        return dns_.resolve(core_, cfg_.dns, host, out);
    }

    Status link_up(bool& up) noexcept { return core_.link_up(up); }
    Status wait_link(bool up, uint32_t timeout_ms) noexcept {
        return core_.wait_link(up, timeout_ms);
    }
    bool irq_pending() noexcept {
        if (core_.irq_pin()) return true;
        for (uint8_t i = 0u; i < 8u; ++i) {
            uint8_t v = 0u;
            if (core_.read8(W5500Core::kSock(i), 0x0002u, v) == Status::Ok && v != 0u) {
                return true;
            }
        }
        return false;
    }

    Status dhcp(uint32_t timeout_ms) noexcept {
        return dhcp_.acquire(core_, cfg_.mac, cfg_, timeout_ms);
    }

private:
    W5500Core core_;
    W5500Socket pool_[MaxSockets];
    W5500Config cfg_{};
    DhcpClient dhcp_;
    DnsClient dns_;
};

template <Size N = 4u>
using W5500Factory = W5500FactoryT<N>;

} // namespace embed::net
