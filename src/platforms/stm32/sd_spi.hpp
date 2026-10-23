// platforms/stm32/sd_spi.hpp
// ---------------------------------------------------------------------------
// SD/SDHC в SPI-режиме. Драйвер не зависит от SoC: hal::ISpi + CS GPIO.
// Инициализация CMD0/CMD8/ACMD41/CMD58, чтение/запись 512-байтных блоков.
// CRC в SPI-режиме не проверяется (SD разрешает), но команды CMD8/CMD0
// передаются с корректным CRC.
// ---------------------------------------------------------------------------
#pragma once

#include "../../os/fs/block_device.hpp"
#include "../../hal/ISpi.hpp"
#include "../../hal/IGpio.hpp"
#include "../../os/config/time_config.hpp"

namespace embed::stm32::sd {

class Card final : public fs::BlockDevice {
public:
    Card(hal::ISpi& bus, hal::IGpioPin& cs) noexcept : bus_(&bus), cs_(&cs) {}

    Status init() noexcept {
        if (bus_ == nullptr || cs_ == nullptr) return Status::InvalidArg;
        hal::SpiConfig slow{};
        slow.hz = 400'000u;
        slow.mode = 0u;
        slow.msb_first = true;
        Status st = bus_->configure(slow);
        if (st != Status::Ok) return st;
        hal::GpioConfig out{};
        out.mode = hal::GpioMode::Output;
        out.initial = true;
        st = cs_->configure(out);
        if (st != Status::Ok) return st;

        // 80 тактов с CS=1.
        uint8_t ff[10];
        for (uint8_t& b : ff) b = 0xFFu;
        cs_->set(true);
        (void)bus_->transfer(ff, nullptr, sizeof(ff), 100u);

        uint8_t r = 0u;
        for (uint8_t i = 0u; i < 10u; ++i) {
            if (command(0u, 0u, 0x95u, r) && r == 0x01u) break;
        }
        if (r != 0x01u) return Status::HardwareError;
        (void)command(8u, 0x1AAu, 0x87u, r);
        if (r != 0x01u) return Status::HardwareError;       // v1/SDSC
        const uint64_t start = config::OsTimer::TimeAvailable()
            ? config::OsTimer::NowMs() : 0u;
        for (;;) {
            if (!command(55u, 0u, 0u, r)) return Status::HardwareError;
            if (!command(41u, 0x40000000u, 0u, r)) return Status::HardwareError;
            if (r == 0x00u) break;
            if (config::OsTimer::TimeAvailable() &&
                config::OsTimer::NowMs() - start > 2000u) return Status::Timeout;
            delay_ms(1u);
        }
        (void)command(58u, 0u, 0u, r);
        if (r != 0x00u) return Status::HardwareError;
        uint8_t ocr[4]{};
        read_bytes(ocr, 4u);
        sdhc_ = (ocr[0] & 0x40u) != 0u;
        (void)command(9u, 0u, 0u, r);
        if (r != 0x00u) return Status::HardwareError;
        uint8_t csd[16]{};
        if (read_data(csd, sizeof(csd)) != Status::Ok) return Status::HardwareError;
        if (sdhc_) {
            const uint32_t c_size = ((static_cast<uint32_t>(csd[7]) & 0x3Fu) << 16) |
                                    (static_cast<uint32_t>(csd[8]) << 8) | csd[9];
            blocks_ = (c_size + 1u) * 1024u;
        } else {
            const uint32_t c_size = ((static_cast<uint32_t>(csd[6]) & 0x03u) << 10) |
                                    (static_cast<uint32_t>(csd[7]) << 2) |
                                    ((static_cast<uint32_t>(csd[8]) >> 6) & 0x03u);
            const uint32_t c_size_mult = ((static_cast<uint32_t>(csd[9]) & 0x03u) << 1) |
                                         ((static_cast<uint32_t>(csd[10]) >> 7) & 0x01u);
            const uint32_t read_bl_len = csd[5] & 0x0Fu;
            const uint32_t capacity = (c_size + 1u) << (c_size_mult + 2u + read_bl_len);
            blocks_ = capacity / 512u;
        }
        if (blocks_ == 0u) return Status::Corruption;
        hal::SpiConfig fast{};
        fast.hz = 25'000'000u;
        fast.mode = 0u;
        fast.msb_first = true;
        (void)bus_->configure(fast);
        cs_->set(true);
        inited_ = true;
        return Status::Ok;
    }

    uint32_t block_count() const noexcept override { return blocks_; }
    uint32_t block_size() const noexcept override { return 512u; }

    Status read(uint32_t lba, void* data, uint32_t count) noexcept override {
        if (!inited_ || data == nullptr || count == 0u) return Status::InvalidArg;
        if (lba >= blocks_ || count > blocks_ - lba) return Status::InvalidArg;
        auto* out = static_cast<uint8_t*>(data);
        for (uint32_t i = 0u; i < count; ++i) {
            uint8_t r = 0u;
            if (!command(sdhc_ ? 17u : 17u, sdhc_ ? lba + i : (lba + i) * 512u, 0u, r) ||
                r != 0x00u) {
                return Status::HardwareError;
            }
            const Status st = read_data(out + i * 512u, 512u);
            if (st != Status::Ok) return st;
        }
        cs_->set(true);
        return Status::Ok;
    }

    Status write(uint32_t lba, const void* data, uint32_t count) noexcept override {
        if (!inited_ || data == nullptr || count == 0u) return Status::InvalidArg;
        if (lba >= blocks_ || count > blocks_ - lba) return Status::InvalidArg;
        const auto* in = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0u; i < count; ++i) {
            uint8_t r = 0u;
            if (!command(sdhc_ ? 24u : 24u, sdhc_ ? lba + i : (lba + i) * 512u, 0u, r) ||
                r != 0x00u) {
                return Status::HardwareError;
            }
            // Start block token, data, 2-byte CRC (0 acceptable in SPI).
            uint8_t token = 0xFEu;
            (void)bus_->transfer(&token, nullptr, 1u, 1000u);
            for (uint32_t b = 0u; b < 512u; ++b) {
                const uint8_t v = in[i * 512u + b];
                (void)bus_->transfer(&v, nullptr, 1u, 1000u);
            }
            uint8_t crc[2] = {0xFFu, 0xFFu};
            (void)bus_->transfer(crc, nullptr, 2u, 1000u);
            uint8_t resp = 0u;
            for (uint8_t g = 0u; g < 64u; ++g) {
                (void)bus_->transfer(nullptr, &resp, 1u, 1000u);
                if ((resp & 0x1Fu) == 0x05u) break;
            }
            if ((resp & 0x1Fu) != 0x05u) return Status::HardwareError;
            // Busy: 0x00 until 0xFF.
            for (uint32_t g = 0u; g < 1000000u; ++g) {
                (void)bus_->transfer(nullptr, &resp, 1u, 1000u);
                if (resp == 0xFFu) break;
            }
        }
        cs_->set(true);
        return Status::Ok;
    }

    Status sync() noexcept override { return inited_ ? Status::Ok : Status::Unavailable; }
    bool initialized() const noexcept { return inited_; }

private:
    static void delay_ms(uint32_t ms) noexcept {
        if (!config::OsTimer::TimeAvailable()) {
            for (volatile uint32_t i = 0u; i < ms * 8000u; ++i) { }
            return;
        }
        const uint64_t start = config::OsTimer::NowMs();
        while (config::OsTimer::NowMs() - start < ms) { }
    }
    bool command(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t& response) noexcept {
        cs_->set(false);
        uint8_t b[6] = {static_cast<uint8_t>(0x40u | cmd),
                        static_cast<uint8_t>(arg >> 24), static_cast<uint8_t>(arg >> 16),
                        static_cast<uint8_t>(arg >> 8), static_cast<uint8_t>(arg), crc};
        (void)bus_->transfer(b, nullptr, sizeof(b), 1000u);
        response = 0xFFu;
        for (uint8_t i = 0u; i < 8u; ++i) {
            (void)bus_->transfer(nullptr, &response, 1u, 1000u);
            if ((response & 0x80u) == 0u) return true;
        }
        cs_->set(true);
        return false;
    }
    void read_bytes(uint8_t* data, Size len) noexcept {
        for (Size i = 0u; i < len; ++i) (void)bus_->transfer(nullptr, &data[i], 1u, 1000u);
    }
    Status read_data(uint8_t* data, Size len) noexcept {
        uint8_t token = 0xFFu;
        for (uint32_t g = 0u; g < 100000u; ++g) {
            (void)bus_->transfer(nullptr, &token, 1u, 1000u);
            if (token == 0xFEu) {
                read_bytes(data, len);
                uint8_t crc[2]{};
                read_bytes(crc, 2u);
                return Status::Ok;
            }
            if (token != 0xFFu) return Status::Corruption;
        }
        return Status::Timeout;
    }

    hal::ISpi* bus_ = nullptr;
    hal::IGpioPin* cs_ = nullptr;
    uint32_t blocks_ = 0u;
    bool sdhc_ = false;
    bool inited_ = false;
};

} // namespace embed::stm32::sd
