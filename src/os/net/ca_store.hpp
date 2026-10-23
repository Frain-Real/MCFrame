// os/net/ca_store.hpp
// ---------------------------------------------------------------------------
// Статический CA-store для TLS и provisioning через NVS.
// Хранит несколько DER/PEM-сертификатов в одном contiguous bundle, который
// передаётся в TlsConfig::ca. Без malloc; размеры фиксированы.
// ---------------------------------------------------------------------------
#pragma once

#include "tls.hpp"
#include "../nvs/nvs.hpp"

namespace embed::net {

class CaStore {
public:
    static constexpr Size kMaxCerts = 4u;
    static constexpr Size kMaxCert = 2048u;
    static constexpr Size kBundleCap = kMaxCerts * kMaxCert;

    Status add(uint32_t id, const void* data, Size len) noexcept {
        if (data == nullptr || len == 0u || len > kMaxCert) return Status::InvalidArg;
        if (find(id) != nullptr) return Status::AlreadyExists;
        if (count_ >= kMaxCerts) return Status::NoMemory;
        Cert& c = certs_[count_++];
        c.id = id;
        c.len = static_cast<uint16_t>(len);
        const auto* p = static_cast<const uint8_t*>(data);
        for (Size i = 0u; i < len; ++i) c.data[i] = p[i];
        rebuild();
        return Status::Ok;
    }

    Status remove(uint32_t id) noexcept {
        Cert* c = find(id);
        if (c == nullptr) return Status::NotFound;
        const Size idx = static_cast<Size>(c - certs_);
        for (Size i = idx + 1u; i < count_; ++i) certs_[i - 1u] = certs_[i];
        --count_;
        rebuild();
        return Status::Ok;
    }

    void clear() noexcept { count_ = 0u; bundle_len_ = 0u; }
    Size count() const noexcept { return count_; }
    const uint8_t* bundle() const noexcept { return bundle_; }
    Size bundle_len() const noexcept { return bundle_len_; }

    TlsConfig config(bool verify = true, const char* alpn = nullptr) const noexcept {
        TlsConfig c{};
        c.ca = bundle_len_ != 0u ? bundle_ : nullptr;
        c.ca_len = bundle_len_;
        c.verify = verify;
        c.alpn = alpn;
        return c;
    }

    /// Сохранить bundle в NVS (ключ должен быть <= 15 символов).
    Status save(hal::INvs& nvs, const char* key = "tls_ca") noexcept {
        if (bundle_len_ == 0u) return Status::InvalidArg;
        return nvs.write(key, bundle_, bundle_len_);
    }

    /// Загрузить bundle из NVS; он трактуется как один PEM/DER bundle.
    Status load(hal::INvs& nvs, const char* key = "tls_ca") noexcept {
        Size got = 0u;
        const Status st = nvs.read(key, bundle_, sizeof(bundle_), got);
        if (st != Status::Ok) return st;
        if (got == 0u) return Status::NotFound;
        count_ = 0u;
        // Сохраняем как один bundle; отдельные cert id не восстанавливаем.
        Cert& c = certs_[0];
        c.id = 0u;
        c.len = static_cast<uint16_t>(got > kMaxCert ? kMaxCert : got);
        for (Size i = 0u; i < c.len; ++i) c.data[i] = bundle_[i];
        count_ = 1u;
        rebuild();
        return Status::Ok;
    }

private:
    struct Cert {
        uint32_t id = 0u;
        uint16_t len = 0u;
        uint8_t data[kMaxCert]{};
    };
    Cert* find(uint32_t id) noexcept {
        for (Size i = 0u; i < count_; ++i) if (certs_[i].id == id) return &certs_[i];
        return nullptr;
    }
    void rebuild() noexcept {
        bundle_len_ = 0u;
        for (Size i = 0u; i < count_; ++i) {
            for (Size k = 0u; k < certs_[i].len; ++k) {
                bundle_[bundle_len_++] = certs_[i].data[k];
            }
        }
    }

    Cert certs_[kMaxCerts]{};
    Size count_ = 0u;
    uint8_t bundle_[kBundleCap]{};
    Size bundle_len_ = 0u;
};

} // namespace embed::net
