// os/shell/net_shell.hpp
// ---------------------------------------------------------------------------
// Shell поверх уже подключённого TCP-сокета и минимальный TCP-сервер.
// Консоль не знает про lwIP/W5500: она получает Socket&.
// ---------------------------------------------------------------------------
#pragma once

#include "shell.hpp"
#include "../net/socket.hpp"

namespace embed::shell {

class TcpConsole {
public:
    TcpConsole() noexcept = default;

    void attach(net::Socket& sock, Console& console) noexcept {
        sock_ = &sock;
        console_ = &console;
        console.init(&TcpConsole::writer, this);
    }

    void detach() noexcept { sock_ = nullptr; console_ = nullptr; }

    /// Забрать доступные байты и покормить консоль. Не блокирует.
    void poll() noexcept {
        if (sock_ == nullptr || console_ == nullptr) return;
        uint8_t buf[32]{};
        for (;;) {
            Size got = 0u;
            const Status st = sock_->recv(buf, sizeof(buf), got, 10u);
            if (st != Status::Ok || got == 0u) break;
            for (Size i = 0u; i < got; ++i) console_->feed(static_cast<char>(buf[i]));
        }
    }

    bool attached() const noexcept { return sock_ != nullptr; }
    Console& console() noexcept { return *console_; }

private:
    static void writer(void* ctx, const char* text) noexcept {
        auto* self = static_cast<TcpConsole*>(ctx);
        if (self == nullptr || self->sock_ == nullptr || text == nullptr) return;
        Size n = 0u;
        while (text[n] != '\0') ++n;
        Size sent = 0u;
        (void)self->sock_->send(text, n, sent);
    }

    net::Socket* sock_ = nullptr;
    Console* console_ = nullptr;
};

/// Один слушающий сокет + одно активное соединение. Для нескольких клиентов
/// приложение может создать несколько TcpServer.
class TcpServer {
public:
    TcpServer(net::Factory& factory, Console& console) noexcept
        : factory_(&factory), console_(&console) {}

    Status begin(uint16_t port) noexcept {
        if (listener_ != nullptr) return Status::AlreadyExists;
        port_ = port;
        Status st = factory_->create(net::Protocol::Tcp, listener_);
        if (st != Status::Ok || listener_ == nullptr) {
            listener_ = nullptr;
            return (st == Status::Ok) ? Status::NoMemory : st;
        }
        st = listener_->listen(port);
        if (st != Status::Ok) {
            listener_->close();
            listener_ = nullptr;
            return st;
        }
        return Status::Ok;
    }

    void poll() noexcept {
        if (listener_ == nullptr) return;
        if (client_ == nullptr) {
            if (!listener_->open()) {
                (void)listener_->listen(port_);
                if (!listener_->open()) return;
            }
            net::Socket* accepted = nullptr;
            const Status st = listener_->accept(accepted);
            if (st == Status::Ok && accepted != nullptr) {
                client_ = accepted;
                client_console_.attach(*client_, *console_);
            }
            return;
        }
        client_console_.poll();
        if (!client_->open()) end_client();
    }

    void end_client() noexcept {
        client_console_.detach();
        const bool same = (client_ == listener_);
        if (client_ != nullptr) client_->close();
        client_ = nullptr;
        // W5500: accepted connection и listener — один аппаратный сокет;
        // после закрытия клиента его нужно снова перевести в LISTEN.
        if (same && listener_ != nullptr) (void)listener_->listen(port_);
    }

    bool listening() const noexcept { return listener_ != nullptr; }
    bool connected() const noexcept { return client_ != nullptr; }

private:
    net::Factory* factory_ = nullptr;
    Console* console_ = nullptr;
    net::Socket* listener_ = nullptr;
    net::Socket* client_ = nullptr;
    uint16_t port_ = 0u;
    TcpConsole client_console_{};
};

} // namespace embed::shell
