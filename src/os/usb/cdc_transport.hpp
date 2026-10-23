// os/usb/cdc_transport.hpp
// ---------------------------------------------------------------------------
// CDC-ACM как транспорт OTA и как порт shell.
//
// Приложение обязано периодически вызывать usb::Hal::poll()/OTG poll и
// передавать bulk OUT в Acm::on_out(); после этого:
//   * AcmTransport::read() отдаёт байты протоколу OTA;
//   * AcmConsole::poll() кормит shell::Console.
// ---------------------------------------------------------------------------
#pragma once

#include "cdc_acm.hpp"
#include "../ota/ota_transport.hpp"
#include "../shell/shell.hpp"

namespace embed::usb::cdc {

class AcmTransport final : public ota::Transport {
public:
    AcmTransport(Acm& acm, Hal& hal, const char* name = "usb-cdc") noexcept
        : acm_(&acm), hal_(&hal), name_(name) {}

    Status begin() noexcept override { return Status::Ok; }
    void end() noexcept override {}

    Status read(void* buffer, Size capacity, Size& bytes_read,
                uint32_t) noexcept override {
        bytes_read = acm_->read(buffer, capacity);
        return (bytes_read != 0u) ? Status::Ok : Status::Timeout;
    }

    Status write(const void* data, Size len, uint32_t) noexcept override {
        Size sent = 0u;
        const Status st = acm_->write(*hal_, data, len, sent);
        return (st == Status::Ok && sent == len) ? Status::Ok : Status::Aborted;
    }

    uint16_t max_payload() const noexcept override { return 252u; }
    ota::proto::Iface iface() const noexcept override { return ota::proto::Iface::Usb; }
    const char* name() const noexcept override { return name_; }

private:
    Acm* acm_ = nullptr;
    Hal* hal_ = nullptr;
    const char* name_ = "usb-cdc";
};

/// Порт shell поверх того же CDC. Управление — Console::feed()/write().
class AcmConsole {
public:
    AcmConsole(Acm& acm, Hal& hal) noexcept : acm_(&acm), hal_(&hal) {}

    void init(shell::Console& console) noexcept {
        console_ = &console;
        console.init(&AcmConsole::writer, this);
    }

    /// Забрать все накопленные OUT-байты и покормить консоль.
    void poll() noexcept {
        if (console_ == nullptr) return;
        uint8_t buf[32]{};
        for (;;) {
            const Size n = acm_->read(buf, sizeof(buf));
            if (n == 0u) break;
            for (Size i = 0u; i < n; ++i) console_->feed(static_cast<char>(buf[i]));
        }
    }

    static void writer(void* ctx, const char* text) noexcept {
        auto* self = static_cast<AcmConsole*>(ctx);
        if (self == nullptr || text == nullptr) return;
        Size n = 0u;
        while (text[n] != '\0') ++n;
        Size sent = 0u;
        (void)self->acm_->write(*self->hal_, text, n, sent);
    }

private:
    Acm* acm_ = nullptr;
    Hal* hal_ = nullptr;
    shell::Console* console_ = nullptr;
};

} // namespace embed::usb::cdc
