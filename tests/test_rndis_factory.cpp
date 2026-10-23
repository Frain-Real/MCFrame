// tests/test_rndis_factory.cpp — compile/smoke for RNDIS net::Factory.
#include "os/net/rndis_factory.hpp"
#include "hal/mock/mock_hal.hpp"

#include <cstdio>

using namespace embed;

namespace {
class Hal final : public usb::Hal {
public:
    Status ep0_write(const void*, Size) noexcept override { return Status::Ok; }
    Status ep0_read(void*, Size, Size& got) noexcept override { got = 0u; return Status::Timeout; }
    Status ep_write(uint8_t ep, const void* d, Size n) noexcept override {
        ep_ = ep; len_ = n;
        if (n <= sizeof(buf_)) { const auto* p = static_cast<const uint8_t*>(d); for (Size i = 0; i < n; ++i) buf_[i] = p[i]; }
        return Status::Ok;
    }
    Status ep_read(uint8_t, void*, Size, Size& got) noexcept override { got = 0u; return Status::NotSupported; }
    void stall_ep0() noexcept override {}
    uint8_t ep_ = 0u; Size len_ = 0u; uint8_t buf_[2048]{};
};


namespace {
uint16_t csum(const uint8_t* p, Size n) {
    uint32_t s = 0u;
    for (Size i = 0; i + 1u < n; i += 2u) s += static_cast<uint16_t>((p[i] << 8) | p[i + 1u]);
    if (n & 1u) s += static_cast<uint16_t>(p[n - 1u] << 8);
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return static_cast<uint16_t>(~s);
}
void put16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint32_t get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

struct Peer {
    Hal* hal = nullptr;
    usb::rndis::Rndis* rndis = nullptr;
    uint8_t dev_mac[6]{};
    uint8_t host_mac[6]{0x02,0x4D,0x43,0,0,0x99};
    uint8_t dev_ip[4]{};
    uint8_t host_ip[4]{};
    bool arp_done = false;
    bool synack_done = false;
    bool udp_done = false;

    void feed(const uint8_t* frame, Size len) {
        uint8_t pkt[44 + 256]{};
        auto le32 = [](uint8_t* p, uint32_t v) {
            p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
        };
        le32(pkt, 1u); le32(pkt + 4, static_cast<uint32_t>(44u + len));
        le32(pkt + 8, 44u); le32(pkt + 12, static_cast<uint32_t>(len));
        for (Size i = 0; i < len; ++i) pkt[44u + i] = frame[i];
        (void)rndis->receive(pkt, 44u + len);
    }

    void pump() {
        if (hal == nullptr || hal->ep_ != 0x83u || hal->len_ < 44u + 42u) return;
        const uint8_t* f = hal->buf_ + 44u;          // RNDIS packet header
        const Size flen = hal->len_ - 44u;
        (void)flen;
        const uint16_t type = static_cast<uint16_t>((f[12] << 8) | f[13]);
        if (type == 0x0806u && !arp_done) {
            uint8_t r[42]{};
            for (int i = 0; i < 6; ++i) { r[i] = f[6 + i]; r[6 + i] = host_mac[i]; }
            put16(r + 12, 0x0806u);
            r[14] = 0; r[15] = 1; r[16] = 8; r[17] = 0; r[18] = 6; r[19] = 4;
            put16(r + 20, 2u);
            for (int i = 0; i < 6; ++i) r[22 + i] = host_mac[i];
            for (int i = 0; i < 4; ++i) r[28 + i] = host_ip[i];
            for (int i = 0; i < 6; ++i) r[32 + i] = f[22 + i];
            for (int i = 0; i < 4; ++i) r[38 + i] = dev_ip[i];
            feed(r, sizeof(r));
            arp_done = true;
            hal->len_ = 0u;
            return;
        }
        if (type == 0x0800u && !synack_done) {
            const uint8_t* ip = f + 14;
            if (ip[9] != 6u) return;
            const Size ihl = Size(ip[0] & 0x0Fu) * 4u;
            const uint8_t* tcp = ip + ihl;
            if ((tcp[13] & 0x02u) == 0u) return;
            const uint16_t sport = static_cast<uint16_t>((tcp[0] << 8) | tcp[1]);
            const uint16_t dport = static_cast<uint16_t>((tcp[2] << 8) | tcp[3]);
            uint8_t out[14 + 20 + 20]{};
            for (int i = 0; i < 6; ++i) { out[i] = dev_mac[i]; out[6 + i] = host_mac[i]; }
            put16(out + 12, 0x0800u);
            uint8_t* oip = out + 14;
            oip[0] = 0x45; put16(oip + 2, 40u); oip[8] = 64; oip[9] = 6;
            for (int i = 0; i < 4; ++i) { oip[12 + i] = host_ip[i]; oip[16 + i] = dev_ip[i]; }
            put16(oip + 10, csum(oip, 20u));
            uint8_t* ot = oip + 20;
            put16(ot + 0, dport); put16(ot + 2, sport);
            put32(ot + 4, 0x5000u);
            put32(ot + 8, get32(tcp + 4) + 1u);
            ot[12] = 5u << 4u; ot[13] = 0x12u;
            put16(ot + 14, 8192u);
            uint8_t ps[12 + 20]{};
            for (int i = 0; i < 4; ++i) { ps[i] = oip[12 + i]; ps[4 + i] = oip[16 + i]; }
            ps[9] = 6; put16(ps + 10, 20u);
            for (int i = 0; i < 20; ++i) ps[12 + i] = ot[i];
            put16(ot + 16, csum(ps, sizeof(ps)));
            feed(out, sizeof(out));
            synack_done = true;
            hal->len_ = 0u;
        }
        if (type == 0x0800u && synack_done && !udp_done) {
            const uint8_t* ip = f + 14;
            if (ip[9] != 17u) return;
            const Size ihl = Size(ip[0] & 0x0Fu) * 4u;
            const uint8_t* udp = ip + ihl;
            const uint16_t sport = static_cast<uint16_t>((udp[0] << 8) | udp[1]);
            const uint16_t dport = static_cast<uint16_t>((udp[2] << 8) | udp[3]);
            uint8_t out[14 + 20 + 8 + 2]{};
            for (int i = 0; i < 6; ++i) { out[i] = dev_mac[i]; out[6 + i] = host_mac[i]; }
            put16(out + 12, 0x0800u);
            uint8_t* oip = out + 14;
            oip[0] = 0x45; put16(oip + 2, 30u); oip[8] = 64; oip[9] = 17;
            for (int i = 0; i < 4; ++i) { oip[12 + i] = host_ip[i]; oip[16 + i] = dev_ip[i]; }
            put16(oip + 10, csum(oip, 20u));
            uint8_t* ou = oip + 20;
            put16(ou + 0, dport); put16(ou + 2, sport);
            put16(ou + 4, 10u);
            ou[8] = 'O'; ou[9] = 'K';
            feed(out, sizeof(out));
            udp_done = true;
            hal->len_ = 0u;
        }
    }
};
}

int total = 0, failed = 0;
void check(const char* what, bool ok) { ++total; if (!ok) ++failed; std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what); }
}

int main() {
    std::puts("RNDIS net::Factory:");
    Hal hal;
    usb::rndis::Rndis rndis;
    net::RndisNetConfig cfg{};
    cfg.ip[0] = 10; cfg.ip[1] = 0; cfg.ip[2] = 0; cfg.ip[3] = 2;
    cfg.gw[0] = 10; cfg.gw[1] = 0; cfg.gw[2] = 0; cfg.gw[3] = 1;
    net::RndisFactory f(rndis, hal, cfg);
    check("init", f.init() == Status::Ok);
    net::Endpoint ep{};
    check("resolve ip", f.resolve("10.1.2.3", ep) == Status::Ok &&
          ep.addr[0] == 10 && ep.addr[3] == 3);
    net::Socket* s = nullptr;
    check("create tcp", f.create(net::Protocol::Tcp, s) == Status::Ok && s != nullptr);
    check("tcp open", s != nullptr && s->open());
    Peer peer;
    peer.hal = &hal;
    peer.rndis = &rndis;
    for (int i = 0; i < 6; ++i) peer.dev_mac[i] = cfg.mac[i];
    for (int i = 0; i < 4; ++i) { peer.dev_ip[i] = cfg.ip[i]; peer.host_ip[i] = cfg.gw[i]; }
    f.set_pump([](void* ctx) noexcept -> Status { static_cast<Peer*>(ctx)->pump(); return Status::Ok; }, &peer);
    net::Endpoint remote{10,0,0,1,80};
    check("tcp connect (ARP/SYN)", s != nullptr && s->connect(remote, 50u) == Status::Ok);
    Size sent = 0u;
    check("tcp send", s != nullptr && s->send("GET", 3u, sent) == Status::Ok && sent == 3u);
    if (s != nullptr) s->close();
    check("create udp", f.create(net::Protocol::Udp, s) == Status::Ok && s != nullptr);
    if (s != nullptr) {
        Size us = 0u;
        check("udp send", s->send_to(net::Endpoint{10,0,0,1,53}, "Q", 1u, us) == Status::Ok && us == 1u);
        uint8_t u[8]{};
        Size got = 0u;
        net::Endpoint from{};
        check("udp recv", s->recv_from(from, u, sizeof(u), got, 50u) == Status::Ok &&
              got == 2u && u[0] == 'O' && u[1] == 'K');
        s->close();
    }
    std::printf("%d/%d проверок пройдено%s\n", total - failed, total,
                failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return failed == 0 ? 0 : 1;
}
