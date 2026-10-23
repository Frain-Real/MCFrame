// os/usb/msc.hpp
#pragma once
#include "usb.hpp"

namespace embed::usb::msc {

/// Блочное хранилище для MSC. Блок обычно 512 байт.
class BlockDevice {
public:
    virtual ~BlockDevice() = default;
    virtual uint32_t block_count() const noexcept = 0;
    virtual uint16_t block_size() const noexcept = 0;
    virtual Status read_block(uint32_t lba, void* dst, uint32_t count) noexcept = 0;
    virtual Status write_block(uint32_t lba, const void* src, uint32_t count) noexcept = 0;
};

/// Mass Storage Class, Bulk-Only Transport + минимальный SCSI.
class Msc final : public Class {
public:
    static constexpr uint8_t kEpOut = 0x01u;
    static constexpr uint8_t kEpIn  = 0x82u;

    explicit Msc(BlockDevice& dev) noexcept : dev_(&dev) {
        for (Size i = 0u; i < sizeof(config_); ++i) config_[i] = kConfig_[i];
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
        return Status::Ok;
    }

    const uint8_t* config_descriptor(Size& len) const noexcept override {
        len = sizeof(config_);
        return config_;
    }

    Status control(const Setup& s, const uint8_t*, Size,
                   const uint8_t*& in, Size& in_len) noexcept override {
        in = nullptr;
        in_len = 0u;
        if (s.request == 0xFEu) {            // GET_MAX_LUN
            max_lun_ = 0u;
            in = &max_lun_;
            in_len = 1u;
            return Status::Ok;
        }
        if (s.request == 0xFFu) return Status::Ok;   // BOT reset
        return Status::NotSupported;
    }

    /// Обработать 31-байтный CBW. Ответ (данные + CSW) отправляется через Hal.
    Status handle_cbw(const uint8_t* cbw, Size len, Hal& hal,
                      const uint8_t* out_data = nullptr,
                      Size out_len = 0u) noexcept {
        if (cbw == nullptr || len < 31u) return Status::InvalidArg;
        const uint32_t sig = rd32(cbw);
        if (sig != 0x43425355u) return Status::Corruption;
        const uint32_t tag = rd32(cbw + 4u);
        const uint32_t dlen = rd32(cbw + 8u);
        const uint8_t flags = cbw[12];
        const uint8_t cblen = cbw[14];
        if (cblen == 0u || cblen > 16u || 15u + cblen > len) return Status::Corruption;

        uint32_t residue = 0u;
        uint8_t status = 0u;
        Status st = Status::Ok;
        const uint8_t op = cbw[15];

        if (op == 0x00u) {                   // TEST UNIT READY
            // без данных
        } else if (op == 0x12u) {            // INQUIRY
            st = send_data(hal, inquiry_, sizeof(inquiry_), dlen, residue);
        } else if (op == 0x25u) {            // READ CAPACITY(10)
            const uint32_t last = (dev_->block_count() == 0u) ? 0u : dev_->block_count() - 1u;
            put32(cap_, last);
            put32(cap_ + 4u, dev_->block_size());
            st = send_data(hal, cap_, sizeof(cap_), dlen, residue);
        } else if (op == 0x1Au) {            // MODE SENSE(6)
            const uint8_t mode[4] = {3u, 0u, 0u, 0u};
            st = send_data(hal, mode, sizeof(mode), dlen, residue);
        } else if (op == 0x03u) {            // REQUEST SENSE
            uint8_t sense[18]{};
            sense[0] = 0x70u;
            sense[7] = 10u;
            st = send_data(hal, sense, sizeof(sense), dlen, residue);
        } else if (op == 0x28u) {            // READ(10)
            if (cblen < 10u) { st = Status::InvalidArg; }
            else {
                const uint32_t lba = rd32be(cbw + 17u);
                const uint16_t n = static_cast<uint16_t>((cbw[22] << 8) | cbw[23]);
                st = read_blocks(lba, n, dlen, residue, hal);
            }
        } else if (op == 0x2Au) {            // WRITE(10)
            if (cblen < 10u) { st = Status::InvalidArg; }
            else if (out_data == nullptr || out_len < dlen) { st = Status::InvalidArg; }
            else {
                const uint32_t lba = rd32be(cbw + 17u);
                const uint16_t n = static_cast<uint16_t>((cbw[22] << 8) | cbw[23]);
                if (dev_->block_size() == 0u || n == 0u ||
                    lba + n > dev_->block_count() ||
                    static_cast<uint32_t>(n) * dev_->block_size() > out_len) {
                    st = Status::InvalidArg;
                } else {
                    st = dev_->write_block(lba, out_data, n);
                }
            }
        } else {
            st = Status::NotSupported;
        }

        if (st != Status::Ok) {
            status = 1u;                     // failed
            residue = dlen;
        }
        uint8_t csw[13]{};
        put32(csw, 0x53425355u);
        put32(csw + 4u, tag);
        put32(csw + 8u, residue);
        csw[12] = status;
        (void)hal.ep_write(ep_in_, csw, sizeof(csw));
        (void)flags;
        last_status_ = status;
        return st;
    }

    uint8_t last_status() const noexcept { return last_status_; }

private:
    Status send_data(Hal& hal, const void* data, Size len, uint32_t dlen,
                     uint32_t& residue) noexcept {
        Size n = len;
        if (n > dlen) n = dlen;
        const Status st = hal.ep_write(ep_in_, data, n);
        residue = dlen - static_cast<uint32_t>(n);
        return st;
    }

    Status read_blocks(uint32_t lba, uint16_t count, uint32_t dlen,
                       uint32_t& residue, Hal& hal) noexcept {
        const uint32_t bs = dev_->block_size();
        if (bs == 0u || count == 0u || lba + count > dev_->block_count()) return Status::InvalidArg;
        if (static_cast<uint32_t>(count) * bs > sizeof(io_)) return Status::NoMemory;
        const Status st = dev_->read_block(lba, io_, count);
        if (st != Status::Ok) return st;
        return send_data(hal, io_, static_cast<Size>(count) * bs, dlen, residue);
    }

    static uint32_t rd32be(const uint8_t* p) noexcept {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }
    static uint32_t rd32(const uint8_t* p) noexcept {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }
    static void put32(uint8_t* p, uint32_t v) noexcept {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    }

    uint8_t config_[32]{};
    uint8_t ep_out_ = kEpOut;
    uint8_t ep_in_ = kEpIn;

    static constexpr uint8_t kConfig_[32] = {
        9u, 2u, 32u, 0u, 1u, 1u, 0u, 0x80u, 50u,
        9u, 4u, 0u, 0u, 2u, 8u, 6u, 0x50u, 0u,
        7u, 5u, kEpOut, 2u, 64u, 0u, 0u,
        7u, 5u, kEpIn, 2u, 64u, 0u, 0u
    };

    static constexpr uint8_t inquiry_[36] = {
        0x00u, 0x80u, 0x00u, 0x01u, 31u, 0u, 0u, 0u,
        'M','C','F','r','a','m','e',' ','M','S','C',' ',' ',' ',' ',' ',
        '1','.','0','0',' ',' ',' ',' '
    };

    BlockDevice* dev_ = nullptr;
    uint8_t io_[512]{};
    uint8_t cap_[8]{};
    uint8_t max_lun_ = 0u;
    uint8_t last_status_ = 0u;
};

} // namespace embed::usb::msc
