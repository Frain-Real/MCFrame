// platforms/stm32f4g4/usb_otg_fs.hpp
#pragma once
#include "os/usb/usb.hpp"

#if defined(MCFRAME_SOC_STM32F4)

namespace embed::stm32f4g4::usb {

/// Начальный usb::Hal для STM32F4 OTG FS (device mode, polled).
///
/// HAL доводит control-stage до enumeration: SETUP/OUT-data/status, EP0
/// chunking по 64 байта, SET_ADDRESS после status. Приложение связывает его с
/// usb::Device через usb::Port. Bulk EP1 остаётся примитивом; полный
/// hot-plug/suspend lifecycle нужно проверять на HIL. Для G4 (USB FS, не OTG)
/// нужен отдельный адаптер — классы usb::cdc/dfu/msc от него не зависят.
class OtgFsHal final : public embed::usb::Hal {
public:
    /// setup_cb получает OUT-данные (для class request); для IN/без данных
    /// out == nullptr, out_len == 0.
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

    bool initialized() const noexcept { return inited_; }

private:
    void handle_reset() noexcept;
    void handle_rx() noexcept;
    void handle_ep0_in_done() noexcept;
    void send_ep0_next() noexcept;

    SetupCallback setup_cb_ = nullptr;
    OutCallback   out_cb_ = nullptr;
    void*         ctx_ = nullptr;
    bool          inited_ = false;
    uint8_t       ctrl_out_[4096]{};
    Size          ctrl_out_len_ = 0u;
    Size          ctrl_pos_ = 0u;
    embed::usb::Setup pending_setup_{};
    const uint8_t* ctrl_in_data_ = nullptr;
    Size          ctrl_in_len_ = 0u;
    Size          ctrl_in_pos_ = 0u;
    bool          ctrl_in_active_ = false;
    bool          expect_out_ = false;
    uint8_t       pending_address_ = 0u;
    bool          address_pending_ = false;
};

} // namespace embed::stm32f4g4::usb

#endif // MCFRAME_SOC_STM32F4
