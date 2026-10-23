#include "dns.hpp"
#include "../config/time_config.hpp"

namespace embed::net {

Size DnsClient::build_query(uint8_t* out, Size cap, const char* host) noexcept {
    if (out == nullptr || cap < 18u || host == nullptr || host[0] == '\0') return 0u;
    for (Size i = 0u; i < 12u; ++i) out[i] = 0u;
    out[0] = 0x12u; out[1] = 0x34u;
    out[2] = 0x01u; out[3] = 0x00u;
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
        } else {
            ++label;
            if (label > 63u) return 0u;
        }
    }
    out[n++] = 0u;
    out[n++] = 0x00u; out[n++] = 0x01u;
    out[n++] = 0x00u; out[n++] = 0x01u;
    return n;
}

bool DnsClient::skip_name(const uint8_t* pkt, Size len, Size& pos) noexcept {
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

bool DnsClient::parse_answer(const uint8_t* pkt, Size len, uint8_t ip[4]) noexcept {
    if (pkt == nullptr || len < 12u || ip == nullptr) return false;
    if (pkt[0] != 0x12u || pkt[1] != 0x34u) return false;
    const uint16_t qd = static_cast<uint16_t>((pkt[4] << 8) | pkt[5]);
    const uint16_t an = static_cast<uint16_t>((pkt[6] << 8) | pkt[7]);
    if (an == 0u) return false;
    Size pos = 12u;
    for (uint16_t i = 0u; i < qd; ++i) {
        if (!skip_name(pkt, len, pos) || pos + 4u > len) return false;
        pos += 4u;
    }
    for (uint16_t i = 0u; i < an; ++i) {
        if (!skip_name(pkt, len, pos) || pos + 10u > len) return false;
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

Status DnsClient::resolve(Socket& sock, const uint8_t dns[4], const char* host,
                          Endpoint& out, uint32_t timeout_ms) noexcept {
    if (dns == nullptr || host == nullptr) return Status::InvalidArg;
    uint8_t literal[4]{};
    if (parse_ipv4(host, literal)) {
        for (uint8_t i = 0u; i < 4u; ++i) out.addr[i] = literal[i];
        return Status::Ok;
    }
    uint8_t q[260]{};
    const Size qn = build_query(q, sizeof(q), host);
    if (qn == 0u) return Status::InvalidArg;
    Endpoint server{};
    for (uint8_t i = 0u; i < 4u; ++i) server.addr[i] = dns[i];
    server.port = 53u;
    Size sent = 0u;
    Status st = sock.send_to(server, q, qn, sent);
    if (st != Status::Ok) return st;
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
        if (parse_answer(rx, got, out.addr)) return Status::Ok;
    }
    return Status::Timeout;
}

} // namespace embed::net
