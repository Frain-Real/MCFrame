// platforms/stm32f4g4/usb_otg_fs.hpp
#pragma once
#include "os/usb/usb.hpp"

#if defined(MCFRAME_SOC_STM32F4)

namespace embed::stm32f4g4::usb {

/// Начальный usb::Hal для STM32F4 OTG FS (device mode, polled).
///
/// Это НЕ полный стек: OTG FS требует обработки setup/status/endpoint
/// событий. HAL даёт endpoint-примитивы и poll(); приложение связывает его с
/// usb::Device через setup_callback. Для G4 (USB FS, не OTG) нужен отдельный
/// адаптер — классы usb::cdc/dfu/msc от него не зависят.
class OtgFsHal final : public embed::usb::Hal {
public:
    using SetupCallback = void (*)(void* ctx, const embed::usb::Setup& s) noexcept;
    using OutCallback = void (*)(void* ctx, uint8_t ep,
                                 const uint8_t* data, Size len) noexcept;

    Status init(SetupCallback on_setup, OutCallback on_out, void* ctx) noexcept;
    void poll() noexcept;

    Status ep0_write(const void* data, Size len) noexcept override;
    Status ep0_read(void* data, Size cap, Size& got) noexcept override;
    Status ep_write(uint8_t ep, const void* data, Size len) noexcept override;
    Status ep_read(uint8_t ep, void* data, Size cap, Size& got) noexcept override;
    void stall_ep0() noexcept override;

    bool initialized() const noexcept { return inited_; }

private:
    void handle_reset() noexcept;
    void handle_rx() noexcept;

    SetupCallback setup_cb_ = nullptr;
    OutCallback   out_cb_ = nullptr;
    void*         ctx_ = nullptr;
    bool          inited_ = false;
    uint8_t       ctrl_out_[64]{};
    Size          ctrl_len_ = 0u;
    Size          ctrl_pos_ = 0u;
};

} // namespace embed::stm32f4g4::usb

#endif // MCFRAME_SOC_STM32F4
