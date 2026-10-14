// async_strategy.hpp
#pragma once
// ---------------------------------------------------------------------------
// СТРАТЕГИЯ ВЫПОЛНЕНИЯ ОБМЕНА — общий тип для портов и интерфейсов.
//
// Вынесено в отдельный заголовок, потому что им пользуются три слоя:
//   * async_port.hpp  — выбирает, опрашивать ли интерфейс в цикле ожидания;
//   * iuart.hpp/ii2c.hpp — вооружают completion опросом (armPoll) в линейных
//     методах, которые ждут завершения напрямую, а не через AsyncPort;
//   * драйверы (mock и stm32) — объявляют kStrategy либо strategy().
// Иначе получился бы цикл включений: iuart.hpp <-> async_port.hpp.
// ---------------------------------------------------------------------------
#include <cstdint>
#include <type_traits>

namespace embed::async {

/// Стратегия выполнения передачи.
/// Выбирается конфигурацией интерфейса, а не прикладным кодом.
enum class Strategy : uint8_t {
    /// Опрос с уступкой планировщику. Годится всегда, но занимает CPU.
    Polling = 0,
    /// Прерывания: HAL сам сообщает о завершении.
    Async,
    /// DMA: ядро не участвует в пересылке, только в ожидании.
    Dma,
};

/// Стратегия драйвера интерфейса.
///
/// Приоритет — за ФУНКЦИЕЙ strategy(): у реальных драйверов (STM32) стратегия
/// выбирается в Config и меняется на лету (usePolling/useInterrupts/useDma),
/// поэтому одной constexpr-константы уже недостаточно. Драйверы с
/// фиксированным режимом (mock, драйверы приложений по прежнему образцу)
/// могут объявить только kStrategy — она и будет использована.
template <typename Driver, typename = void>
struct DriverStrategy {
    static Strategy get() noexcept { return Driver::kStrategy; }
};

template <typename Driver>
struct DriverStrategy<Driver, std::void_t<decltype(Driver::strategy())>> {
    static Strategy get() noexcept { return Driver::strategy(); }
};

} // namespace embed::async
