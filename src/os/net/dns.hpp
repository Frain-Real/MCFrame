// os/net/dns.hpp
// ---------------------------------------------------------------------------
// Минимальный DNS A-record клиент/кодек. Не зависит от W5500: работает с
// любым UDP-сокетом, включая RNDIS/lwIP.
// ---------------------------------------------------------------------------
#pragma once

#include "socket.hpp"

namespace embed::net {

class DnsClient {
public:
    /// resolve через UDP-сокет, уже созданный фабрикой.
    Status resolve(Socket& sock, const uint8_t dns[4], const char* host,
                   Endpoint& out, uint32_t timeout_ms = 5000u) noexcept;

    static Size build_query(uint8_t* out, Size cap, const char* host) noexcept;
    /// Разобрать первый A-record. Compression pointers поддерживаются.
    static bool parse_answer(const uint8_t* pkt, Size len, uint8_t ip[4]) noexcept;

private:
    static bool skip_name(const uint8_t* pkt, Size len, Size& pos) noexcept;
};

} // namespace embed::net
