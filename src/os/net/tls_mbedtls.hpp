// os/net/tls_mbedtls.hpp
// ---------------------------------------------------------------------------
// TLS-провайдер на mbedTLS для net::TlsFactoryT.
//
// Заголовок намеренно не входит в общую сборку MCFrame: mbedTLS подключает
// приложение (на C3 он уже есть, на STM32 его нужно добавить в проект).
// TlsSession работает поверх обычного net::Socket, поэтому raw TCP/DNS
// остаются прежними.
// ---------------------------------------------------------------------------
#pragma once

#include "tls.hpp"
#if MBEDTLS_VERSION_MAJOR >= 4
#include <psa/crypto.h>
#endif

#if __has_include(<mbedtls/build_info.h>)
#include <mbedtls/build_info.h>
#else
#include <mbedtls/config.h>
#endif
#include <mbedtls/error.h>
#if MBEDTLS_VERSION_MAJOR < 4
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#endif
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

namespace embed::net {

class MbedTlsSession final : public TlsSession {
public:
    MbedTlsSession() noexcept = default;
    ~MbedTlsSession() noexcept override { close(); }

    Status start(Socket& raw, const Endpoint&, const char* host,
                 const TlsConfig& cfg, uint32_t timeout_ms) noexcept override {
        close();
        raw_ = &raw;
        timeout_ms_ = timeout_ms;
        active_ = false;

        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&conf_);
        mbedtls_x509_crt_init(&ca_);
#if MBEDTLS_VERSION_MAJOR < 4
        mbedtls_ctr_drbg_init(&drbg_);
        mbedtls_entropy_init(&entropy_);
#endif
        initialized_ = true;

        int rc = 0;
#if MBEDTLS_VERSION_MAJOR >= 4
        if (psa_crypto_init() != PSA_SUCCESS) return fail(-1);
#endif
#if MBEDTLS_VERSION_MAJOR < 4
        static const char kPersonalization[] = "mcframe-tls";
        rc = mbedtls_ctr_drbg_seed(&drbg_, mbedtls_entropy_func, &entropy_,
                                   reinterpret_cast<const unsigned char*>(kPersonalization),
                                   sizeof(kPersonalization) - 1u);
        if (rc != 0) return fail(rc);
#endif

        rc = mbedtls_ssl_config_defaults(&conf_, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc != 0) return fail(rc);

#if MBEDTLS_VERSION_MAJOR < 4
        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &drbg_);
#endif
        if (cfg.ca != nullptr && cfg.ca_len != 0u) {
            rc = mbedtls_x509_crt_parse(&ca_, cfg.ca, cfg.ca_len);
            if (rc != 0) return fail(rc);
        }
        const int auth = cfg.verify ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE;
        mbedtls_ssl_conf_authmode(&conf_, auth);
        if (cfg.ca != nullptr && cfg.ca_len != 0u) {
            mbedtls_ssl_conf_ca_chain(&conf_, &ca_, nullptr);
        }
        if (timeout_ms != 0u) mbedtls_ssl_conf_read_timeout(&conf_, timeout_ms);
        if (cfg.alpn != nullptr) {
            alpn_protocols_[0] = cfg.alpn;
            alpn_protocols_[1] = nullptr;
            mbedtls_ssl_conf_alpn_protocols(&conf_, alpn_protocols_);
        }

        rc = mbedtls_ssl_setup(&ssl_, &conf_);
        if (rc != 0) return fail(rc);
        if (host != nullptr) {
            rc = mbedtls_ssl_set_hostname(&ssl_, host);
            if (rc != 0) return fail(rc);
        }
        mbedtls_ssl_set_bio(&ssl_, this, &MbedTlsSession::send_cb,
                            &MbedTlsSession::recv_cb, nullptr);

        for (;;) {
            rc = mbedtls_ssl_handshake(&ssl_);
            if (rc == 0) break;
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            return fail(rc);
        }
        active_ = true;
        return Status::Ok;
    }

    Status send(const void* data, Size len, Size& sent) noexcept override {
        sent = 0u;
        if (!active_) return Status::Unavailable;
        const int rc = mbedtls_ssl_write(&ssl_,
            static_cast<const unsigned char*>(data), len);
        if (rc < 0) return map(rc);
        sent = static_cast<Size>(rc);
        return Status::Ok;
    }

    Status recv(void* data, Size cap, Size& got,
                uint32_t timeout_ms) noexcept override {
        got = 0u;
        if (!active_) return Status::Unavailable;
        if (timeout_ms != 0u) timeout_ms_ = timeout_ms;
        const int rc = mbedtls_ssl_read(&ssl_, static_cast<unsigned char*>(data), cap);
        if (rc == 0) return Status::Unavailable;
        if (rc < 0) return map(rc);
        got = static_cast<Size>(rc);
        return Status::Ok;
    }

    void close() noexcept override {
        if (!initialized_) { raw_ = nullptr; active_ = false; return; }
        if (active_) (void)mbedtls_ssl_close_notify(&ssl_);
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&conf_);
        mbedtls_x509_crt_free(&ca_);
#if MBEDTLS_VERSION_MAJOR < 4
        mbedtls_ctr_drbg_free(&drbg_);
        mbedtls_entropy_free(&entropy_);
#endif
        initialized_ = false;
        active_ = false;
        raw_ = nullptr;
    }

private:
    static int send_cb(void* ctx, const unsigned char* buf, size_t len) noexcept {
        auto* self = static_cast<MbedTlsSession*>(ctx);
        if (self == nullptr || self->raw_ == nullptr) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        Size sent = 0u;
        const Status st = self->raw_->send(buf, len, sent);
        if (st == Status::Ok) return static_cast<int>(sent);
        if (st == Status::Busy) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }

    static int recv_cb(void* ctx, unsigned char* buf, size_t len) noexcept {
        auto* self = static_cast<MbedTlsSession*>(ctx);
        if (self == nullptr || self->raw_ == nullptr) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
        Size got = 0u;
        const Status st = self->raw_->recv(buf, len, got, self->timeout_ms_);
        if (st == Status::Ok) return static_cast<int>(got);
        if (st == Status::Timeout) return MBEDTLS_ERR_SSL_TIMEOUT;
        if (st == Status::Busy) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }

    Status fail(int) noexcept {
        close();
        return Status::NotSupported;
    }

    static Status map(int rc) noexcept {
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT) return Status::Timeout;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return Status::Busy;
        }
        return Status::Aborted;
    }

    Socket* raw_ = nullptr;
    uint32_t timeout_ms_ = 5000u;
    bool initialized_ = false;
    bool active_ = false;
    const char* alpn_protocols_[2]{};
    mbedtls_ssl_context ssl_{};
    mbedtls_ssl_config conf_{};
    mbedtls_x509_crt ca_{};
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ctr_drbg_context drbg_{};
    mbedtls_entropy_context entropy_{};
#endif
};

template <Size N = 4u>
class MbedTlsProvider final : public TlsProvider {
    static_assert(N >= 1u && N <= 8u, "TLS: 1..8 sessions");
public:
    Status create(TlsSession*& out) noexcept override {
        for (Size i = 0u; i < N; ++i) {
            if (!used_[i]) {
                used_[i] = true;
                out = &sessions_[i];
                return Status::Ok;
            }
        }
        out = nullptr;
        return Status::NoMemory;
    }

    void release(TlsSession* s) noexcept override {
        for (Size i = 0u; i < N; ++i) {
            if (&sessions_[i] == s) {
                sessions_[i].close();
                used_[i] = false;
                return;
            }
        }
    }

private:
    MbedTlsSession sessions_[N];
    bool used_[N] = {};
};

} // namespace embed::net
