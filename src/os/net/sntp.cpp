// os/net/sntp.cpp
#include "sntp.hpp"
#include "../config/time_config.hpp"
#include "../time/rtc.hpp"

namespace embed::net::sntp {
namespace {
constexpr uint32_t kNtpOffset = 2208988800u;
uint32_t get_u32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}
} // namespace

Status query(Factory& factory, const char* server,
             uint32_t timeout_ms, Result& out) noexcept {
    if (server == nullptr) return Status::InvalidArg;

    Socket* s = nullptr;
    Status st = factory.create(Protocol::Udp, s);
    if (st != Status::Ok || s == nullptr) return (st == Status::Ok) ? Status::NoMemory : st;

    Endpoint to{};
    to.port = 123u;
    st = factory.resolve(server, to);
    if (st != Status::Ok) { s->close(); return st; }

    // LI=0, VN=4, Mode=3 (client), transmit timestamp (0 — неизвестно).
    uint8_t req[48]{};
    req[0] = (0u << 6) | (4u << 3) | 3u;
    Size sent = 0u;
    st = s->send_to(to, req, sizeof(req), sent);
    if (st != Status::Ok || sent != sizeof(req)) { s->close(); return (st == Status::Ok) ? Status::Aborted : st; }

    uint8_t resp[48]{};
    Endpoint from{};
    Size got = 0u;
    st = s->recv_from(from, resp, sizeof(resp), got, timeout_ms);
    s->close();
    if (st != Status::Ok) return st;
    if (got < 48u) return Status::Corruption;

    const uint8_t mode = static_cast<uint8_t>(resp[0] & 0x07u);
    if (mode != 4u) return Status::Corruption;
    out.stratum = resp[1];
    if (out.stratum == 0u || out.stratum > 15u) return Status::NotSupported;

    const uint32_t secs = get_u32(resp + 40u);
    if (secs < kNtpOffset) return Status::Corruption;
    out.epoch_seconds = static_cast<uint64_t>(secs - kNtpOffset);
    out.fraction = get_u32(resp + 44u);
    return Status::Ok;
}

Status sync(Factory& factory, const char* server,
            uint32_t timeout_ms, Result& out) noexcept {
    const Status st = query(factory, server, timeout_ms, out);
    if (st != Status::Ok) return st;
    const Status t = config::OsTimer::SyncTime(out.epoch_seconds, 0u);
    if (t != Status::Ok) return t;
    (void)rtc::WallClock::instance().persist();
    return Status::Ok;
}

} // namespace embed::net::sntp
