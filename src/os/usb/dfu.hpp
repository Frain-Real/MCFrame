// os/usb/dfu.hpp
#pragma once
#include "usb.hpp"

namespace embed::usb::dfu {

class Target {
public:
    virtual ~Target() = default;
    virtual Size capacity() const noexcept = 0;
    virtual Status begin() noexcept = 0;
    virtual Status write(uint32_t offset, const void* data, Size len) noexcept = 0;
    virtual Status read(uint32_t offset, void* data, Size len) noexcept = 0;
    virtual Status finish() noexcept = 0;
};

class Dfu final : public Class {
public:
    enum class State : uint8_t {
        AppIdle = 0u, AppDetach = 1u, DfuIdle = 2u, DnloadSync = 3u,
        DnBusy = 4u, ManifestSync = 5u, Manifest = 6u, Error = 7u
    };

    explicit Dfu(Target* target = nullptr) noexcept : target_(target) {}

    const uint8_t* config_descriptor(Size& len) const noexcept override {
        len = sizeof(config_);
        return config_;
    }

    Status control(const Setup& s, const uint8_t* out, Size out_len,
                   const uint8_t*& in, Size& in_len) noexcept override {
        in = nullptr;
        in_len = 0u;
        switch (s.request) {
            case 0x00u:                       // DETACH
                state_ = State::AppDetach;
                return Status::Ok;
            case 0x01u:                       // DNLOAD
                if (s.length == 0u) return manifest();
                return dnload(out, out_len, s.length);
            case 0x02u: {                     // UPLOAD
                if (upload_pos_ >= size_) return Status::NotFound;
                Size n = size_ - upload_pos_;
                if (n > s.length) n = s.length;
                if (n > sizeof(upload_buf_)) n = sizeof(upload_buf_);
                if (target_ != nullptr) {
                    if (target_->read(upload_pos_, upload_buf_, n) != Status::Ok) {
                        state_ = State::Error;
                        status_code_ = 1u;
                        return Status::HardwareError;
                    }
                    in = upload_buf_;
                } else {
                    in = data_ + upload_pos_;
                }
                in_len = n;
                upload_pos_ += n;
                state_ = State::DfuIdle;
                return Status::Ok;
            }
            case 0x03u:                       // GETSTATUS
                status_[0] = status_code_;
                status_[1] = 0u; status_[2] = 0u; status_[3] = 0u;
                status_[4] = static_cast<uint8_t>(state_);
                status_[5] = 0u;
                in = status_;
                in_len = sizeof(status_);
                return Status::Ok;
            case 0x04u:                       // CLRSTATUS
                status_code_ = 0u;
                state_ = State::DfuIdle;
                return Status::Ok;
            case 0x05u:                       // GETSTATE
                state_byte_ = static_cast<uint8_t>(state_);
                in = &state_byte_;
                in_len = 1u;
                return Status::Ok;
            case 0x06u:                       // ABORT
                state_ = State::DfuIdle;
                offset_ = 0u;
                upload_pos_ = 0u;
                begun_ = false;
                status_code_ = 0u;
                return Status::Ok;
            default:
                return Status::NotSupported;
        }
    }

    State state() const noexcept { return state_; }
    Size size() const noexcept { return size_; }
    const uint8_t* data() const noexcept { return data_; }
    uint8_t status_code() const noexcept { return status_code_; }

private:
    Status manifest() noexcept {
        if (target_ != nullptr) {
            const Status st = target_->finish();
            if (st != Status::Ok) {
                state_ = State::Error;
                status_code_ = 1u;
                return st;
            }
        }
        state_ = State::Manifest;
        status_code_ = 0u;
        return Status::Ok;
    }

    Status dnload(const uint8_t* out, Size out_len, uint16_t wlength) noexcept {
        if (out == nullptr || out_len == 0u) return Status::InvalidArg;
        if (out_len > wlength) out_len = wlength;
        if (out_len == 0u) return Status::InvalidArg;

        if (target_ != nullptr) {
            if (!begun_) {
                const Status bs = target_->begin();
                if (bs != Status::Ok) {
                    state_ = State::Error;
                    status_code_ = 1u;
                    return bs;
                }
                begun_ = true;
            }
            if (offset_ + out_len > target_->capacity()) {
                state_ = State::Error;
                status_code_ = 1u;
                return Status::NoMemory;
            }
            const Status ws = target_->write(offset_, out, out_len);
            if (ws != Status::Ok) {
                state_ = State::Error;
                status_code_ = 1u;
                return ws;
            }
        } else {
            if (offset_ + out_len > sizeof(data_)) {
                state_ = State::Error;
                status_code_ = 1u;
                return Status::NoMemory;
            }
            for (Size i = 0u; i < out_len; ++i) data_[offset_ + i] = out[i];
        }
        offset_ += out_len;
        size_ = offset_;
        state_ = State::DfuIdle;
        status_code_ = 0u;
        return Status::Ok;
    }

    static constexpr uint8_t config_[18] = {
        9u, 2u, 18u, 0u, 1u, 1u, 0u, 0x80u, 50u,
        9u, 4u, 0u, 0u, 0u, 0xFEu, 1u, 2u, 0u
    };

    static constexpr Size kCapacity = 4096u;
    uint8_t data_[kCapacity]{};
    Size    size_ = 0u;
    Size    offset_ = 0u;
    Size    upload_pos_ = 0u;
    Target* target_ = nullptr;
    bool    begun_ = false;
    uint8_t upload_buf_[64]{};
    State   state_ = State::AppIdle;
    uint8_t status_code_ = 0u;
    uint8_t status_[6]{};
    uint8_t state_byte_ = 0u;
};

} // namespace embed::usb::dfu
