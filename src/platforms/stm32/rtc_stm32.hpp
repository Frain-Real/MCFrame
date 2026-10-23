// platforms/stm32/rtc_stm32.hpp
#pragma once
#include "os/time/rtc.hpp"

namespace embed::stm32::rtc {

/// Аппаратный RTC STM32F1 в backup-домене. Счётчик — секунды Unix epoch,
/// признак достоверности — BKP->DR1. Пресейлер и тактирование настраивает
/// power::init() (LSE/LSI); до этого backend отвечает TimeNotSynced.
class Backend final : public embed::rtc::Backend {
public:
    Status read(uint64_t& epoch_sec) noexcept override;
    Status write(uint64_t epoch_sec) noexcept override;
    const char* name() const noexcept override { return "stm32f1-rtc"; }
};

Backend& backend() noexcept;

} // namespace embed::stm32::rtc
