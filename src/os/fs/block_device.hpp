// os/fs/block_device.hpp
#pragma once

#include "../common.hpp"

namespace embed::fs {

/// Блочное устройство (SD/MMC, NOR, RAM-диск) для ФС.
class BlockDevice {
public:
    virtual ~BlockDevice() = default;
    virtual uint32_t block_count() const noexcept = 0;
    virtual uint32_t block_size() const noexcept = 0;
    virtual Status read(uint32_t lba, void* data, uint32_t count) noexcept = 0;
    virtual Status write(uint32_t lba, const void* data, uint32_t count) noexcept = 0;
    /// Стереть блоки. По умолчанию не поддержано: SD может реализовать
    /// TRIM/erase, NOR — реальный секторный erase. littlefs без erase
    /// деградирует до перезаписи 0xFF (безопасно только для SD/RAM).
    virtual Status erase(uint32_t, uint32_t) noexcept { return Status::NotSupported; }
    virtual Status sync() noexcept { return Status::Ok; }
    /// Проверка присутствия носителя (hotplug). По умолчанию — всегда есть.
    virtual bool present() const noexcept { return true; }
};

} // namespace embed::fs
