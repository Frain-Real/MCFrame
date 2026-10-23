// os/time/rtc.hpp
// ---------------------------------------------------------------------------
// RTC wall clock: календарное время, переживающее сброс и глубокий сон.
//
// Монотонное время ОС (OsTimer::NowMs) после сброса начинается с нуля. RTC —
// единственный источник, который продолжает считать в STOP/Standby и хранит
// epoch. Политика здесь: взять время из backend при старте, отдать его в
// OsTimer::SyncTime, а после SNTP/ручной установки — записать обратно.
//
// Backend — платформенный/бортовой объект. На STM32F1 это аппаратный RTC в
// backup-домене; на стенде — SoftBackend. WallClock не знает регистров.
// ---------------------------------------------------------------------------
#pragma once

#include "../common.hpp"
#include "timer.hpp"
#include <cstdint>

namespace embed::rtc {

/// Абстракция энергонезависимых секунд. Возврат read():
///   Ok            — epoch заполнен;
///   TimeNotSynced — RTC ещё не инициализирован/нет достоверного времени;
///   NotSupported  — backend не умеет хранить время.
class Backend {
public:
    virtual ~Backend() = default;
    virtual Status read(uint64_t& epoch_sec) noexcept = 0;
    virtual Status write(uint64_t epoch_sec) noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

/// Программный backend для стенда и плат без RTC. Время живёт только в RAM.
class SoftBackend final : public Backend {
public:
    Status read(uint64_t& epoch_sec) noexcept override {
        if (!valid_) return Status::TimeNotSynced;
        epoch_sec = epoch_;
        return Status::Ok;
    }
    Status write(uint64_t epoch_sec) noexcept override {
        if (epoch_sec == 0u) return Status::InvalidArg;
        epoch_ = epoch_sec;
        valid_ = true;
        return Status::Ok;
    }
    const char* name() const noexcept override { return "soft"; }
    void invalidate() noexcept { valid_ = false; }
    bool valid() const noexcept { return valid_; }
private:
    uint64_t epoch_ = 0u;
    bool     valid_ = false;
};

/// Синглтон-фасад. Backend обязан жить дольше WallClock (обычно — статический).
class WallClock {
public:
    static WallClock& instance() noexcept {
        static WallClock w;
        return w;
    }

    WallClock(const WallClock&) = delete;
    WallClock& operator=(const WallClock&) = delete;

    /// Привязать backend. nullptr отвязывает.
    void attach(Backend* backend) noexcept;

    bool attached() const noexcept { return backend_ != nullptr; }

    /// Прочитать RTC и синхронизировать OsTimer. Если RTC пуст, но OsTimer
    /// уже имеет время (например, установлен приложением), записать его в RTC.
    /// Возврат: Ok | TimeNotSynced | NotSupported | InvalidArg.
    Status init() noexcept;

    /// Прочитать RTC и применить к OsTimer, не записывая обратно.
    Status sync_from_rtc() noexcept;

    /// Взять текущее время OsTimer и сохранить в RTC.
    Status persist() noexcept;

    /// Установить календарное время (epoch UTC) и сохранить в RTC.
    Status set_epoch(uint64_t epoch_sec) noexcept;

    /// Текущее epoch-время ОС. 0 — время не синхронизировано.
    uint64_t epoch() noexcept;

    /// Календарное представление. При отсутствии времени — 1970-01-01.
    DateTime now() noexcept;

    /// Смещение/точность RTC в ppm (0 — неизвестно).
    int16_t ppm() const noexcept { return ppm_; }
    void set_ppm(int16_t ppm) noexcept { ppm_ = ppm; }

private:
    WallClock() noexcept = default;
    Backend* backend_ = nullptr;
    int16_t  ppm_ = 0;
};

} // namespace embed::rtc
