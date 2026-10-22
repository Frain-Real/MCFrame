// os/nvs/nvs.hpp
// ---------------------------------------------------------------------------
// Энергонезависимое key-value хранилище поверх ota::Storage.
//
// Два банка, журнальная запись, CRC32 каждой записи и заголовка банка.
// Запись всегда добавляется в конец активного банка; при заполнении банк
// компактируется в другой (erase + copies live values). Заголовок банка
// пишется последним, поэтому сбой питания при компакции оставляет старый
// банк целым.
//
// Ограничения (осознанно): до 16 ключей, значение до 64 байт, ключ до 15
// символов. Этого достаточно для WiFi-credentials, калибровки и настроек.
// ---------------------------------------------------------------------------
#pragma once

#include "../ota/ota_storage.hpp"
#include "../../hal/INvs.hpp"

namespace embed::nvs {

inline constexpr uint32_t kMaxKeys = 16u;
inline constexpr uint32_t kMaxKeyLen = 15u;
inline constexpr uint32_t kMaxValueLen = 64u;
inline constexpr uint32_t kBankHeaderSize = 32u;
inline constexpr uint32_t kRecordHeaderSize = 32u;
inline constexpr uint32_t kRecordMagic = 0x4E565331u; // "NVS1"

struct Config {
    ota::Storage* storage = nullptr;
    uint32_t bank_size = 4096u;   ///< размер одного банка, кратен page_size
};

class Store {
public:
    Store() noexcept = default;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Status init(const Config& cfg) noexcept;
    void deinit() noexcept;

    Status get(const char* key, void* buffer, Size capacity, Size& len) noexcept;
    Status set(const char* key, const void* data, Size len) noexcept;
    Status erase_key(const char* key) noexcept;
    Status format() noexcept;

    bool contains(const char* key) const noexcept;
    uint32_t key_count() const noexcept { return count_; }
    uint32_t bank_seq() const noexcept { return active_seq_; }

private:
    struct Entry {
        char key[kMaxKeyLen + 1]{};
        uint32_t offset = 0u;   // абсолютное смещение значения
        uint16_t len = 0u;
        uint16_t flags = 0u;
        uint32_t seq = 0u;      // bank_seq записи
        bool used = false;
    };

    static constexpr uint16_t kFlagDeleted = 1u << 0;

    Status load_bank(uint32_t bank, uint32_t& seq, uint32_t& end) noexcept;
    Status scan() noexcept;
    Status append(uint32_t bank, uint32_t& pos, const char* key,
                  const void* data, Size len, uint16_t flags) noexcept;
    Status compact() noexcept;
    Status write_bank_header(uint32_t bank, uint32_t seq) noexcept;
    Status find(const char* key, uint32_t& idx) const noexcept;
    Entry* find_mut(const char* key) noexcept;

    ota::Storage* storage_ = nullptr;
    uint32_t bank_size_ = 0u;
    uint32_t page_ = 0u;
    uint32_t active_bank_ = 0u;
    uint32_t active_seq_ = 0u;
    uint32_t active_pos_ = 0u;
    uint32_t count_ = 0u;
    bool needs_compact_ = false;
    Entry entries_[kMaxKeys]{};
};

// Адаптер hal::INvs для плат, где NVS хранится в InternalFlashStorage.
// (C3 использует esp_nvs, STM32 — этот класс.)
class HalNvs final : public hal::INvs {
public:
    Status init() noexcept override;
    Status read(const char* key, void* buffer, Size capacity, Size& len) noexcept override;
    Status write(const char* key, const void* data, Size len) noexcept override;
    Status erase(const char* key) noexcept override;
    void bind(Store* store) noexcept { store_ = store; }
private:
    Store* store_ = nullptr;
};

} // namespace embed::nvs
