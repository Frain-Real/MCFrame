// os/time/rtc.cpp
#include "rtc.hpp"

#include "../config/time_config.hpp"
#include "../log/log.hpp"

namespace embed::rtc {

void WallClock::attach(Backend* backend) noexcept {
    backend_ = backend;
}

Status WallClock::sync_from_rtc() noexcept {
    if (backend_ == nullptr) return Status::NotSupported;

    uint64_t epoch = 0u;
    const Status st = backend_->read(epoch);
    if (st != Status::Ok) return st;
    if (epoch == 0u) return Status::TimeNotSynced;

    return config::OsTimer::SyncTime(epoch, 0u);
}

Status WallClock::init() noexcept {
    if (backend_ == nullptr) return Status::NotSupported;

    const Status st = sync_from_rtc();
    if (st == Status::Ok) return Status::Ok;
    if (st != Status::TimeNotSynced && st != Status::NotFound) return st;

    // RTC пуст: если ОС уже знает время (например, его только что получили
    // по сети), сохраняем его, чтобы оно пережило сброс.
    if (config::OsTimer::TimeValid()) {
        return persist();
    }
    return st;
}

Status WallClock::persist() noexcept {
    if (backend_ == nullptr) return Status::NotSupported;

    const uint64_t epoch = config::OsTimer::NowEpochSeconds();
    if (epoch == 0u) return Status::TimeNotSynced;
    return backend_->write(epoch);
}

Status WallClock::set_epoch(uint64_t epoch_sec) noexcept {
    if (epoch_sec == 0u) return Status::InvalidArg;
    const Status st = config::OsTimer::SyncTime(epoch_sec, 0u);
    if (st != Status::Ok) return st;
    // Ошибка записи RTC не отменяет уже установленное системное время:
    // приложение должно знать, что время есть, но после сброса его не будет.
    const Status w = persist();
    if (w != Status::Ok) {
        log::write_u32(log::Level::Warn, "RTC", "persist_failed",
                       static_cast<uint32_t>(w));
    }
    return Status::Ok;
}

uint64_t WallClock::epoch() noexcept {
    return config::OsTimer::NowEpochSeconds();
}

DateTime WallClock::now() noexcept {
    return config::OsTimer::NowDateTime();
}

} // namespace embed::rtc
