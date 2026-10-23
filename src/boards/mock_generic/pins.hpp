// boards/mock_generic/pins.hpp
// ---------------------------------------------------------------------------
// Карта mock-платы: физических ног нет, но API карты должен быть доступен
// приложению и тестам без #ifdef. Один виртуальный LED — этого достаточно,
// чтобы board::pins() имел тот же тип/смысл, что и на железе.
// ---------------------------------------------------------------------------
#pragma once

#include "boards/pin_map.hpp"

namespace board {

inline constexpr PinMap<1> kPins = PinMap<1>{{
    {Pin{Port::A, 0u}, Signal::Led, 0u, 0u, 0u},
}};

static_assert(kPins.valid(), "mock_generic: конфликт в карте пинов");

} // namespace board
