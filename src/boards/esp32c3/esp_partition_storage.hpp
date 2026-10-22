// boards/esp32c3/esp_partition_storage.hpp
// ESP-IDF partition как ota::Storage (для MCFrame OTA/MOLP на C3).
#pragma once
#include "os/ota/ota_storage.hpp"
#include "esp_partition.h"

namespace embed::esp32c3 {

class EspPartitionStorage final : public ota::Storage {
public:
    EspPartitionStorage() noexcept = default;
    explicit EspPartitionStorage(const esp_partition_t* part) noexcept : part_(part) {}
    bool bind(const esp_partition_t* part) noexcept { part_ = part; return part_ != nullptr; }
    bool bind_label(const char* label) noexcept;
    bool bind_ota(uint8_t index) noexcept;

    uint32_t capacity() const noexcept override { return part_ != nullptr ? part_->size : 0u; }
    uint32_t page_size() const noexcept override { return 4096u; }
    uint32_t write_granularity() const noexcept override { return 1u; }
    const char* name() const noexcept override { return "esp_partition"; }
    Status erase(uint32_t offset, uint32_t len) noexcept override;
    Status write(uint32_t offset, const void* data, uint32_t len) noexcept override;
    Status read(uint32_t offset, void* data, uint32_t len) noexcept override;
    const esp_partition_t* partition() const noexcept { return part_; }
private:
    const esp_partition_t* part_ = nullptr;
};

} // namespace embed::esp32c3
