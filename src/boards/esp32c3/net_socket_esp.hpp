// boards/esp32c3/net_socket_esp.hpp
#pragma once
#include "os/net/socket.hpp"

namespace embed::esp32c3 {

/// lwIP/BSD-socket backend для net::Factory. Один статический пул на 4
/// сокета: в приложении одновременно живут MQTT, HTTP и SNTP.
class SocketFactoryEsp final : public net::Factory {
public:
    Status create(net::Protocol proto, net::Socket*& out) noexcept override;
    Status resolve(const char* host, net::Endpoint& out) noexcept override;
};

SocketFactoryEsp& sockets() noexcept;

} // namespace embed::esp32c3
