#pragma once
#include "iinterface.hpp"
namespace embed {
template<typename Impl>
class IUart : public IHwInterface<Impl>
{
public:
    enum class Parity
    {
        None,
        Odd,
        Even
    };

    enum class StopBits
    {
        One,
        OneAndHalf,
        Two
    };
protected:
    struct Config
    {
        unsigned baudRate;
        unsigned dataBits;
        Parity parity;
        StopBits stopBits;
    };
public:
    Status configure(const Config& config){ return static_cast<Impl*>(this)->configure(config); };

};
}