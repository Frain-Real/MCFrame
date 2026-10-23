// os/usb/composite.hpp
// ---------------------------------------------------------------------------
// Composite USB device: несколько Class-функций в одной configuration.
//
// Дочерние классы отдают СВОЙ configuration descriptor (config header +
// interface/functional/endpoint). Composite склеивает их, перенумеровывает
// bInterfaceNumber и проверяет, что endpoint-адреса не пересекаются.
//
// Ограничение: дочерние классы должны использовать непересекающиеся
// endpoint-адреса. CDC (0x81/0x01/0x82) + DFU (только EP0) подходят; CDC+MSC
// потребует remap endpoint-ов на уровне класса.
// ---------------------------------------------------------------------------
#pragma once

#include "usb.hpp"

namespace embed::usb {

template <Size MaxChildren = 3u, Size DescriptorCap = 256u>
class Composite final : public Class {
public:
    Composite(Class* const* children, Size count) noexcept
        : children_(children), count_(count > MaxChildren ? MaxChildren : count) {}

    Status init(Device* dev) noexcept override {
        if (children_ == nullptr || count_ == 0u) return Status::InvalidArg;
        base_[0] = 0u;
        for (Size i = 0u; i < count_; ++i) {
            if (children_[i] == nullptr) return Status::InvalidArg;
            const Status st = children_[i]->init(dev);
            if (st != Status::Ok) return st;
            Size n = 0u;
            const uint8_t* d = children_[i]->config_descriptor(n);
            if (d == nullptr || n < 9u || d[1] != 2u) return Status::InvalidArg;
            base_[i + 1u] = static_cast<uint8_t>(base_[i] + d[4]);
        }
        return build();
    }

    const uint8_t* config_descriptor(Size& len) const noexcept override {
        len = valid_ ? len_ : 0u;
        return valid_ ? desc_ : nullptr;
    }

    Status control(const Setup& s, const uint8_t* out, Size out_len,
                   const uint8_t*& in, Size& in_len) noexcept override {
        const int child = child_for_interface(static_cast<uint8_t>(s.index));
        if (child < 0) return Status::NotSupported;
        return children_[static_cast<Size>(child)]->control(s, out, out_len, in, in_len);
    }

    void configured() noexcept override {
        for (Size i = 0u; i < count_; ++i) children_[i]->configured();
        configured_ = true;
    }

    void set_interface(uint16_t iface) noexcept override {
        const int child = child_for_interface(static_cast<uint8_t>(iface));
        if (child >= 0) {
            children_[static_cast<Size>(child)]->set_interface(
                static_cast<uint16_t>(iface - base_[static_cast<Size>(child)]));
        }
    }

    bool is_configured() const noexcept { return configured_; }
    Size descriptor_length() const noexcept { return valid_ ? len_ : 0u; }

private:
    int child_for_interface(uint8_t iface) const noexcept {
        for (Size i = 0u; i < count_; ++i) {
            if (iface >= base_[i] && iface < base_[i + 1u]) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    Status build() noexcept {
        desc_[0] = 9u; desc_[1] = 2u;
        desc_[2] = 0u; desc_[3] = 0u;             // wTotalLength later
        desc_[4] = base_[count_];                  // bNumInterfaces
        desc_[5] = 1u;                             // bConfigurationValue
        desc_[6] = 0u;
        desc_[7] = 0x80u;
        desc_[8] = 50u;
        Size out = 9u;
        bool endpoints[256]{};
        for (Size i = 0u; i < count_; ++i) {
            Size n = 0u;
            const uint8_t* d = children_[i]->config_descriptor(n);
            if (d == nullptr || n < 9u) return Status::InvalidArg;
            Size pos = 9u;
            while (pos + 2u <= n) {
                const uint8_t blen = d[pos];
                const uint8_t type = d[pos + 1u];
                if (blen < 2u || pos + blen > n) return Status::Corruption;
                if (type == 4u && blen >= 9u) {
                    if (out + blen > sizeof(desc_)) return Status::NoMemory;
                    for (uint8_t k = 0u; k < blen; ++k) desc_[out + k] = d[pos + k];
                    desc_[out + 2u] = static_cast<uint8_t>(base_[i] + d[pos + 2u]);
                } else if (type == 5u && blen >= 7u) {
                    const uint8_t ep = d[pos + 2u];
                    // Адрес 0 зарезервирован; проверяем только реальные EP.
                    if (ep != 0u && endpoints[ep]) return Status::NotSupported;
                    endpoints[ep] = true;
                    if (out + blen > sizeof(desc_)) return Status::NoMemory;
                    for (uint8_t k = 0u; k < blen; ++k) desc_[out + k] = d[pos + k];
                } else {
                    if (out + blen > sizeof(desc_)) return Status::NoMemory;
                    for (uint8_t k = 0u; k < blen; ++k) desc_[out + k] = d[pos + k];
                }
                out += blen;
                pos += blen;
            }
        }
        len_ = out;
        desc_[2] = static_cast<uint8_t>(len_ & 0xFFu);
        desc_[3] = static_cast<uint8_t>(len_ >> 8);
        valid_ = true;
        return Status::Ok;
    }

    Class* const* children_ = nullptr;
    Size count_ = 0u;
    uint8_t base_[MaxChildren + 1u]{};
    uint8_t desc_[DescriptorCap]{};
    Size len_ = 0u;
    bool valid_ = false;
    bool configured_ = false;
};

} // namespace embed::usb
