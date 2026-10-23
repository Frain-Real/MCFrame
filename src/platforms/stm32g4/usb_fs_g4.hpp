// platforms/stm32g4/usb_fs_g4.hpp
#pragma once
#include "os/usb/usb.hpp"

#if defined(MCFRAME_SOC_STM32G4)

namespace embed::stm32g4::usb {

/// USB 2.0 FS device (не OTG) для STM32G4: EP0 enumeration + EP1 bulk.
/// Регистры STM32 USB-FS (PMA), опрос из задачи/прерывания приложения.
class UsbFsG4Hal final : public embed::usb::Hal {
public:
    using SetupCallback = void (*)(void* ctx, const embed::usb::Setup& s,
                                   const uint8_t* out, Size out_len) noexcept;
    using OutCallback = void (*)(void* ctx, uint8_t ep,
                                 const uint8_t* data, Size len) noexcept;

    Status init(SetupCallback on_setup, OutCallback on_out, void* ctx) noexcept;
    void poll() noexcept;

    Status ep0_write(const void* data, Size len) noexcept override;
    Status ep0_read(void* data, Size cap, Size& got) noexcept override;
    Status ep_write(uint8_t ep, const void* data, Size len) noexcept override;
    Status ep_read(uint8_t ep, void* data, Size cap, Size& got) noexcept override;
    void stall_ep0() noexcept override;
    Status set_address(uint8_t address) noexcept override;
    Status suspend() noexcept override;
    Status resume() noexcept override;

private:
    void handle_reset() noexcept;
    void handle_ep0() noexcept;
    void handle_ep1() noexcept;
    void send_ep0_next() noexcept;

    SetupCallback setup_cb_ = nullptr;
    OutCallback out_cb_ = nullptr;
    void* ctx_ = nullptr;
    bool inited_ = false;
    uint8_t ctrl_out_[4096]{};
    Size ctrl_out_len_ = 0u;
    Size ctrl_pos_ = 0u;
    const uint8_t* ctrl_in_ = nullptr;
    Size ctrl_in_len_ = 0u;
    Size ctrl_in_pos_ = 0u;
    bool ctrl_in_active_ = false;
    embed::usb::Setup pending_setup_{};
    bool expect_out_ = false;
    uint8_t pending_address_ = 0u;
    bool address_pending_ = false;
    uint8_t bulk_out_[512]{};
    Size bulk_out_len_ = 0u;
    Size bulk_out_pos_ = 0u;
};

} // namespace embed::stm32g4::usb

#endif
