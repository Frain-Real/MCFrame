// os/net/tls.hpp
// ---------------------------------------------------------------------------
// TLS-слой: фабрика сокетов, которая оборачивает обычный TCP-сокет
// TlsSession-ом. Криптография и X.509 не живут в ядре: провайдер
// (mbedTLS/esp-tls/другой) реализует TlsSession, а MQTT/HTTP/SNTP продолжают
// работать через обычный net::Socket.
//
// Контракт:
//   * TlsFactoryT<Provider,N> — статический пул из N TLS-сессий;
//   * resolve() делегируется base-фабрике (DNS остаётся прежним);
//   * create(Tcp) сначала создаёт raw TCP, затем TlsSession::start();
//   * send/recv идут через session, close закрывает и TLS, и raw.
// ---------------------------------------------------------------------------
#pragma once

#include "socket.hpp"

namespace embed::net {

struct TlsConfig {
    const uint8_t* ca = nullptr;      ///< DER/PEMM CA bundle; nullptr — системный/без проверки
    Size           ca_len = 0u;
    bool           verify = true;     ///< проверять цепочку и hostname
    const char*    alpn = nullptr;    ///< опционально, например "mqtt"
};

class TlsSession {
public:
    virtual ~TlsSession() = default;
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;

    virtual Status start(Socket& raw, const Endpoint& remote, const char* host,
                         const TlsConfig& cfg, uint32_t timeout_ms) noexcept = 0;
    virtual Status send(const void* data, Size len, Size& sent) noexcept = 0;
    virtual Status recv(void* data, Size cap, Size& got,
                        uint32_t timeout_ms) noexcept = 0;
    virtual void close() noexcept = 0;

protected:
    TlsSession() = default;
};

class TlsProvider {
public:
    virtual ~TlsProvider() = default;
    virtual Status create(TlsSession*& out) noexcept = 0;
    virtual void release(TlsSession*) noexcept {}
};

/// TCP-сокет, у которого после connect() поверх raw-сокета поднят TLS.
class TlsSocket final : public Socket {
public:
    TlsSocket() noexcept = default;
    ~TlsSocket() noexcept override { close(); }
    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    Status begin(Factory& base, TlsProvider& provider, const char* host,
                 const TlsConfig& cfg) noexcept {
        base_ = &base;
        provider_ = &provider;
        host_ = host;
        cfg_ = cfg;
        tls_ = nullptr;
        raw_ = nullptr;
        connected_ = false;
        return Status::Ok;
    }

    Status connect(const Endpoint& remote, uint32_t timeout_ms) noexcept override {
        if (base_ == nullptr || provider_ == nullptr) return Status::Unavailable;
        close();
        Status st = base_->create(Protocol::Tcp, raw_);
        if (st != Status::Ok || raw_ == nullptr) return (st == Status::Ok) ? Status::NoMemory : st;
        st = raw_->connect(remote, timeout_ms);
        if (st != Status::Ok) { raw_->close(); raw_ = nullptr; return st; }
        st = provider_->create(tls_);
        if (st != Status::Ok || tls_ == nullptr) { raw_->close(); raw_ = nullptr; return (st == Status::Ok) ? Status::NoMemory : st; }
        st = tls_->start(*raw_, remote, host_, cfg_, timeout_ms);
        if (st != Status::Ok) {
            tls_->close();
            provider_->release(tls_);
            tls_ = nullptr;
            raw_->close();
            raw_ = nullptr;
            return st;
        }
        connected_ = true;
        return Status::Ok;
    }

    Status send(const void* data, Size len, Size& sent) noexcept override {
        if (!connected_ || tls_ == nullptr || raw_ == nullptr) return Status::Unavailable;
        (void)raw_;
        return tls_->send(data, len, sent);
    }

    Status recv(void* data, Size cap, Size& received,
                uint32_t timeout_ms) noexcept override {
        if (!connected_ || tls_ == nullptr) return Status::Unavailable;
        return tls_->recv(data, cap, received, timeout_ms);
    }

    Status send_to(const Endpoint&, const void*, Size, Size&) noexcept override {
        return Status::NotSupported;
    }
    Status recv_from(Endpoint&, void*, Size, Size&, uint32_t) noexcept override {
        return Status::NotSupported;
    }

    void close() noexcept override {
        if (tls_ != nullptr) {
            tls_->close();
            if (provider_ != nullptr) provider_->release(tls_);
            tls_ = nullptr;
        }
        if (raw_ != nullptr) {
            raw_->close();
            raw_ = nullptr;
        }
        connected_ = false;
    }

    bool open() const noexcept override { return connected_; }

private:
    Factory*     base_ = nullptr;
    TlsProvider* provider_ = nullptr;
    Socket*      raw_ = nullptr;
    TlsSession*  tls_ = nullptr;
    const char*  host_ = nullptr;
    TlsConfig    cfg_{};
    bool         connected_ = false;
};

/// Фабрика TLS-сокетов. base — DNS/raw TCP; provider — криптографический
/// backend. Пул статический; одновременно живёт не больше N соединений.
template <Size N = 2u>
class TlsFactoryT final : public Factory {
    static_assert(N >= 1u, "TLS: нужен хотя бы один слот");

public:
    TlsFactoryT(Factory& base, TlsProvider& provider) noexcept
        : base_(&base), provider_(&provider) {}

    void set_config(const TlsConfig& cfg) noexcept { cfg_ = cfg; }

    Status create(Protocol proto, Socket*& out) noexcept override {
        if (proto != Protocol::Tcp) return Status::NotSupported;
        for (Size i = 0u; i < N; ++i) {
            if (!slots_[i].open()) {
                const Status st = slots_[i].begin(*base_, *provider_, host_, cfg_);
                if (st != Status::Ok) return st;
                out = &slots_[i];
                return Status::Ok;
            }
        }
        return Status::NoMemory;
    }

    Status resolve(const char* host, Endpoint& out) noexcept override {
        const Size n = copy_host(host);
        if (n == 0u) return Status::InvalidArg;
        return base_->resolve(host, out);
    }

    Factory& base() noexcept { return *base_; }
    TlsProvider& provider() noexcept { return *provider_; }

private:
    Size copy_host(const char* host) noexcept {
        if (host == nullptr) return 0u;
        Size n = 0u;
        while (host[n] != '\0' && n + 1u < sizeof(host_)) {
            host_[n] = host[n];
            ++n;
        }
        host_[n] = '\0';
        return n;
    }

    Factory*  base_ = nullptr;
    TlsProvider* provider_ = nullptr;
    TlsConfig cfg_{};
    char      host_[128]{};
    TlsSocket slots_[N];
};

} // namespace embed::net
