// os/nvs/nvs.hpp
// ---------------------------------------------------------------------------
// NVS v2: энергонезависимое key-value хранилище поверх ota::Storage.
//
// Что изменилось по сравнению с первой версией (и почему):
//   * NAMESPACES — ключи разных подсистем больше не сталкиваются: wifi/ssid
//     и app/ssid — разные записи. Namespace — префикс ключа, а не отдельный
//     банк: одна компакция и один wear-leveling на всё.
//   * BLOBS — значение до MCFRAME_NVS_VALUE_MAX (по умолчанию 512 Б) вместо
//     64 Б; ключей — MCFRAME_NVS_MAX_KEYS (по умолчанию 64) вместо 16.
//   * SCHEMA VERSION — в заголовке банка; при несовпадении вызывается
//     прикладная миграция, а не «данные молча стали нечитаемыми».
//   * ENCRYPTION — необязательный Cipher (AesCtrHmac): значение шифруется
//     и аутентифицируется; ключ остаётся открытым, чтобы поиск не требовал
//     расшифровки всех записей.
//
// Формат банка (v2), little-endian:
//   0  magic "NVB2" (u32) | 4 format(u16)=2 | 6 schema(u16)
//   8  seq(u32)           | 12 flags(u32)  | 16..27 reserved
//   28 crc32(0..27)
//
// Формат записи (v2):
//   0  magic "NVR2"(u32) | 4 key_len(u8) | 5 flags(u8)
//   6  value_len(u16)    | 8 nonce(u64)   | 16 crc32(0..15 + key + value)
//   20 reserved(u32)     | 24 key[key_len] | padding to 4 | value
//
// Совместимость: банк v1 (старый формат) читается и при первом открытии
// автоматически переписывается в v2. Данные не теряются.
// ---------------------------------------------------------------------------
#pragma once

#include "../ota/ota_storage.hpp"
#include "../../hal/INvs.hpp"
#include "../../hal/IFlash.hpp"

#ifndef MCFRAME_NVS_MAX_KEYS
#define MCFRAME_NVS_MAX_KEYS 64u
#endif
#ifndef MCFRAME_NVS_VALUE_MAX
#define MCFRAME_NVS_VALUE_MAX 512u
#endif

namespace embed::nvs {

inline constexpr uint32_t kMaxKeys = MCFRAME_NVS_MAX_KEYS;
inline constexpr uint32_t kMaxKeyLen = 47u;          // namespace/key
inline constexpr uint32_t kMaxNamespaceLen = 15u;
inline constexpr uint32_t kMaxNameLen = 31u;
inline constexpr uint32_t kMaxValueLen = MCFRAME_NVS_VALUE_MAX;
inline constexpr uint32_t kBankHeaderSize = 32u;
inline constexpr uint32_t kRecordHeaderSize = 24u;
inline constexpr uint32_t kMaxCipherOverhead = 32u;
inline constexpr uint32_t kRecordMagic = 0x4E565232u;   // "NVR2"
inline constexpr uint32_t kBankMagic   = 0x4E564232u;   // "NVB2"
inline constexpr uint16_t kFormatVersion = 2u;
inline constexpr uint16_t kBankFlagEncrypted = 1u << 0;

inline constexpr uint32_t kAlignedKeyLen = (kMaxKeyLen + 3u) & ~3u;
inline constexpr uint32_t kRecordMax =
    kRecordHeaderSize + kAlignedKeyLen + kMaxValueLen + kMaxCipherOverhead + 8u;

static_assert(kMaxKeys > 0u && kMaxKeys <= 1024u, "MCFRAME_NVS_MAX_KEYS вне разумных пределов");
static_assert(kMaxValueLen > 0u && kMaxValueLen <= 4096u, "MCFRAME_NVS_VALUE_MAX вне разумных пределов");

/// Шифрование/аутентификация значения. Реализация не хранит ключ в NVS:
/// ключ передаётся приложением при создании Cipher.
///
/// seal() пишет len + overhead() байт, open() читает len байт и возвращает
/// длину открытого текста. nonce уникален для записи и хранится в заголовке.
class Cipher {
public:
    virtual ~Cipher() = default;

    virtual Size overhead() const noexcept = 0;

    virtual Status seal(uint64_t nonce, const void* aad, Size aad_len,
                        const void* plain, Size len, void* out) noexcept = 0;

    virtual Status open(uint64_t nonce, const void* aad, Size aad_len,
                        const void* cipher, Size len, void* out,
                        Size& plain_len) noexcept = 0;
};

struct Config {
    ota::Storage* storage = nullptr;
    uint32_t bank_size = 4096u;   ///< размер одного банка, кратен page_size
    /// Целевая версия схемы данных. 0 — «не проверять».
    uint16_t schema_version = 1u;
    /// Миграция со старой схемы на новую. Вызывается ДО первого чтения
    /// приложением; внутри можно пользоваться этим же Store.
    Status (*migrate)(void* ctx, uint16_t from, uint16_t to) noexcept = nullptr;
    void* migrate_ctx = nullptr;
    /// Шифрование новых значений. nullptr — без шифрования; старые
    /// незашифрованные записи при этом читаются.
    Cipher* cipher = nullptr;
};

class Store;

/// Namespace — префикс ключа. Объект лёгкий (указатель + имя), его можно
/// создавать в задаче: он не владеет памятью и не блокирует.
class Namespace {
public:
    Namespace() noexcept = default;

    Status get(const char* key, void* buffer, Size capacity, Size& len) const noexcept;
    Status set(const char* key, const void* data, Size len) const noexcept;
    Status erase(const char* key) const noexcept;
    bool contains(const char* key) const noexcept;
    const char* name() const noexcept { return name_; }
    bool valid() const noexcept { return store_ != nullptr; }

private:
    friend class Store;
    Namespace(Store* store, const char* name) noexcept;

    bool full_key(const char* key, char out[kMaxKeyLen + 1]) const noexcept;

    Store* store_ = nullptr;
    char   name_[kMaxNamespaceLen + 1]{};
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

    /// Явные имена для blob-значений: то же самое, что set/get, но
    /// подчёркивает, что длина не ограничена 64 байтами.
    Status set_blob(const char* key, const void* data, Size len) noexcept {
        return set(key, data, len);
    }
    Status get_blob(const char* key, void* buffer, Size capacity, Size& len) noexcept {
        return get(key, buffer, capacity, len);
    }

    Namespace open_namespace(const char* name) noexcept;

    bool contains(const char* key) const noexcept;
    uint32_t key_count() const noexcept { return count_; }
    uint32_t bank_seq() const noexcept { return active_seq_; }
    uint16_t schema_version() const noexcept { return schema_; }
    uint32_t max_value_len() const noexcept { return kMaxValueLen; }
    uint32_t max_keys() const noexcept { return kMaxKeys; }

private:
    struct Entry {
        uint32_t hash = 0u;
        uint32_t offset = 0u;      ///< начало записи
        uint16_t value_len = 0u;   ///< длина значения (для encrypted — с тегом)
        uint8_t  key_len = 0u;
        uint8_t  flags = 0u;
        uint8_t  format = 2u;      ///< 1 или 2 (старый/новый формат записи)
        bool     used = false;
    };

    static constexpr uint16_t kFlagDeleted = 1u << 0;
    static constexpr uint16_t kFlagEncrypted = 1u << 1;

    Status load_bank(uint32_t bank, uint16_t& format, uint16_t& schema,
                     uint32_t& seq) noexcept;
    Status scan() noexcept;
    Status append(uint32_t bank, uint32_t& pos, const char* key,
                  const void* data, Size len, uint16_t flags) noexcept;
    Status compact() noexcept;
    Status write_bank_header(uint32_t bank, uint32_t seq, uint16_t schema,
                             uint16_t format) noexcept;
    Status find(const char* key, uint32_t& idx) const noexcept;
    Entry* find_mut(const char* key) noexcept;
    Status read_key(const Entry& e, char out[kMaxKeyLen + 1]) const noexcept;
    Status read_nonce(const Entry& e, uint64_t& nonce) const noexcept;
    Status read_value(const Entry& e, void* out) const noexcept;

    static uint32_t hash_key(const char* key, Size len) noexcept;
    static uint32_t align4(uint32_t n) noexcept { return (n + 3u) & ~3u; }
    static bool valid_name(const char* name) noexcept;
    static bool valid_key_path(const char* key) noexcept;

    ota::Storage* storage_ = nullptr;
    Cipher*       cipher_ = nullptr;
    uint32_t      bank_size_ = 0u;
    uint32_t      page_ = 0u;
    uint32_t      gran_ = 1u;          ///< грануляр программирования носителя
    uint32_t      last_record_offset_ = 0u;
    uint32_t      active_bank_ = 0u;
    uint32_t      active_seq_ = 0u;
    uint32_t      active_pos_ = 0u;
    uint32_t      count_ = 0u;
    uint16_t      schema_ = 1u;
    uint16_t      active_format_ = 2u;
    bool          needs_compact_ = false;
    bool          initialized_ = false;

    Entry    entries_[kMaxKeys]{};
    uint8_t  record_[kRecordMax]{};
    uint8_t  plain_[kMaxValueLen]{};
};

/// Адаптер ota::Storage поверх hal::IFlash (внешняя SPI-Flash/FRAM/EEPROM).
///
/// Нужен там, где внутренней Flash под NVS нет: F1 slot-образ (128 КБ
/// заняты) или внешняя SPI-Flash/FRAM/EEPROM. Ёмкость задаёт приложение —
/// интерфейс IFlash её не знает.
class HalFlashStorage final : public ota::Storage {
public:
    HalFlashStorage(hal::IFlash& flash, uint32_t capacity) noexcept
        : flash_(&flash), capacity_(capacity) {}

    uint32_t capacity() const noexcept override { return capacity_; }
    uint32_t page_size() const noexcept override { return flash_->page_size(); }
    uint32_t write_granularity() const noexcept override { return 1u; }
    bool     is_volatile() const noexcept override { return flash_->is_volatile(); }
    const char* name() const noexcept override { return "hal_flash"; }

    Status erase(uint32_t offset, uint32_t len) noexcept override {
        if (offset + len < offset || offset + len > capacity_) return Status::InvalidArg;
        return flash_->erase(offset, len);
    }
    Status write(uint32_t offset, const void* data, uint32_t len) noexcept override {
        if (offset + len < offset || offset + len > capacity_) return Status::InvalidArg;
        return flash_->write(offset, data, len);
    }
    Status read(uint32_t offset, void* data, uint32_t len) noexcept override {
        if (offset + len < offset || offset + len > capacity_) return Status::InvalidArg;
        return flash_->read(offset, data, len);
    }

private:
    hal::IFlash* flash_;
    uint32_t     capacity_;
};

/// Носитель NVS на участке внутренней Flash с ЯВНО заданным размером
/// страницы. Нужен F4: там секторы 16/64/128 КБ, а NVS занимает два
/// 16-КБ сектора сразу за загрузчиком. `InternalFlashStorage` сообщает
/// page_size платформы (128 КБ) и не может стереть один 16-КБ банк.
class FlashSectorStorage final : public ota::Storage {
public:
    FlashSectorStorage(uint32_t offset, uint32_t len, uint32_t page) noexcept
        : offset_(offset), len_(len), page_(page) {}

    Status begin() noexcept override {
        const platform::FlashOps* f = platform::ops().flash;
        if (f == nullptr || f->unlock == nullptr) return Status::NotSupported;
        locked_ = false;
        return f->unlock() ? Status::Ok : Status::HardwareError;
    }
    void end() noexcept override {
        const platform::FlashOps* f = platform::ops().flash;
        if (!locked_ && f != nullptr && f->lock != nullptr) {
            f->lock();
            locked_ = true;
        }
    }

    uint32_t capacity() const noexcept override { return len_; }
    uint32_t page_size() const noexcept override { return page_; }
    uint32_t write_granularity() const noexcept override {
        const platform::FlashOps* f = platform::ops().flash;
        return (f != nullptr && f->write_granularity != nullptr)
               ? f->write_granularity() : 1u;
    }
    const char* name() const noexcept override { return "flash_sector"; }

    Status erase(uint32_t offset, uint32_t len) noexcept override {
        const platform::FlashOps* f = platform::ops().flash;
        if (f == nullptr || f->erase == nullptr || f->base == nullptr) {
            return Status::NotSupported;
        }
        if (page_ == 0u || (offset % page_) != 0u || (len % page_) != 0u ||
            offset + len < offset || offset + len > len_) {
            return Status::InvalidArg;
        }
        return f->erase(f->base() + offset_, len) ? Status::Ok
                                                   : Status::HardwareError;
    }
    Status write(uint32_t offset, const void* data, uint32_t len) noexcept override {
        const platform::FlashOps* f = platform::ops().flash;
        if (f == nullptr || f->program == nullptr || f->base == nullptr) {
            return Status::NotSupported;
        }
        if (offset + len < offset || offset + len > len_) return Status::InvalidArg;
        return f->program(f->base() + offset_ + offset, data, len) ? Status::Ok
                                                                   : Status::HardwareError;
    }
    Status read(uint32_t offset, void* data, uint32_t len) noexcept override {
        const platform::FlashOps* f = platform::ops().flash;
        if (f == nullptr || f->read == nullptr || f->base == nullptr) {
            return Status::NotSupported;
        }
        if (offset + len < offset || offset + len > len_) return Status::InvalidArg;
        return f->read(f->base() + offset_ + offset, data, len) ? Status::Ok
                                                                : Status::HardwareError;
    }

private:
    uint32_t offset_;
    uint32_t len_;
    uint32_t page_;
    bool     locked_ = true;
};

/// Мост к hal::INvs для плат, где NVS хранится в InternalFlashStorage.
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
