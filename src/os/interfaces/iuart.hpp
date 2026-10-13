// iuart.hpp
#pragma once
#include "iinterface.hpp"

namespace embed {

/// Интерфейс UART.
///
/// Config объявлен в ПУБЛИЧНОЙ секции: прежде он был protected, и
/// configure() нельзя было вызвать снаружи, хотя сам метод публичный.
template<typename Impl>
class IUart : public IHwInterface<Impl>
{
public:
    enum class Parity { None, Odd, Even };

    enum class StopBits { One, OneAndHalf, Two };

    struct Config
    {
        unsigned baudRate = 115200u;
        unsigned dataBits = 8u;
        Parity   parity   = Parity::None;
        StopBits stopBits = StopBits::One;
    };

    using AsyncMode = typename IHwInterface<Impl>::AsyncMode;

    Status configure(const Config& config) noexcept {
        return static_cast<Impl*>(this)->configure(config);
    }
};

}
