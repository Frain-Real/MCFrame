// ii2c.hpp
#pragma once

#include "iinterface.hpp"

namespace embed {

/// Интерфейс I2C.
template<typename Impl>
class II2c : public IHwInterface<Impl>
{
public:
    enum class Speed { Standard100k, Fast400k, FastPlus1M };
    enum class MasterMode { Master, Slave };

    struct Config
    {
        Speed      speed = Speed::Standard100k;
        bool       enableClockStretching = true;
        MasterMode role = MasterMode::Master;
    };

    using AsyncMode = typename IHwInterface<Impl>::AsyncMode;

    Status configure(const Config& config) noexcept {
        return static_cast<Impl*>(this)->configure(config);
    }

    /// Сканирование шины. found[addr] == true, если устройство ответило.
    Status scan(bool (&found)[128]) noexcept {
        return static_cast<Impl*>(this)->scan(found);
    }

    Status recover() noexcept {
        return static_cast<Impl*>(this)->recover();
    }
};

}
