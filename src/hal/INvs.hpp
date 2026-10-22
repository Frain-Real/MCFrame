// hal/INvs.hpp
// ---------------------------------------------------------------------------
// Энергонезависимое хранилище ключ→значение. На STM32 — своя NVS, на C3 —
// nvs_flash; приложение видит только этот интерфейс.
// ---------------------------------------------------------------------------
#pragma once

#include "os/common.hpp"

namespace embed::hal {

class INvs {
public:
    virtual ~INvs() = default;

    virtual Status init() noexcept = 0;
    virtual Status read(const char* key, void* buffer, Size capacity,
                        Size& len) noexcept = 0;
    virtual Status write(const char* key, const void* data, Size len) noexcept = 0;
    virtual Status erase(const char* key) noexcept = 0;
};

} // namespace embed::hal
