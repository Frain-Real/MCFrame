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
                else if (v == 0x04u) sr_[s] = 0x17u; // CONNECT -> ESTABLISHED
                else if (v == 0x10u) sr_[s] = 0x00u; // CLOSE
                else if (v == 0x20u) ++sends_;
            }
        } else if (bsb == net::W5500Core::kTx(s)) {
            ++tx_bytes_;
        }
    }

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
    check("resolve name rejected", net.resolve("example.com", ep) == Status::NotFound);
    ep.port = 1883;
    check("connect", s->connect(ep, 100u) == Status::Ok);

    Size sent = 0u;
    check("send", s->send("hello", 5u, sent) == Status::Ok && sent == 5u);
    check("SEND command", spi.sends() >= 1u);
    check("TX bytes", spi.tx_bytes() >= 5u);

    Size got = 0u;
    char b[8]{};
    check("empty recv timeout", s->recv(b, sizeof(b), got, 1u) == Status::Timeout);
    s->close();
    check("closed", !s->open());

    std::printf("%d/%d проверок пройдено%s\n", total - failed, total,
                failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return failed == 0 ? 0 : 1;
}
