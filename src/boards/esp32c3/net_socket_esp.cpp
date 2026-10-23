// boards/esp32c3/net_socket_esp.cpp
#include "net_socket_esp.hpp"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace embed::esp32c3 {
namespace {

class EspSocket final : public net::Socket {
public:
    ~EspSocket() override { close(); }

    Status open(net::Protocol proto) noexcept {
        close();
        const int type = (proto == net::Protocol::Tcp) ? SOCK_STREAM : SOCK_DGRAM;
        fd_ = ::socket(AF_INET, type, 0);
        if (fd_ < 0) return Status::HardwareError;
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) (void)::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        return Status::Ok;
    }

    Status connect(const net::Endpoint& remote, uint32_t timeout_ms) noexcept override {
        if (fd_ < 0) return Status::Unavailable;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(remote.port);
        std::memcpy(&a.sin_addr.s_addr, remote.addr, 4);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) return Status::Ok;
        if (errno != EINPROGRESS) return Status::Unavailable;
        Status st = wait(true, timeout_ms);
        if (st != Status::Ok) return st;
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            return Status::Unavailable;
        }
        return Status::Ok;
    }

    Status send(const void* data, Size len, Size& sent) noexcept override {
        sent = 0u;
        if (fd_ < 0) return Status::Unavailable;
        const int n = ::send(fd_, data, len, 0);
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? Status::Busy
                                                                   : Status::Unavailable;
        sent = static_cast<Size>(n);
        return Status::Ok;
    }

    Status recv(void* data, Size cap, Size& received, uint32_t timeout_ms) noexcept override {
        received = 0u;
        if (fd_ < 0) return Status::Unavailable;
        Status st = wait(false, timeout_ms);
        if (st != Status::Ok) return st;
        const int n = ::recv(fd_, data, cap, 0);
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? Status::Busy
                                                                   : Status::Unavailable;
        received = static_cast<Size>(n);
        return Status::Ok;
    }

    Status send_to(const net::Endpoint& remote, const void* data, Size len,
                   Size& sent) noexcept override {
        sent = 0u;
        if (fd_ < 0) return Status::Unavailable;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(remote.port);
        std::memcpy(&a.sin_addr.s_addr, remote.addr, 4);
        const int n = ::sendto(fd_, data, len, 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        if (n < 0) return Status::Unavailable;
        sent = static_cast<Size>(n);
        return Status::Ok;
    }

    Status recv_from(net::Endpoint& remote, void* data, Size cap,
                     Size& received, uint32_t timeout_ms) noexcept override {
        received = 0u;
        if (fd_ < 0) return Status::Unavailable;
        Status st = wait(false, timeout_ms);
        if (st != Status::Ok) return st;
        sockaddr_in a{};
        socklen_t alen = sizeof(a);
        const int n = ::recvfrom(fd_, data, cap, 0, reinterpret_cast<sockaddr*>(&a), &alen);
        if (n < 0) return Status::Unavailable;
        if (alen >= sizeof(sockaddr_in)) {
            std::memcpy(remote.addr, &a.sin_addr.s_addr, 4);
            remote.port = ntohs(a.sin_port);
        }
        received = static_cast<Size>(n);
        return Status::Ok;
    }

    void close() noexcept override {
        if (fd_ >= 0) { (void)::close(fd_); fd_ = -1; }
    }
    bool open() const noexcept override { return fd_ >= 0; }

private:
    Status wait(bool for_write, uint32_t timeout_ms) noexcept {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd_, &set);
        timeval tv{};
        tv.tv_sec = static_cast<long>(timeout_ms / 1000u);
        tv.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
        const int r = ::select(fd_ + 1, for_write ? nullptr : &set,
                               for_write ? &set : nullptr, nullptr, &tv);
        if (r == 0) return Status::Timeout;
        if (r < 0) return Status::Unavailable;
        return Status::Ok;
    }

    int fd_ = -1;
};

EspSocket g_pool[4]{};

Status resolve_impl(const char* host, net::Endpoint& out) noexcept {
    if (host == nullptr) return Status::InvalidArg;
    uint8_t ip[4]{};
    if (net::parse_ipv4(host, ip)) {
        for (uint8_t i = 0u; i < 4u; ++i) out.addr[i] = ip[i];
        return Status::Ok;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr) {
        return Status::NotFound;
    }
    const auto* a = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    std::memcpy(out.addr, &a->sin_addr.s_addr, 4);
    ::freeaddrinfo(res);
    return Status::Ok;
}

} // namespace

SocketFactoryEsp& sockets() noexcept {
    static SocketFactoryEsp f;
    return f;
}

Status SocketFactoryEsp::create(net::Protocol proto, net::Socket*& out) noexcept {
    for (auto& s : g_pool) {
        if (!s.open()) {
            const Status st = s.open(proto);
            if (st != Status::Ok) return st;
            out = &s;
            return Status::Ok;
        }
    }
    return Status::NoMemory;
}

Status SocketFactoryEsp::resolve(const char* host, net::Endpoint& out) noexcept {
    return resolve_impl(host, out);
}

} // namespace embed::esp32c3
