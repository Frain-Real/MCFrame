#include "boards/esp32c3/esp_partition_storage.hpp"

namespace embed::esp32c3 {

bool EspPartitionStorage::bind_label(const char* label) noexcept {
    part_ = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
    return part_ != nullptr;
}
bool EspPartitionStorage::bind_ota(uint8_t index) noexcept {
    const esp_partition_subtype_t st = (index == 0u) ? ESP_PARTITION_SUBTYPE_APP_OTA_0
                                                     : ESP_PARTITION_SUBTYPE_APP_OTA_1;
    part_ = esp_partition_find_first(ESP_PARTITION_TYPE_APP, st, nullptr);
    return part_ != nullptr;
}
Status EspPartitionStorage::erase(uint32_t offset, uint32_t len) noexcept {
    if (part_ == nullptr || (offset % page_size()) != 0u || (len % page_size()) != 0u) {
        return Status::InvalidArg;
    }
    return esp_partition_erase_range(part_, offset, len) == ESP_OK
        ? Status::Ok : Status::HardwareError;
}
Status EspPartitionStorage::write(uint32_t offset, const void* data, uint32_t len) noexcept {
    if (part_ == nullptr || (data == nullptr && len != 0u)) return Status::InvalidArg;
    return esp_partition_write(part_, offset, data, len) == ESP_OK
        ? Status::Ok : Status::HardwareError;
}
Status EspPartitionStorage::read(uint32_t offset, void* data, uint32_t len) noexcept {
    if (part_ == nullptr || (data == nullptr && len != 0u)) return Status::InvalidArg;
    return esp_partition_read(part_, offset, data, len) == ESP_OK
        ? Status::Ok : Status::HardwareError;
}

} // namespace embed::esp32c3
