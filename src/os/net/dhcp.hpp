// os/net/dhcp.hpp
// ---------------------------------------------------------------------------
// DHCP-клиент и DHCP-кодек, не зависящий от W5500/RNDIS. Транспорт — обычный
// UDP-сокет (порт 68), поэтому один и тот же код работает поверх W5500,
// RNDIS и lwIP.
// ---------------------------------------------------------------------------
#pragma once

#include "socket.hpp"
#include "../config/time_config.hpp"

namespace embed::net {

struct DhcpLease {
    uint8_t  ip[4]{};
    uint8_t  gw[4]{};
    uint8_t  mask[4]{};
    uint8_t  dns[4]{};
    uint8_t  server[4]{};
    uint32_t lease_s = 0u;
    uint32_t t1_s = 0u;
    uint32_t t2_s = 0u;
    uint64_t obtained_ms = 0u;
};

class DhcpClient {
public:
    /// Got a lease. Socket must be a UDP socket bound to port 68.
    Status acquire(Socket& sock, const uint8_t mac[6], DhcpLease& lease,
                   uint32_t timeout_ms = 10000u) noexcept;

    /// DHCPREQUEST renewal for an existing lease (ciaddr set).
    Status renew(Socket& sock, const uint8_t mac[6], DhcpLease& lease,
                 uint32_t timeout_ms = 5000u) noexcept;

    static Size build_discover(uint8_t* out, Size cap, uint32_t xid,
                               const uint8_t mac[6]) noexcept;
    static Size build_request(uint8_t* out, Size cap, uint32_t xid,
                              const uint8_t mac[6], const DhcpLease& lease) noexcept;
    static Size build_renew(uint8_t* out, Size cap, uint32_t xid,
                            const uint8_t mac[6], const DhcpLease& lease) noexcept;
    static bool parse_reply(const uint8_t* pkt, Size len, uint32_t xid,
                            uint8_t& type, DhcpLease& lease) noexcept;

    /// Сколько миллисекунд осталось до истечения lease (0 — истёк).
    static uint32_t remaining_ms(const DhcpLease& lease, uint64_t now_ms) noexcept {
        if (lease.lease_s == 0u || now_ms <= lease.obtained_ms) return lease.lease_s * 1000u;
        const uint64_t total = static_cast<uint64_t>(lease.lease_s) * 1000u;
        const uint64_t used = now_ms - lease.obtained_ms;
        return used >= total ? 0u : static_cast<uint32_t>(total - used);
    }

private:
    static uint32_t rd32be(const uint8_t* p) noexcept;
    static uint32_t rd32(const uint8_t* p) noexcept;
    static void put32(uint8_t* p, uint32_t v) noexcept;
    static Size build(uint8_t* out, Size cap, uint32_t xid, const uint8_t mac[6],
                      uint8_t type, const DhcpLease* lease, bool renew) noexcept;
    static bool receive(Socket& sock, uint32_t xid, uint8_t wanted,
                        DhcpLease& lease, uint32_t timeout_ms) noexcept;
};

} // namespace embed::net
