// os/time/duration.hpp
// ---------------------------------------------------------------------------
// ТИПОБЕЗОПАСНОЕ ВРЕМЯ.
//
// Зачем это появилось: `scheduler::sleep_for(uint32_t ticks)` при тике 10 мс
// означал, что sleep_for(100) — это секунда, а не 100 мс. Ошибка не ловится
// компилятором и проявляется как «задача почему-то тормозит» или «таймаут
// сработал в 10 раз позже». Duration хранит МИКРОСЕКУНДЫ и умеет явно
// переводить их в миллисекунды/тики с округлением ВВЕРХ (спать меньше
// запрошенного нельзя).
//
// Duration — значение, а не «сколько-то тиков»: период тика задаётся при
// переводе, потому что он настраивается в рантайме (OsTimer::TickMs()).
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace embed::chrono {

/// Интервал времени. Внутри — микросекунды: это покрывает и 1 мс, и
/// несколько суток без потери точности при сложении.
class Duration {
public:
    constexpr Duration() noexcept : us_(0u) {}

    static constexpr Duration from_us(uint64_t us) noexcept {
        Duration d;
        d.us_ = us;
        return d;
    }
    static constexpr Duration from_ms(uint64_t ms) noexcept {
        Duration d;
        d.us_ = ms * 1000u;
        return d;
    }
    static constexpr Duration from_seconds(uint64_t s) noexcept {
        Duration d;
        d.us_ = s * 1000000u;
        return d;
    }
    /// Интервал в тиках при заданном периоде тика.
    static constexpr Duration from_ticks(uint64_t ticks, uint32_t tick_ms) noexcept {
        Duration d;
        d.us_ = ticks * tick_ms * 1000u;
        return d;
    }

    constexpr uint64_t us() const noexcept { return us_; }
    constexpr uint64_t ms() const noexcept { return us_ / 1000u; }
    /// Миллисекунды с округлением вверх (для таймаутов).
    constexpr uint64_t ms_ceil() const noexcept {
        return (us_ + 999u) / 1000u;
    }

    /// Сколько тиков нужно, чтобы проспать НЕ МЕНЬШЕ этого интервала.
    /// Ноль остаётся нулём (нулевой сон — это yield, а не один тик).
    constexpr uint32_t ticks_ceil(uint32_t tick_ms) const noexcept {
        if (us_ == 0u) return 0u;
        const uint64_t tick_us = uint64_t(tick_ms == 0u ? 1u : tick_ms) * 1000u;
        const uint64_t ticks = (us_ + tick_us - 1u) / tick_us;
        return ticks > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(ticks);
    }

    constexpr bool is_zero() const noexcept { return us_ == 0u; }
    constexpr bool is_infinite() const noexcept { return us_ == UINT64_MAX; }

    constexpr Duration operator+(Duration other) const noexcept {
        return from_us(us_ + other.us_);
    }
    constexpr Duration operator-(Duration other) const noexcept {
        return from_us(us_ > other.us_ ? us_ - other.us_ : 0u);
    }
    constexpr Duration& operator+=(Duration other) noexcept {
        us_ += other.us_;
        return *this;
    }
    constexpr Duration operator*(uint64_t factor) const noexcept {
        return from_us(us_ * factor);
    }
    constexpr bool operator==(Duration other) const noexcept { return us_ == other.us_; }
    constexpr bool operator!=(Duration other) const noexcept { return us_ != other.us_; }
    constexpr bool operator<(Duration other) const noexcept { return us_ < other.us_; }
    constexpr bool operator<=(Duration other) const noexcept { return us_ <= other.us_; }
    constexpr bool operator>(Duration other) const noexcept { return us_ > other.us_; }
    constexpr bool operator>=(Duration other) const noexcept { return us_ >= other.us_; }

private:
    uint64_t us_ = 0u;
};

/// Абсолютная точка на монотонной шкале миллисекунд (OsTimer::NowMs()).
class TimePoint {
public:
    constexpr TimePoint() noexcept : ms_(0u) {}
    static constexpr TimePoint from_ms(uint64_t ms) noexcept {
        TimePoint t;
        t.ms_ = ms;
        return t;
    }

    constexpr uint64_t ms() const noexcept { return ms_; }
    constexpr bool is_zero() const noexcept { return ms_ == 0u; }

    constexpr TimePoint operator+(Duration d) const noexcept {
        return from_ms(ms_ + d.ms_ceil());
    }
    constexpr Duration operator-(TimePoint other) const noexcept {
        return Duration::from_ms(ms_ > other.ms_ ? ms_ - other.ms_ : 0u);
    }
    constexpr bool operator==(TimePoint other) const noexcept { return ms_ == other.ms_; }
    constexpr bool operator!=(TimePoint other) const noexcept { return ms_ != other.ms_; }
    constexpr bool operator<(TimePoint other) const noexcept { return ms_ < other.ms_; }
    constexpr bool operator<=(TimePoint other) const noexcept { return ms_ <= other.ms_; }
    constexpr bool operator>(TimePoint other) const noexcept { return ms_ > other.ms_; }
    constexpr bool operator>=(TimePoint other) const noexcept { return ms_ >= other.ms_; }

private:
    uint64_t ms_ = 0u;
};

constexpr Duration microseconds(uint64_t us) noexcept { return Duration::from_us(us); }
constexpr Duration milliseconds(uint64_t ms) noexcept { return Duration::from_ms(ms); }
constexpr Duration seconds(uint64_t s) noexcept { return Duration::from_seconds(s); }

} // namespace embed::chrono
