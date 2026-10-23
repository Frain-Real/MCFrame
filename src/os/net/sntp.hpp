// os/net/sntp.hpp
#pragma once
#include "socket.hpp"

namespace embed::net::sntp {

struct Result {
    uint64_t epoch_seconds = 0u;
    uint32_t fraction      = 0u;
    uint8_t  stratum       = 0u;
    int8_t   offset_ms     = 0;   ///< UTC - local, 0 если неизвестно
};

/// Запросить время у SNTP-сервера (UDP/123). Сервер — имя или IPv4.
Status query(Factory& factory, const char* server,
             uint32_t timeout_ms, Result& out) noexcept;

/// query() + OsTimer::SyncTime + сохранение в RTC (если backend привязан).
Status sync(Factory& factory, const char* server,
            uint32_t timeout_ms, Result& out) noexcept;

} // namespace embed::net::sntp
