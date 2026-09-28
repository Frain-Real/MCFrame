#pragma once 
#include <cstdint>

namespace embed {
    enum class Status : int
    {
        Ok = 0,
        InvalidArg,
        NoMemory,
        Busy,
        Timeout,
        NotFound,
        AlreadyExists,
        PermissionDenied,
        NotSupported,
        HardwareError,
        Unavailable,
        Fault,
        Aborted
    };

    using Size          = unsigned int;
    using Address       = uintptr_t;
    using Handle        = unsigned int;
    using CoreId        = unsigned char;
    using ThreadId      = unsigned short;
    using ProcessId     = unsigned short;
    using IrqNumber     = unsigned short;
    using Priority      = unsigned char;
    using Milliseconds  = unsigned int;
    using Microseconds  = unsigned int;

    constexpr Milliseconds WaitForever = 0xFFFFFFFFu;

    using Callback_h = void (*)(void* arg);
    using task_func = void (*)(void* ctx);
    
    enum class Access : unsigned char
    {
        None = 0,
        Read,
        Write,
        ReadWrite,
        Execute
    };
}