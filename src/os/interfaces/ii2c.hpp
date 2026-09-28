#pragma once 

#include "iinterface.hpp"
namespace embed {
template<typename Impl>
class II2c : public IHwInterface<Impl>
    {
    public:
        enum class Speed
        {
            Standard100k,
            Fast400k,
            FastPlus1M
        };
        enum class MasterMode {
            Master,
            Slave
        };
    protected:
        struct Config
        {
            Speed speed;
            bool enableClockStretching;
            MasterMode role;
        };
    public:
        Status configure(const Config& config){ return static_cast<Impl*>(this)->configure(config); };
        
        Status scan(bool found[128]) { return static_cast<Impl*>(this)->scan(found); };
        Status recover() { return static_cast<Impl*>(this)->recover(); };

};
}