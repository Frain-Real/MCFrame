// os/usb/dfu_storage.hpp
#pragma once

#include "dfu.hpp"
#include "../ota/ota_storage.hpp"

namespace embed::usb::dfu {

/// DFU Target поверх ota::Storage: erase при первом DNLOAD, запись по
/// смещению, чтение для UPLOAD, finish — барьер после последнего блока.
class StorageTarget final : public Target {
public:
    StorageTarget(ota::Storage& storage, uint32_t base, uint32_t capacity) noexcept
        : storage_(&storage), base_(base), capacity_(capacity) {}

    Size capacity() const noexcept override { return capacity_; }

    Status begin() noexcept override {
        if (storage_ == nullptr || capacity_ == 0u) return Status::InvalidArg;
        const Status st = storage_->erase(base_, capacity_);
        offset_ = 0u;
        return st;
    }

    Status write(uint32_t offset, const void* data, Size len) noexcept override {
        if (storage_ == nullptr || data == nullptr) return Status::InvalidArg;
        if (offset > capacity_ || len > capacity_ - offset) return Status::NoMemory;
        return storage_->write(base_ + offset, data, len);
    }

    Status read(uint32_t offset, void* data, Size len) noexcept override {
        if (storage_ == nullptr || data == nullptr) return Status::InvalidArg;
        if (offset > capacity_ || len > capacity_ - offset) return Status::InvalidArg;
        return storage_->read(base_ + offset, data, len);
    }

    Status finish() noexcept override { return Status::Ok; }

private:
    ota::Storage* storage_ = nullptr;
    uint32_t base_ = 0u;
    uint32_t capacity_ = 0u;
    uint32_t offset_ = 0u;
};

} // namespace embed::usb::dfu
