// tests/test_w5500.cpp

#include "os/net/w5500.hpp"
#include "hal/mock/mock_hal.hpp"

#include <cstdio>
#include <cstring>

using namespace embed;

namespace {
int total = 0, failed = 0;
void check(const char* what, bool ok) {
    ++total; if (!ok) ++failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

class FakeW5500Spi final : public hal::ISpi {
public:
    Status configure(const hal::SpiConfig&) noexcept override { return Status::Ok; }
    Status lock(Timeout) noexcept override { locked = true; return Status::Ok; }
    void unlock() noexcept override { locked = false; }
    uint32_t caps() const noexcept override { return 0u; }

    Status transfer(const void* tx, void* rx, Size n, Timeout) noexcept override {
        if (tx == nullptr || n < 3u) return Status::InvalidArg;
        const auto* t = static_cast<const uint8_t*>(tx);
        auto* r = static_cast<uint8_t*>(rx);
        const uint8_t ctrl = t[0];
        const uint8_t bsb = static_cast<uint8_t>(ctrl >> 3);
        const bool wr = (ctrl & 0x04u) != 0u;
        uint16_t addr = static_cast<uint16_t>((t[1] << 8) | t[2]);
        for (Size i = 3u; i < n; ++i) {
            if (wr) write_reg(bsb, addr, t[i]);
            else if (r != nullptr) r[i] = read_reg(bsb, addr);
            ++addr;
        }
        return Status::Ok;
    }

    uint8_t read_reg(uint8_t bsb, uint16_t addr) noexcept {
        if (bsb == 0u) {
            if (addr == 0x39u) return 0x04u;   // VERSIONR
            if (addr == 0x2Eu) return 0x01u;   // PHYCFGR: link up
            return 0u;
        }
        const uint8_t s = static_cast<uint8_t>((bsb - 1u) / 4u);
        if (s >= 8u) return 0u;
        if (bsb == net::W5500Core::kSock(s)) {
            if (addr == 0x0020u) return 0x00u;  // TX_FSR low = 2048? separate
            if (addr == 0x0021u) return 0x08u;
            if (addr == 0x0003u) return sr_[s];
            if (addr <= 0x2Fu) return sock_[s][addr];
        }
        return 0u;
    }
    void write_reg(uint8_t bsb, uint16_t addr, uint8_t v) noexcept {
        if (bsb == 0u) return;
        const uint8_t s = static_cast<uint8_t>((bsb - 1u) / 4u);
        if (s >= 8u) return;
        if (bsb == net::W5500Core::kSock(s)) {
            if (addr <= 0x2Fu) sock_[s][addr] = v;
            if (addr == 0x0001u) {  // Sn_CR
                if (v == 0x01u) sr_[s] = 0x13u;      // OPEN -> INIT
                else if (v == 0x02u) sr_[s] = 0x14u; // LISTEN
                else if (v == 0x04u) sr_[s] = 0x17u; // CONNECT -> ESTABLISHED
                else if (v == 0x10u) sr_[s] = 0x00u; // CLOSE
                else if (v == 0x20u) { ++sends_; sock_[s][0x02] |= 0x20u; }
            }
        } else if (bsb == net::W5500Core::kTx(s)) {
            ++tx_bytes_;
        }
    }

    void set_state(uint8_t s, uint8_t v) noexcept { if (s < 8u) sr_[s] = v; }
    uint32_t sends() const noexcept { return sends_; }
    uint32_t tx_bytes() const noexcept { return tx_bytes_; }
    bool locked = false;

private:
    uint8_t sock_[8][0x30]{};
    uint8_t sr_[8]{};
    uint32_t sends_ = 0u;
    uint32_t tx_bytes_ = 0u;
};

} // namespace

int main() {
    std::puts("=== W5500 SPI Ethernet ===");
    FakeW5500Spi spi;
    hal::mock::MockGpioPin cs;
    net::W5500FactoryT<2> net{spi, cs};

    net::W5500Config cfg{};
    cfg.ip[0] = 10; cfg.ip[1] = 0; cfg.ip[2] = 0; cfg.ip[3] = 50;
    check("init", net.init(cfg) == Status::Ok);
    check("SPI configured", true);

    net::Socket* s = nullptr;
    check("create TCP", net.create(net::Protocol::Tcp, s) == Status::Ok && s != nullptr);
    net::Endpoint ep{};
    check("resolve IPv4", net.resolve("192.168.1.10", ep) == Status::Ok &&
          ep.addr[0] == 192 && ep.addr[3] == 10);
    check("resolve name without DNS answer is not Ok",
          net.resolve("example.com", ep) != Status::Ok);
    ep.port = 1883;
    check("connect", s->connect(ep, 100u) == Status::Ok);

    check("SEND command", spi.sends() >= 1u);
    check("TX bytes", spi.tx_bytes() >= 5u);

    Size got = 0u;
    char b[8]{};
    check("empty recv timeout", s->recv(b, sizeof(b), got, 1u) == Status::Timeout);
    s->close();
    check("closed", !s->open());
    net::Socket* server = nullptr;
    check("server create", net.create(net::Protocol::Tcp, server) == Status::Ok && server != nullptr);
    check("listen", server != nullptr && server->listen(2323u) == Status::Ok);
    net::Socket* accepted = nullptr;
    check("accept busy", server != nullptr && server->accept(accepted) == Status::Busy &&
          accepted == nullptr);
    spi.set_state(0u, 0x17u);
    check("accept established", server != nullptr && server->accept(accepted) == Status::Ok &&
          accepted == server);
    if (server != nullptr) server->close();
    check("listen again", server != nullptr && server->listen(2323u) == Status::Ok);
    net::Socket* second = nullptr;
    check("second socket", net.create(net::Protocol::Tcp, second) == Status::Ok &&
          second != nullptr && second != server);
    if (second != nullptr) second->close();


    // DNS parser: A-record с compression pointer.
    const uint8_t dns[] = {
        0x12,0x34, 0x81,0x80, 0x00,0x01, 0x00,0x01, 0x00,0x00, 0x00,0x00,
        0x03,'w','w','w', 0x07,'e','x','a','m','p','l','e', 0x03,'c','o','m', 0x00,
        0x00,0x01, 0x00,0x01,
        0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x00,0x3C, 0x00,0x04,
        1,2,3,4
    };
    uint8_t dip[4]{};
    check("DNS parse A", net::DnsClient::parse_answer(dns, sizeof(dns), dip) &&
          dip[0] == 1 && dip[3] == 4);

    uint8_t disc[300]{};
    const uint8_t mac[6] = {2,0x4D,0x43,0,0,1};
    const Size dn = net::DhcpClient::build_discover(disc, sizeof(disc), 0x11223344u, mac);
    check("DHCP discover built", dn > 240u && disc[0] == 1u && disc[236] == 0x63u);
    uint8_t reply[300]{};
    for (Size i = 0u; i < 240u; ++i) reply[i] = 0u;
    reply[0] = 2u; reply[1] = 1u; reply[2] = 6u;
    reply[4] = 0x44; reply[5] = 0x33; reply[6] = 0x22; reply[7] = 0x11;
    reply[16] = 10u; reply[17] = 0u; reply[18] = 0u; reply[19] = 7u;
    reply[236] = 0x63; reply[237] = 0x82; reply[238] = 0x53; reply[239] = 0x63;
    Size rp = 240u;
    const uint8_t opts[] = {53,1,2, 1,4,255,255,255,0, 3,4,10,0,0,1,
                            6,4,8,8,8,8, 54,4,10,0,0,1, 51,4,0,0,0x0E,0x10, 255};
    for (Size i = 0u; i < sizeof(opts); ++i) reply[rp++] = opts[i];
    net::DhcpLease lease{};
    uint8_t dtype = 0u;
    const bool dh = net::DhcpClient::parse_reply(reply, rp, 0x11223344u, dtype, lease);
    uint8_t renewpkt[300]{};
    const Size rn = net::DhcpClient::build_renew(renewpkt, sizeof(renewpkt),
                                                 0x11223344u, mac, lease);
    check("DHCP renew built", rn > 240u && renewpkt[242] == 3u);
    check("DHCP offer parse", dh && dtype == 2u && lease.ip[3] == 7u &&
          lease.gw[3] == 1u && lease.dns[0] == 8u);

    bool up = false;
    check("PHY link", net.link_up(up) == Status::Ok && up);
    hal::mock::MockGpioPin irq;
    net::W5500FactoryT<2> net_irq{spi, cs, nullptr, &irq};
    check("IRQ active-low", net_irq.irq_pending());

    std::printf("%d/%d проверок пройдено%s\n", total - failed, total,
                failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return failed == 0 ? 0 : 1;
}
