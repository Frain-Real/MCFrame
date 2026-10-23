// platforms/stm32f4g4/rtc_m4.hpp
#pragma once
#include "os/time/rtc.hpp"

namespace embed::stm32f4g4::rtc {

/// RTC F4/G4 (RTC v2/v3): календарь в backup-домене. Секунды Unix
/// восстанавливаются из BCD TR/DR; BKP0R — признак достоверности.
class Backend final : public embed::rtc::Backend {
public:
    Status read(uint64_t& epoch_sec) noexcept override;
    Status write(uint64_t epoch_sec) noexcept override;
    const char* name() const noexcept override { return "stm32m4-rtc"; }

    /// Запустить LSE (при отказе — LSI) и RTC. Идемпотентно.
    Status init() noexcept;

private:
    bool clocked() const noexcept;
    Status set_datetime(uint64_t epoch_sec) noexcept;
};

Backend& backend() noexcept;

} // namespace embed::stm32f4g4::rtc
