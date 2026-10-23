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
#include "dhcp.hpp"
#include "dns.hpp"
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
// Кодеки/клиенты вынесены в os/net/{dhcp,dns}.hpp; здесь только адаптеры
// к выделенным аппаратным сокетам W5500 (7 — DHCP, 6 — DNS).

class W5500DhcpClient {
public:
    Status acquire(W5500Core& core, const uint8_t mac[6],
                   W5500Config& cfg, uint32_t timeout_ms) noexcept {
        DhcpLease lease{};
        Status st = run(core, mac, lease, timeout_ms, false);
        if (st != Status::Ok) return st;
        const Status ns = core.set_network(lease.ip, lease.gw, lease.mask);
        if (ns != Status::Ok) return ns;
        apply(cfg, lease);
        lease_ = lease;
        return Status::Ok;
    }

    Status renew(W5500Core& core, const uint8_t mac[6], W5500Config& cfg,
                 uint32_t timeout_ms) noexcept {
        DhcpLease lease = lease_;
        if (lease.lease_s == 0u) return acquire(core, mac, cfg, timeout_ms);
        Status st = run(core, mac, lease, timeout_ms, true);
        if (st != Status::Ok) return st;
        const Status ns = core.set_network(lease.ip, lease.gw, lease.mask);
        if (ns != Status::Ok) return ns;
        apply(cfg, lease);
        lease_ = lease;
        return Status::Ok;
    }

    const DhcpLease& lease() const noexcept { return lease_; }
    bool valid() const noexcept { return lease_.lease_s != 0u; }

private:
    static void apply(W5500Config& cfg, const DhcpLease& lease) noexcept {
        bool have_dns = false;
        for (uint8_t i = 0u; i < 4u; ++i) {
            cfg.ip[i] = lease.ip[i];
            cfg.gw[i] = lease.gw[i];
            cfg.mask[i] = lease.mask[i];
            if (lease.dns[i] != 0u) have_dns = true;
        }
        if (have_dns) for (uint8_t i = 0u; i < 4u; ++i) cfg.dns[i] = lease.dns[i];
    }

    Status run(W5500Core& core, const uint8_t mac[6], DhcpLease& lease,
               uint32_t timeout_ms, bool renew) noexcept {
        W5500Socket sock;
        Status st = sock.start(&core, 7u, Protocol::Udp, 68u);
        if (st != Status::Ok) return st;
        DhcpClient codec;
        st = renew ? codec.renew(sock, mac, lease, timeout_ms)
                   : codec.acquire(sock, mac, lease, timeout_ms);
        sock.close();
        return st;
    }

    DhcpLease lease_{};
};

class W5500DnsClient {
public:
    Status resolve(W5500Core& core, const uint8_t dns[4],
                   const char* host, Endpoint& out) noexcept {
        W5500Socket sock;
        Status st = sock.start(&core, 6u, Protocol::Udp);
        if (st != Status::Ok) return st;
        DnsClient codec;
        st = codec.resolve(sock, dns, host, out);
        sock.close();
        return st;
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
    Status renew_dhcp(uint32_t timeout_ms) noexcept {
        return dhcp_.renew(core_, cfg_.mac, cfg_, timeout_ms);
    }
    const DhcpLease& lease() const noexcept { return dhcp_.lease(); }
    bool dhcp_valid() const noexcept { return dhcp_.valid(); }

    /// Периодический вызов из приложения: T1 renewal и expiry.
    /// Возврат Timeout — lease истёк, сеть нужно поднимать заново.
    Status dhcp_poll(uint32_t timeout_ms = 5000u) noexcept {
        if (!cfg_.use_dhcp || !dhcp_.valid()) return Status::Ok;
        if (!config::OsTimer::TimeAvailable()) return Status::Ok;
        const uint64_t now = config::OsTimer::NowMs();
        const DhcpLease& l = dhcp_.lease();
        if (DhcpClient::remaining_ms(l, now) == 0u) {
            for (uint8_t i = 0u; i < 4u; ++i) cfg_.ip[i] = 0u;
            return Status::Timeout;
        }
        const uint32_t t1 = l.t1_s != 0u ? l.t1_s
                           : (l.lease_s != 0u ? l.lease_s / 2u : 0u);
        if (t1 != 0u && now - l.obtained_ms >= static_cast<uint64_t>(t1) * 1000u) {
            return renew_dhcp(timeout_ms);
        }
        return Status::Ok;
    }

private:
    W5500Core core_;
    W5500Socket pool_[MaxSockets];
    W5500Config cfg_{};
    W5500DhcpClient dhcp_;
    W5500DnsClient dns_;
};

template <Size N = 4u>
using W5500Factory = W5500FactoryT<N>;

} // namespace embed::net
