#include "dhcp.hpp"

namespace embed::net {

uint32_t DhcpClient::rd32be(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}
uint32_t DhcpClient::rd32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
void DhcpClient::put32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}

Size DhcpClient::build_discover(uint8_t* out, Size cap, uint32_t xid,
                                const uint8_t mac[6]) noexcept {
    return build(out, cap, xid, mac, 1u, nullptr, false);
}
Size DhcpClient::build_request(uint8_t* out, Size cap, uint32_t xid,
                               const uint8_t mac[6], const DhcpLease& lease) noexcept {
    return build(out, cap, xid, mac, 3u, &lease, false);
}
Size DhcpClient::build_renew(uint8_t* out, Size cap, uint32_t xid,
                             const uint8_t mac[6], const DhcpLease& lease) noexcept {
    return build(out, cap, xid, mac, 3u, &lease, true);
}

Size DhcpClient::build(uint8_t* out, Size cap, uint32_t xid, const uint8_t mac[6],
                       uint8_t type, const DhcpLease* lease, bool renew) noexcept {
    if (out == nullptr || cap < 280u || mac == nullptr) return 0u;
    for (Size i = 0u; i < 240u; ++i) out[i] = 0u;
    out[0] = 1u; out[1] = 1u; out[2] = 6u;
    put32(out + 4u, xid);
    out[10] = 0x80u; out[11] = 0x00u;
    for (uint8_t i = 0u; i < 6u; ++i) out[28u + i] = mac[i];
    if (renew && lease != nullptr) {
        for (uint8_t i = 0u; i < 4u; ++i) out[12u + i] = lease->ip[i];
    }
    put32(out + 236u, 0x63825363u);
    Size n = 240u;
    out[n++] = 53u; out[n++] = 1u; out[n++] = type;
    if (lease != nullptr) {
        if (!renew) {
            out[n++] = 50u; out[n++] = 4u;
            for (uint8_t i = 0u; i < 4u; ++i) out[n++] = lease->ip[i];
            out[n++] = 54u; out[n++] = 4u;
            for (uint8_t i = 0u; i < 4u; ++i) out[n++] = lease->server[i];
        }
        out[n++] = 57u; out[n++] = 2u;
        out[n++] = 0x05u; out[n++] = 0xDCu;             // max message size 1500
        out[n++] = 61u; out[n++] = 7u; out[n++] = 1u;
        for (uint8_t i = 0u; i < 6u; ++i) out[n++] = mac[i];
    }
    out[n++] = 55u; out[n++] = 4u;
    out[n++] = 1u; out[n++] = 3u; out[n++] = 6u; out[n++] = 51u;
    out[n++] = 255u;
    return n;
}

bool DhcpClient::parse_reply(const uint8_t* pkt, Size len, uint32_t xid,
                             uint8_t& type, DhcpLease& lease) noexcept {
    if (pkt == nullptr || len < 240u || pkt[0] != 2u || pkt[1] != 1u || pkt[2] != 6u) {
        return false;
    }
    if (rd32(pkt + 4u) != xid) return false;
    if (rd32be(pkt + 236u) != 0x63825363u) return false;
    for (uint8_t i = 0u; i < 4u; ++i) lease.ip[i] = pkt[16u + i];
    Size pos = 240u;
    bool have_type = false;
    while (pos < len) {
        const uint8_t code = pkt[pos++];
        if (code == 0u) continue;
        if (code == 255u) break;
        if (pos >= len) return false;
        const uint8_t olen = pkt[pos++];
        if (pos + olen > len) return false;
        if (code == 53u && olen >= 1u) {
            type = pkt[pos]; have_type = true;
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
        } else if (code == 58u && olen >= 4u) {
            lease.t1_s = rd32be(pkt + pos);
        } else if (code == 59u && olen >= 4u) {
            lease.t2_s = rd32be(pkt + pos);
        }
        pos = static_cast<Size>(pos + olen);
    }
    return have_type && (type == 2u || type == 5u);
}

bool DhcpClient::receive(Socket& sock, uint32_t xid, uint8_t wanted,
                         DhcpLease& lease, uint32_t timeout_ms) noexcept {
    const bool timed = config::OsTimer::TimeAvailable();
    const uint64_t start = timed ? config::OsTimer::NowMs() : 0u;
    for (uint8_t attempt = 0u; attempt < 8u; ++attempt) {
        Endpoint from{};
        uint8_t rx[512]{};
        Size got = 0u;
        const Status st = sock.recv_from(from, rx, sizeof(rx), got, 500u);
        if (st == Status::Timeout) {
            if (timed && config::OsTimer::NowMs() - start >= timeout_ms) break;
            continue;
        }
        if (st != Status::Ok) return false;
        uint8_t type = 0u;
        if (!parse_reply(rx, got, xid, type, lease)) continue;
        if (type == wanted || (wanted == 5u && type == 2u)) return true;
    }
    return false;
}

Status DhcpClient::acquire(Socket& sock, const uint8_t mac[6], DhcpLease& lease,
                           uint32_t timeout_ms) noexcept {
    if (mac == nullptr) return Status::InvalidArg;
    lease = DhcpLease{};
    const uint32_t xid = 0x4D434631u ^ static_cast<uint32_t>(
        config::OsTimer::TimeAvailable() ? config::OsTimer::NowMs() : 0u);
    uint8_t pkt[300]{};
    Size n = build_discover(pkt, sizeof(pkt), xid, mac);
    Endpoint bcast{255, 255, 255, 255, 67u};
    Size sent = 0u;
    Status st = sock.send_to(bcast, pkt, n, sent);
    if (st != Status::Ok) return st;

    bool offered = false;
    const bool timed = config::OsTimer::TimeAvailable();
    const uint64_t start = timed ? config::OsTimer::NowMs() : 0u;
    for (uint8_t attempt = 0u; attempt < 8u; ++attempt) {
        Endpoint from{};
        uint8_t rx[512]{};
        Size got = 0u;
        st = sock.recv_from(from, rx, sizeof(rx), got, 500u);
        if (st == Status::Timeout) {
            if (timed && config::OsTimer::NowMs() - start >= timeout_ms) break;
            continue;
        }
        if (st != Status::Ok) return st;
        uint8_t type = 0u;
        if (!parse_reply(rx, got, xid, type, lease)) continue;
        if (type == 2u || type == 5u) { offered = true; break; }
    }
    if (!offered) return Status::Timeout;
    if (lease.ip[0] == 0u && lease.ip[1] == 0u && lease.ip[2] == 0u && lease.ip[3] == 0u) {
        return Status::NotFound;
    }
    n = build_request(pkt, sizeof(pkt), xid, mac, lease);
    st = sock.send_to(bcast, pkt, n, sent);
    if (st != Status::Ok) return st;
    if (!receive(sock, xid, 5u, lease, timeout_ms)) return Status::Timeout;
    lease.obtained_ms = timed ? config::OsTimer::NowMs() : 0u;
    return Status::Ok;
}

Status DhcpClient::renew(Socket& sock, const uint8_t mac[6], DhcpLease& lease,
                         uint32_t timeout_ms) noexcept {
    if (mac == nullptr) return Status::InvalidArg;
    const uint32_t xid = 0x4D434632u ^ static_cast<uint32_t>(
        config::OsTimer::TimeAvailable() ? config::OsTimer::NowMs() : 0u);
    uint8_t pkt[300]{};
    const Size n = build_renew(pkt, sizeof(pkt), xid, mac, lease);
    Endpoint bcast{255, 255, 255, 255, 67u};
    Size sent = 0u;
    const Status st = sock.send_to(bcast, pkt, n, sent);
    if (st != Status::Ok) return st;
    if (!receive(sock, xid, 5u, lease, timeout_ms)) return Status::Timeout;
    lease.obtained_ms = config::OsTimer::TimeAvailable() ? config::OsTimer::NowMs() : 0u;
    return Status::Ok;
}

} // namespace embed::net
