// platforms/stm32f4g4/periph_m4_instantiate.cpp
// ---------------------------------------------------------------------------
// Явные инстанцирования драйверов F4/G4.
//
// Зачем: BSP создаёт объекты I2C/ADC, но self-test вызывает только SPI/PWM.
// Без явного инстанцирования методы configure/write/read I2C и read ADC не
// компилировались бы до первого использования приложением — и ошибка в них
// всплыла бы у пользователя, а не в CI. Кросс-сборка должна проверять ВСЕ
// драйверы, а не только те, что попали в демо.
// ---------------------------------------------------------------------------
#include "stm32_periph_m4.hpp"

template class embed::stm32::Uart<1>;
template class embed::stm32::Uart<2>;
template class embed::stm32::Uart<3>;
template class embed::stm32::Spi<1>;
template class embed::stm32::Spi<2>;
template class embed::stm32::Spi<3>;
template class embed::stm32::I2c<1>;
template class embed::stm32::I2c<2>;
template class embed::stm32::Adc<1>;
