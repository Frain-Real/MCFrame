// tests/test_net.cpp
// ---------------------------------------------------------------------------
// Сетевой слой: сокеты, MQTT, HTTP, SNTP. Транспорт — сценарий в памяти:
// тест подаёт РОВНО те байты, которые прислал бы сервер, и проверяет, что
// клиент сформировал корректный запрос. Так проверяется протокол, а не lwIP.
// ---------------------------------------------------------------------------
#include "os/net/http.hpp"
#include "os/net/mqtt.hpp"
#include "os/net/sntp.hpp"
#include "os/time/rtc.hpp"
#include "os/config/time_config.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

using namespace embed;
using namespace embed::net;

namespace {
int g_total = 0;
int g_failed = 0;
void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

class ScriptSocket final : public net::Socket {
public:
    void reset() noexcept { rx_len_ = tx_len_ = 0u; open_ = false; }
    void push(const void* p, Size n) noexcept {
        const auto* b = static_cast<const uint8_t*>(p);
        for (Size i = 0u; i < n && rx_len_ < sizeof(rx_); ++i) rx_[rx_len_++] = b[i];
    }
    Status connect(const net::Endpoint& ep, uint32_t) noexcept override {
        ep_ = ep;
        open_ = true;
        return Status::Ok;
    }
    Status send(const void* p, Size n, Size& sent) noexcept override {
        const auto* b = static_cast<const uint8_t*>(p);
        for (Size i = 0u; i < n && tx_len_ < sizeof(tx_); ++i) tx_[tx_len_++] = b[i];
        sent = n;
        return Status::Ok;
    }
    Status recv(void* p, Size cap, Size& got, uint32_t) noexcept override {
        got = 0u;
        if (!open_) return Status::Unavailable;
        if (rx_pos_ >= rx_len_) return Status::Timeout;
        Size n = 0u;
        auto* out = static_cast<uint8_t*>(p);
        while (n < cap && rx_pos_ < rx_len_) out[n++] = rx_[rx_pos_++];
        got = n;
        return Status::Ok;
    }
    Status send_to(const net::Endpoint&, const void* p, Size n, Size& sent) noexcept override {
        open_ = true;
        return send(p, n, sent);
    }
    Status recv_from(net::Endpoint& ep, void* p, Size cap, Size& got, uint32_t to) noexcept override {
        ep = net::Endpoint{127, 0, 0, 1, 123};
        return recv(p, cap, got, to);
    }
    void close() noexcept override { open_ = false; }
    bool open() const noexcept override { return open_; }

    Size tx_len() const noexcept { return tx_len_; }
    const uint8_t* tx() const noexcept { return tx_; }
    Size rx_len() const noexcept { return rx_len_; }
    void clear_tx() noexcept { tx_len_ = 0u; }

private:
    uint8_t rx_[4096]{};
    Size rx_len_ = 0u;
    Size rx_pos_ = 0u;
    uint8_t tx_[4096]{};
    Size tx_len_ = 0u;
    net::Endpoint ep_{};
    bool open_ = false;
};

class MockFactory final : public net::Factory {
public:
    Status create(net::Protocol proto, net::Socket*& out) noexcept override {
        out = (proto == net::Protocol::Tcp) ? &tcp : &udp;
        out->close();
        return Status::Ok;
    }
    Status resolve(const char* host, net::Endpoint& out) noexcept override {
        if (host == nullptr) return Status::InvalidArg;
        if (std::strcmp(host, "broker") == 0 || std::strcmp(host, "example.com") == 0 ||
            std::strcmp(host, "pool.ntp.org") == 0) {
            out.addr[0] = 127; out.addr[1] = 0; out.addr[2] = 0; out.addr[3] = 1;
            return Status::Ok;
        }
        uint8_t ip[4]{};
        if (net::parse_ipv4(host, ip)) {
            for (uint8_t i = 0; i < 4; ++i) out.addr[i] = ip[i];
            return Status::Ok;
        }
        return Status::NotFound;
    }
    ScriptSocket tcp;
    ScriptSocket udp;
};

void test_socket_ipv4() {
    std::puts("IPv4/URL:");
    uint8_t ip[4]{};
    check("parse 192.168.1.10", net::parse_ipv4("192.168.1.10", ip) &&
          ip[0] == 192 && ip[3] == 10);
    check("reject 256.1.1.1", !net::parse_ipv4("256.1.1.1", ip));
    net::http::Url u{};
    check("http url", net::http::parse_url("http://example.com/api?x=1", u) &&
          std::strcmp(u.host, "example.com") == 0 && u.port == 80u &&
          std::strcmp(u.path, "/api?x=1") == 0 && !u.tls);
    check("https is marked", net::http::parse_url("https://example.com/", u) && u.tls);
}

void test_mqtt() {
    std::puts("MQTT:");
    MockFactory f;
    mqtt::Client<512> c;
    mqtt::Config cfg{};
    cfg.host = "broker";
    cfg.client_id = "mc-test";
    check("init", c.init(f, cfg) == Status::Ok);

    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    f.tcp.push(connack, sizeof(connack));
    f.tcp.rx_len();   // no-op
    check("connect", c.connect(100u) == Status::Ok);
    check("connected", c.connected());
    check("CONNECT header", f.tcp.tx_len() > 2u && f.tcp.tx()[0] == 0x10u);

    f.tcp.clear_tx();
    const char payload[] = "hello";
    check("publish qos0", c.publish("dev/temp", payload, 5u) == Status::Ok);
    check("PUBLISH type", f.tcp.tx_len() > 2u && (f.tcp.tx()[0] & 0xF0u) == 0x30u);

    f.tcp.clear_tx();
    const uint8_t suback[] = {0x90, 0x03, 0x00, 0x01, 0x00};
    f.tcp.push(suback, sizeof(suback));
    check("subscribe", c.subscribe("dev/#") == Status::Ok);

    struct Ctx { int count; char topic[32]; char body[16]; Size len; } ctx{};
    c.set_callback([](void* p, const mqtt::Message& m) {
        auto* x = static_cast<Ctx*>(p);
        ++x->count;
        Size i = 0u;
        while (m.topic[i] != '\0' && i + 1u < sizeof(x->topic)) { x->topic[i] = m.topic[i]; ++i; }
        x->topic[i] = '\0';
        for (Size k = 0u; k < m.len && k + 1u < sizeof(x->body); ++k) x->body[k] = static_cast<char>(m.payload[k]);
        x->len = m.len;
    }, &ctx);

    // PUBLISH QoS0, topic "dev/x", payload "42"
    const uint8_t pub[] = {0x30, 0x09, 0x00, 0x05, 'd', 'e', 'v', '/', 'x', '4', '2'};
    f.tcp.push(pub, sizeof(pub));
    check("poll publish", c.poll(100u) == Status::Ok);
    check("message count", ctx.count == 1);
    check("topic", std::strcmp(ctx.topic, "dev/x") == 0);
    check("payload", ctx.len == 2u && ctx.body[0] == '4' && ctx.body[1] == '2');

    f.tcp.clear_tx();
    const uint8_t pingresp[] = {0xD0, 0x00};
    f.tcp.push(pingresp, sizeof(pingresp));
    check("ping", c.ping(100u) == Status::Ok);
}

void test_http() {
    std::puts("HTTP:");
    MockFactory f;
    const char resp[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello";
    f.tcp.push(resp, sizeof(resp) - 1u);
    http::Client<512> c;
    http::Response r{};
    char body[32]{};
    Size n = 0u;
    auto sink = [](void* p, const uint8_t* d, Size len) noexcept -> bool {
        auto* st = static_cast<std::pair<char*, Size*>*>(p);
        for (Size i = 0u; i < len && *st->second + 1u < 32u; ++i) st->first[(*st->second)++] = static_cast<char>(d[i]);
        return true;
    };
    std::pair<char*, Size*> st{body, &n};
    check("GET", c.get(f, "http://example.com/hello", r, sink, &st, 100u) == Status::Ok);
    check("status", r.status == 200);
    check("content length", r.content_length == 5u);
    check("content type", std::strcmp(r.content_type, "text/plain") == 0);
    check("body", n == 5u && std::strcmp(body, "hello") == 0);

    check("HTTPS rejected", c.get(f, "https://example.com/", r) == Status::NotSupported);

    // chunked body.
    MockFactory f2;
    const char chunked[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\n\r\n";
    f2.tcp.push(chunked, sizeof(chunked) - 1u);
    char body2[16]{};
    Size n2 = 0u;
    std::pair<char*, Size*> st2{body2, &n2};
    http::Response r2{};
    check("chunked GET", c.get(f2, "http://example.com/", r2, sink, &st2, 100u) == Status::Ok);
    check("chunked body", n2 == 5u && std::strcmp(body2, "hello") == 0);
}

void test_sntp() {
    std::puts("SNTP:");
    config::OsTimer::Config tc{};
    tc.min_tick_ms = 1; tc.max_tick_ms = 1000; tc.default_tick_ms = 1;
    check("timer init", config::OsTimer::Init(tc) == Status::Ok);
    rtc::SoftBackend rtc_backend;
    rtc::WallClock::instance().attach(&rtc_backend);

    MockFactory f;
    uint8_t resp[48]{};
    resp[0] = (0u << 6) | (4u << 3) | 4u;   // LI=0 VN=4 mode=server
    resp[1] = 1u;                            // stratum
    const uint32_t ntp = 2208988800u + 1767225600u;
    resp[40] = static_cast<uint8_t>(ntp >> 24);
    resp[41] = static_cast<uint8_t>(ntp >> 16);
    resp[42] = static_cast<uint8_t>(ntp >> 8);
    resp[43] = static_cast<uint8_t>(ntp);
    f.udp.push(resp, sizeof(resp));

    sntp::Result out{};
    check("query", sntp::query(f, "pool.ntp.org", 100u, out) == Status::Ok);
    check("epoch", out.epoch_seconds == 1767225600ull && out.stratum == 1u);

    f.udp.push(resp, sizeof(resp));
    check("sync", sntp::sync(f, "pool.ntp.org", 100u, out) == Status::Ok);
    check("OS time set", config::OsTimer::NowEpochSeconds() == 1767225600ull);
    check("RTC persisted", rtc_backend.valid());
}

} // namespace

int main() {
    std::puts("=== network ===");
    test_socket_ipv4();
    test_mqtt();
    test_http();
    test_sntp();
    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
