// hal/IUart.hpp
// ---------------------------------------------------------------------------
// UART как интерфейс. writeLine/readLine — удобные обёртки над write/read.
// ---------------------------------------------------------------------------
#pragma once

#include "os/common.hpp"

namespace embed::hal {

enum class UartParity : uint8_t { None, Odd, Even };
enum class UartStopBits : uint8_t { One, Two };

struct UartConfig {
    uint32_t     baud_rate  = 115200u;
    uint8_t      data_bits  = 8u;
    UartParity   parity     = UartParity::None;
    UartStopBits stop_bits  = UartStopBits::One;
    bool         flow_control = false;
};

class IUart {
public:
    virtual ~IUart() = default;

    virtual Status configure(const UartConfig& cfg) noexcept = 0;
    virtual Status open() noexcept = 0;
    virtual Status close() noexcept = 0;

    virtual Status read(void* buffer, Size capacity, Size& bytes_read,
                        Timeout timeout) noexcept = 0;
    virtual Status write(const void* data, Size len, Size& written,
                         Timeout timeout) noexcept = 0;

    Status writeLine(const char* text, Timeout timeout = 1000u) noexcept {
        if (text == nullptr) return Status::InvalidArg;
        Size n = 0u;
        while (text[n] != '\0') ++n;
        Size written = 0u;
        Status st = write(text, n, written, timeout);
        if (st != Status::Ok) return st;
        return write("\r\n", 2u, written, timeout);
    }
};

} // namespace embed::hal
