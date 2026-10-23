// os/usb/cdc_acm.hpp
#pragma once
#include "usb.hpp"

namespace embed::usb::cdc {

/// CDC-ACM (Abstract Control Model) с bulk IN/OUT. Уведомления (serial state)
/// не отправляются автоматически — приложение может вызвать notify().
class Acm final : public Class {
public:
    static constexpr uint8_t kEpOut = 0x01u;
    static constexpr uint8_t kEpIn  = 0x82u;
    static constexpr uint8_t kEpNotify = 0x81u;
    static constexpr Size kBuffer = 256u;

    Acm() noexcept {
        for (Size i = 0u; i < sizeof(config_); ++i) config_[i] = kConfig_[i];
    }

    const uint8_t* config_descriptor(Size& len) const noexcept override {
        len = sizeof(config_);
        return config_;
    }

    Status remap_endpoint(uint8_t from, uint8_t to) noexcept override {
        if (from == 0u || to == 0u) return Status::InvalidArg;
        bool found = false;
        for (Size i = 0u; i < sizeof(config_); ) {
            const uint8_t blen = config_[i];
            if (blen < 2u || i + blen > sizeof(config_)) return Status::Corruption;
            if (config_[i + 1u] == 5u && blen >= 7u && config_[i + 2u] == from) {
                config_[i + 2u] = to;
                found = true;
            }
            i += blen;
        }
        if (!found) return Status::NotFound;
        if (from == kEpOut) ep_out_ = to;
        if (from == kEpIn) ep_in_ = to;
        if (from == kEpNotify) ep_notify_ = to;
        return Status::Ok;
    }

    Status control(const Setup& s, const uint8_t* out, Size out_len,
                   const uint8_t*& in, Size& in_len) noexcept override {
        in = nullptr;
        in_len = 0u;
        if (s.request == 0x20u) {                 // SET_LINE_CODING
            if (out == nullptr || out_len < sizeof(line_)) return Status::InvalidArg;
            for (Size i = 0u; i < sizeof(line_); ++i) line_[i] = out[i];
            return Status::Ok;
        }
        if (s.request == 0x21u) {                 // GET_LINE_CODING
            in = line_;
            in_len = sizeof(line_);
            return Status::Ok;
        }
        if (s.request == 0x22u) {                 // SET_CONTROL_LINE_STATE
            line_state_ = s.value;
            return Status::Ok;
        }
        if (s.request == 0x23u) {                 // SEND_BREAK
            break_ms_ = s.value;
            return Status::Ok;
        }
        return Status::NotSupported;
    }

    void configured() noexcept override { configured_ = true; }
    bool is_configured() const noexcept { return configured_; }

    /// Данные от хоста (OUT endpoint).
    void on_out(const uint8_t* data, Size len) noexcept {
        for (Size i = 0u; i < len && count_ < sizeof(buf_); ++i) {
            const Size pos = (head_ + count_) % sizeof(buf_);
            buf_[pos] = data[i];
            ++count_;
        }
    }

    /// Передать данные устройству->хост. Возврат — сколько принято.
    Status write(Hal& hal, const void* data, Size len, Size& sent) noexcept {
        const Status st = hal.ep_write(ep_in_, data, len);
        sent = (st == Status::Ok) ? len : 0u;
        return st;
    }

    /// Прочитать принятые данные. 0 — буфер пуст.
    Size read(void* dst, Size cap) noexcept {
        auto* p = static_cast<uint8_t*>(dst);
        Size n = 0u;
        while (n < cap && count_ > 0u) {
            p[n++] = buf_[head_];
            head_ = (head_ + 1u) % sizeof(buf_);
            --count_;
        }
        return n;
    }

    /// Отправить class notification (serial state) на EP 0x81.
    Status notify(Hal& hal, uint16_t state) noexcept {
        uint8_t b[10] = {0xA1u, 0x20u, 0x00u, 0x00u, 0x00u, 0x00u, 0x02u, 0x00u,
                         static_cast<uint8_t>(state & 0xFFu),
                         static_cast<uint8_t>(state >> 8)};
        return hal.ep_write(ep_notify_, b, sizeof(b));
    }

    uint16_t line_state() const noexcept { return line_state_; }
    const uint8_t* line_coding() const noexcept { return line_; }

private:
    uint8_t config_[67]{};
    uint8_t ep_out_ = kEpOut;
    uint8_t ep_in_ = kEpIn;
    uint8_t ep_notify_ = kEpNotify;

    static constexpr uint8_t kConfig_[67] = {
        9u, 2u, 67u, 0u, 2u, 1u, 0u, 0x80u, 50u,
        9u, 4u, 0u, 0u, 2u, 2u, 1u, 0u, 0u,
        5u, 0x24u, 0x00u, 0x10u, 0x01u,
        5u, 0x24u, 0x01u, 0x00u, 1u,
        4u, 0x24u, 0x02u, 0x02u,
        5u, 0x24u, 0x06u, 0u, 1u,
        7u, 5u, 0x81u, 3u, 16u, 0u, 16u,
        9u, 4u, 1u, 0u, 0u, 0x0Au, 0u, 0u, 0u,
        7u, 5u, kEpOut, 2u, 64u, 0u, 0u,
        7u, 5u, kEpIn, 2u, 64u, 0u, 0u
    };

    uint8_t line_[7] = {0x00, 0xC2u, 0x01u, 0x00, 0x00, 0x00, 0x08u};
    uint16_t line_state_ = 0u;
    uint16_t break_ms_ = 0u;
    bool configured_ = false;
    uint8_t buf_[kBuffer]{};
    Size head_ = 0u;
    Size count_ = 0u;
};

} // namespace embed::usb::cdc
