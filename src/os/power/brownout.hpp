// os/power/brownout.hpp
// ---------------------------------------------------------------------------
// PVD / brown-out: предупреждение о просадке питания ДО того, как ядро
// потеряет работоспособность.
//
// На STM32 PVD — компаратор питания с выбираемым порогом. Он не спасает от
// мгновенного отключения, но даёт несколько сотен микросекунд на:
//   * завершение записи во Flash (не начинать новую),
//   * сброс незавершённой OTA-сессии,
//   * остановку мотора/нагрузки,
//   * запись аварийного маркера.
// Колбэк вызывается из ISR: он обязан быть коротким и не блокироваться.
// ---------------------------------------------------------------------------
#pragma once

#include "../common.hpp"

namespace embed::power {

class Brownout {
public:
    using Callback = void (*)(void* ctx) noexcept;

    static Brownout& instance() noexcept;

    /// Включить PVD. threshold_mv округляется к ближайшему аппаратному порогу.
    /// 0 — использовать порог по умолчанию (~2.9 В).
    Status init(uint32_t threshold_mv, Callback cb = nullptr, void* ctx = nullptr) noexcept;
    void deinit() noexcept;

    /// true — питание ниже выбранного порога ПРЯМО СЕЙЧАС.
    bool below() const noexcept;
    bool enabled() const noexcept { return enabled_; }
    uint32_t threshold_mv() const noexcept { return threshold_mv_; }

    /// Вызывается из ISR; публично только для обработчика.
    void isr() noexcept;

private:
    Brownout() noexcept = default;
    bool enabled_ = false;
    uint32_t threshold_mv_ = 0u;
    Callback cb_ = nullptr;
    void* ctx_ = nullptr;
};

} // namespace embed::power
