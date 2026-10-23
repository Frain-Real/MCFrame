// os/net/socket.hpp
// ---------------------------------------------------------------------------
// Сетевой слой MCFrame: минимальный контракт сокетов, не привязанный к lwIP.
//
// Зачем свой интерфейс: MQTT/HTTP/SNTP — это протоколы, а не драйверы. Один
// и тот же код должен работать поверх lwIP (ESP32-C3), внешнего модема,
// loopback-модели в host-тестах и будущего TLS-транспорта. Сокет здесь —
// объект с методами send/recv/connect; фабрика выдаёт уже созданные объекты
// (статический пул), поэтому в горячем пути нет malloc.
//
// ВАЖНО: реализация обязана соблюдать таймауты и возвращать Timeout, а не
// зависать. HTTP/MQTT считают таймаут частью контракта: без него task с
// сетевым клиентом блокирует весь планировщик.
// ---------------------------------------------------------------------------
#pragma once

#include "../common.hpp"

namespace embed::net {

/// IPv4-адрес + порт. DNS выполняет Factory::resolve.
struct Endpoint {
    uint8_t  addr[4] = {0, 0, 0, 0};
    uint16_t port    = 0u;

    constexpr Endpoint() noexcept = default;
    constexpr Endpoint(uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                       uint16_t p) noexcept
        : addr{a, b, c, d}, port(p) {}
};

enum class Protocol : uint8_t { Tcp = 0u, Udp = 1u };

/// Один открытый сокет. Для TCP используются connect/send/recv; для UDP —
/// send_to/recv_from. Неподдержанная операция возвращает NotSupported.
class Socket {
public:
    virtual ~Socket() = default;

    /// Открыть TCP-соединение. Для UDP может быть no-op.
    virtual Status connect(const Endpoint& remote, uint32_t timeout_ms) noexcept = 0;
    virtual Status send(const void* data, Size len, Size& sent) noexcept = 0;
    virtual Status recv(void* data, Size cap, Size& received,
                        uint32_t timeout_ms) noexcept = 0;
    virtual Status send_to(const Endpoint& remote, const void* data, Size len,
                           Size& sent) noexcept = 0;
    virtual Status recv_from(Endpoint& remote, void* data, Size cap,
                             Size& received, uint32_t timeout_ms) noexcept = 0;
    virtual void close() noexcept = 0;
    virtual bool open() const noexcept = 0;

    /// TCP-сервер: перевести сокет в LISTEN. По умолчанию не поддержано.
    virtual Status listen(uint16_t) noexcept { return Status::NotSupported; }
    /// Принять соединение. out — сокет из пула фабрики.
    virtual Status accept(Socket*& out) noexcept {
        (void)out;
        return Status::NotSupported;
    }

protected:
    Socket() = default;
};

/// Фабрика сокетов/резолвер. Реализация хранит статический пул сокетов;
/// create() возвращает объект, который живёт до close()/повторного create().
class Factory {
public:
    virtual ~Factory() = default;
    virtual Status create(Protocol proto, Socket*& out) noexcept = 0;
    /// Разрешить имя/IP в Endpoint (порт задаёт вызывающий).
    virtual Status resolve(const char* host, Endpoint& out) noexcept = 0;
};

/// Слушающий TCP-сокет с явным жизненным циклом. Владеет одним Socket из
/// пула фабрики; close()/rearm() освобождают/пересоздают его.
class Listener {
public:
    Listener() noexcept = default;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    ~Listener() noexcept { close(); }

    Status open(Factory& factory, uint16_t port) noexcept {
        close();
        factory_ = &factory;
        port_ = port;
        Status st = factory.create(Protocol::Tcp, socket_);
        if (st != Status::Ok || socket_ == nullptr) {
            socket_ = nullptr;
            return (st == Status::Ok) ? Status::NoMemory : st;
        }
        st = socket_->listen(port);
        if (st != Status::Ok) { socket_->close(); socket_ = nullptr; return st; }
        return Status::Ok;
    }

    Status accept(Socket*& out) noexcept {
        out = nullptr;
        if (socket_ == nullptr) return Status::Unavailable;
        const Status st = socket_->accept(out);
        if (st == Status::Ok && out == socket_) same_socket_ = true;
        return st;
    }

    /// После accepted-соединения на том же аппаратном сокете (W5500)
    /// пересоздать listener. Для C3/lwIP с отдельным accepted-сокетом не нужен.
    Status rearm() noexcept {
        if (factory_ == nullptr || port_ == 0u) return Status::Unavailable;
        const uint16_t p = port_;
        close();
        return open(*factory_, p);
    }

    void close() noexcept {
        if (socket_ != nullptr) { socket_->close(); socket_ = nullptr; }
        same_socket_ = false;
    }

    bool listening() const noexcept { return socket_ != nullptr; }
    bool same_socket() const noexcept { return same_socket_; }
    uint16_t port() const noexcept { return port_; }

private:
    Factory* factory_ = nullptr;
    Socket* socket_ = nullptr;
    uint16_t port_ = 0u;
    bool same_socket_ = false;
};

/// Проверка «host — это IPv4-литерал». Полезно реализациям resolve.
inline bool parse_ipv4(const char* text, uint8_t out[4]) noexcept {
    if (text == nullptr) return false;
    uint32_t part = 0u, value = 0u, count = 0u;
    for (const char* p = text;; ++p) {
        const char c = *p;
        if (c >= '0' && c <= '9') {
            value = value * 10u + static_cast<uint32_t>(c - '0');
            if (value > 255u) return false;
            part = 1u;
        } else if (c == '.' || c == '\0') {
            if (part == 0u || count >= 4u) return false;
            out[count++] = static_cast<uint8_t>(value);
            value = 0u;
            part = 0u;
            if (c == '\0') break;
        } else {
            return false;
        }
    }
    return count == 4u;
}

} // namespace embed::net
