// board.hpp
// ---------------------------------------------------------------------------
// Единственная точка, где приложение узнаёт про пины и периферию платы.
// Приложение включает "board.hpp"; конкретный BSP выбирается по SoC.
// ---------------------------------------------------------------------------
#pragma once

#if defined(MCFRAME_SOC_STM32F1)
  #include "boards/f103_generic/board.hpp"
#elif defined(MCFRAME_SOC_HOST)
  #include "boards/mock_generic/board.hpp"
#elif defined(MCFRAME_SOC_STM32F4)
  #include "boards/f407_generic/board.hpp"
#elif defined(MCFRAME_SOC_STM32G4)
  #include "boards/g431_generic/board.hpp"
#elif defined(MCFRAME_SOC_ESP32C3)
  #include "boards/esp32c3/board.hpp"
#else
  #error "MCFrame: board.hpp не знает MCFRAME_SOC (Phase 2/4)"
#endif
