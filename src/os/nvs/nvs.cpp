#include "nvs.hpp"
#include "../crypto/crc.hpp"

#include <cstring>

namespace embed::nvs {
namespace {

constexpr uint32_t kBankMagicV1 = 0x4E564231u;   // "NVB1"
constexpr uint16_t kBankVersionV1 = 1u;
constexpr uint32_t kRecordMagicV1 = 0x4E565331u; // "NVS1"
constexpr uint32_t kRecordHeaderV1 = 32u;
constexpr uint32_t kKeyLenV1 = 16u;

uint32_t crc32(const uint8_t* data, uint32_t len) noexcept {
    return crypto::Crc32Zlib::compute(data, len);
}

void put_u16(uint8_t* p, uint16_t v) noexcept {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
}
void put_u32(uint8_t* p, uint32_t v) noexcept {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
void put_u64(uint8_t* p, uint64_t v) noexcept {
    for (uint8_t i = 0u; i < 8u; ++i) p[i] = uint8_t(v >> (8u * i));
}
uint16_t get_u16(const uint8_t* p) noexcept {
    return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8);
}
uint32_t get_u32(const uint8_t* p) noexcept {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t get_u64(const uint8_t* p) noexcept {
    uint64_t v = 0u;
    for (uint8_t i = 0u; i < 8u; ++i) v |= uint64_t(p[i]) << (8u * i);
    return v;
}

} // namespace

bool Store::valid_name(const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') return false;
    Size n = 0u;
    for (; name[n] != '\0'; ++n) {
        if (n >= kMaxNamespaceLen) return false;
        const char c = name[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return n > 0u;
}

bool Store::valid_key_path(const char* key) noexcept {
    if (key == nullptr || key[0] == '\0') return false;
    Size n = 0u;
    for (; key[n] != '\0'; ++n) {
        if (n >= kMaxKeyLen) return false;
        const char c = key[n];
        if (c < 0x20 || c == 0x7F || c == '\\') return false;
        if (c == '/' && (n == 0u || key[n + 1u] == '\0' || key[n + 1u] == '/')) {
            return false;
        }
    }
    return n > 0u;
}

uint32_t Store::hash_key(const char* key, Size len) noexcept {
    uint32_t h = 2166136261u;   // FNV-1a
    for (Size i = 0u; i < len; ++i) {
        h ^= static_cast<uint8_t>(key[i]);
        h *= 16777619u;
    }
    return h;
}

Store::Entry* Store::find_mut(const char* key) noexcept {
    if (key == nullptr) return nullptr;
    const Size len = std::strlen(key);
    const uint32_t h = hash_key(key, len);
    for (auto& e : entries_) {
        if (!e.used) continue;
        if (e.hash != h || e.key_len != len) continue;
        char stored[kMaxKeyLen + 1]{};
        if (read_key(e, stored) != Status::Ok) continue;
        if (std::strcmp(stored, key) == 0) return &e;
    }
    return nullptr;
}

Status Store::find(const char* key, uint32_t& idx) const noexcept {
    if (key == nullptr) return Status::InvalidArg;
    const Size len = std::strlen(key);
    const uint32_t h = hash_key(key, len);
    for (uint32_t i = 0u; i < kMaxKeys; ++i) {
        const Entry& e = entries_[i];
        if (!e.used || e.hash != h || e.key_len != len) continue;
        char stored[kMaxKeyLen + 1]{};
        if (read_key(e, stored) != Status::Ok) continue;
        if (std::strcmp(stored, key) == 0) { idx = i; return Status::Ok; }
    }
    return Status::NotFound;
}

Status Store::read_key(const Entry& e, char out[kMaxKeyLen + 1]) const noexcept {
    out[0] = '\0';
    if (storage_ == nullptr) return Status::Unavailable;
    if (e.format == 1u) {
        uint8_t buf[kKeyLenV1]{};
        const Status st = storage_->read(e.offset + 4u, buf, sizeof(buf));
        if (st != Status::Ok) return st;
        Size n = 0u;
        while (n < kKeyLenV1 && buf[n] != 0u) { out[n] = char(buf[n]); ++n; }
        out[n] = '\0';
        return Status::Ok;
    }
    if (e.key_len == 0u || e.key_len > kMaxKeyLen) return Status::Corruption;
    const Status st = storage_->read(e.offset + kRecordHeaderSize, out, e.key_len);
    if (st != Status::Ok) return st;
    out[e.key_len] = '\0';
    return Status::Ok;
}

Status Store::read_nonce(const Entry& e, uint64_t& nonce) const noexcept {
    nonce = 0u;
    if (e.format != 2u) return Status::Ok;   // v1 не шифровался
    uint8_t buf[8]{};
    const Status st = storage_->read(e.offset + 8u, buf, sizeof(buf));
    if (st != Status::Ok) return st;
    nonce = get_u64(buf);
    return Status::Ok;
}

Status Store::read_value(const Entry& e, void* out) const noexcept {
    if (storage_ == nullptr || out == nullptr) return Status::InvalidArg;
    const uint32_t value_off = e.offset +
        ((e.format == 1u) ? kRecordHeaderV1
                          : (kRecordHeaderSize + align4(e.key_len)));
    return storage_->read(value_off, out, e.value_len);
}

Status Store::load_bank(uint32_t bank, uint16_t& format, uint16_t& schema,
                        uint32_t& seq) noexcept {
    format = 0u; schema = 0u; seq = 0u;
    uint8_t hdr[kBankHeaderSize]{};
    const uint32_t base = bank * bank_size_;
    const Status st = storage_->read(base, hdr, sizeof(hdr));
    if (st != Status::Ok) return st;

    const uint32_t magic = get_u32(hdr);
    if (magic == kBankMagic) {
        if (get_u16(hdr + 4u) != kFormatVersion) return Status::NotFound;
        if (crc32(hdr, 28u) != get_u32(hdr + 28u)) return Status::NotFound;
        format = kFormatVersion;
        schema = get_u16(hdr + 6u);
        seq = get_u32(hdr + 8u);
        return Status::Ok;
    }
    if (magic == kBankMagicV1) {
        if (get_u16(hdr + 4u) != kBankVersionV1) return Status::NotFound;
        if (crc32(hdr, 10u) != get_u32(hdr + 10u)) return Status::NotFound;
        format = 1u;
        schema = 0u;
        seq = get_u32(hdr + 6u);
        return Status::Ok;
    }
    return Status::NotFound;
}

Status Store::write_bank_header(uint32_t bank, uint32_t seq, uint16_t schema,
                                uint16_t /*format*/) noexcept {
    uint8_t hdr[kBankHeaderSize]{};
    for (auto& b : hdr) b = 0xFFu;
    put_u32(hdr, kBankMagic);
    put_u16(hdr + 4u, kFormatVersion);
    put_u16(hdr + 6u, schema);
    put_u32(hdr + 8u, seq);
    put_u32(hdr + 12u, cipher_ != nullptr ? kBankFlagEncrypted : 0u);
    put_u32(hdr + 28u, crc32(hdr, 28u));
    return storage_->write(bank * bank_size_, hdr, sizeof(hdr));
}

Status Store::scan() noexcept {
    count_ = 0u;
    needs_compact_ = false;
    for (auto& e : entries_) e = Entry{};

    for (uint32_t n = 0u; n < 2u; ++n) {
        const uint32_t bank = (n == 0u) ? (active_bank_ ^ 1u) : active_bank_;
        const uint32_t base = bank * bank_size_;
        uint8_t hdr[kBankHeaderSize]{};
        if (storage_->read(base, hdr, sizeof(hdr)) != Status::Ok) continue;

        uint16_t format = 0u;
        if (get_u32(hdr) == kBankMagic && get_u16(hdr + 4u) == kFormatVersion &&
            crc32(hdr, 28u) == get_u32(hdr + 28u)) {
            format = kFormatVersion;
        } else if (get_u32(hdr) == kBankMagicV1 &&
                   get_u16(hdr + 4u) == kBankVersionV1 &&
                   crc32(hdr, 10u) == get_u32(hdr + 10u)) {
            format = 1u;
        } else {
            continue;
        }

        uint32_t pos = base + kBankHeaderSize;
        const uint32_t end = base + bank_size_;
        bool dirty = false;

        while (pos + (format == 1u ? kRecordHeaderV1 : kRecordHeaderSize) <= end) {
            uint8_t rh[kBankHeaderSize]{};
            const uint32_t hdr_len = (format == 1u) ? kRecordHeaderV1
                                                    : kRecordHeaderSize;
            if (storage_->read(pos, rh, hdr_len) != Status::Ok) { dirty = true; break; }
            // Хвост журнала — стёртая Flash (0xFF), а не порча записи.
            // Раньше сканер считал это ошибкой, откатывал active_pos_ к
            // началу банка и следующая запись затирала живые данные.
            if (get_u32(rh) == 0xFFFFFFFFu) break;

            uint32_t total = 0u;
            uint16_t value_len = 0u;
            uint8_t key_len = 0u;
            uint8_t flags = 0u;
            char key[kMaxKeyLen + 1]{};

            if (format == 2u) {
                if (get_u32(rh) != kRecordMagic) { dirty = true; break; }
                key_len = rh[4];
                flags = rh[5];
                value_len = get_u16(rh + 6u);
                if (key_len == 0u || key_len > kMaxKeyLen ||
                    value_len > kMaxValueLen + kMaxCipherOverhead) {
                    dirty = true; break;
                }
                const uint32_t data_len = kRecordHeaderSize + align4(key_len) + value_len;
                total = ((data_len + gran_ - 1u) / gran_) * gran_;
                if (total > (end - pos) || total > sizeof(record_)) { dirty = true; break; }

                if (storage_->read(pos, record_, total) != Status::Ok) { dirty = true; break; }
                const uint32_t stored_crc = get_u32(record_ + 16u);
                put_u32(record_ + 16u, 0u);
                const bool crc_ok = crc32(record_, data_len) == stored_crc;
                put_u32(record_ + 16u, stored_crc);
                if (!crc_ok) { dirty = true; break; }
                for (uint8_t i = 0u; i < key_len; ++i) key[i] = char(record_[kRecordHeaderSize + i]);
                key[key_len] = '\0';
            } else {
                if (get_u32(rh) != kRecordMagicV1) { dirty = true; break; }
                value_len = get_u16(rh + 20u);
                flags = uint8_t(get_u16(rh + 22u) & 0x1u);
                if (value_len > kMaxValueLen) { dirty = true; break; }
                total = kRecordHeaderV1 + align4(value_len);
                if (total > (end - pos) || total > sizeof(record_)) { dirty = true; break; }

                if (storage_->read(pos, record_, total) != Status::Ok) { dirty = true; break; }
                const uint32_t stored_crc = get_u32(record_ + 28u);
                if (crc32(record_, 28u) != stored_crc) { dirty = true; break; }
                if (storage_->read(pos + 4u, key, kKeyLenV1) != Status::Ok) { dirty = true; break; }
                key[kKeyLenV1] = '\0';
                key_len = static_cast<uint8_t>(std::strlen(key));
            }

            if (!valid_key_path(key)) { dirty = true; break; }

            const uint32_t h = hash_key(key, key_len);
            Entry* slot = nullptr;
            for (auto& e : entries_) {
                if (!e.used || e.hash != h || e.key_len != key_len) continue;
                char stored[kMaxKeyLen + 1]{};
                if (read_key(e, stored) == Status::Ok && std::strcmp(stored, key) == 0) {
                    slot = &e;
                    break;
                }
            }
            if (slot == nullptr) {
                for (auto& e : entries_) if (!e.used) { slot = &e; break; }
            }
            if (slot != nullptr) {
                if ((flags & kFlagDeleted) != 0u) {
                    slot->used = false;
                } else {
                    slot->used = true;
                    slot->hash = h;
                    slot->offset = pos;
                    slot->value_len = value_len;
                    slot->key_len = key_len;
                    slot->flags = flags;
                    slot->format = static_cast<uint8_t>(format);
                }
            }
            pos += total;
        }

        if (bank == active_bank_) {
            active_pos_ = dirty ? (base + kBankHeaderSize) : pos;
            active_format_ = format;
            if (dirty) needs_compact_ = true;
        }
    }

    count_ = 0u;
    for (auto& e : entries_) if (e.used) ++count_;
    return Status::Ok;
}

Status Store::init(const Config& cfg) noexcept {
    if (cfg.storage == nullptr || cfg.bank_size == 0u) return Status::InvalidArg;
    storage_ = cfg.storage;
    cipher_  = cfg.cipher;
    const Status b = storage_->begin();
    if (b != Status::Ok) return b;
    page_ = storage_->page_size();
    if (page_ == 0u) page_ = 1u;
    gran_ = storage_->write_granularity();
    if (gran_ == 0u) gran_ = 1u;
    if (gran_ > 8u) gran_ = 8u;
    bank_size_ = cfg.bank_size;
    if ((bank_size_ % page_) != 0u) {
        bank_size_ = ((bank_size_ + page_ - 1u) / page_) * page_;
    }
    if (bank_size_ * 2u > storage_->capacity()) return Status::InvalidArg;
    if (bank_size_ < (kBankHeaderSize + kRecordHeaderSize + 64u)) {
        return Status::InvalidArg;
    }

    initialized_ = false;
    count_ = 0u;
    needs_compact_ = false;
    active_bank_ = 0u;
    active_seq_ = 1u;
    active_pos_ = kBankHeaderSize;
    active_format_ = kFormatVersion;
    schema_ = (cfg.schema_version == 0u) ? 1u : cfg.schema_version;
    for (auto& e : entries_) e = Entry{};

    uint16_t f0 = 0u, f1 = 0u, s0 = 0u, s1 = 0u;
    uint32_t q0 = 0u, q1 = 0u;
    const bool v0 = load_bank(0u, f0, s0, q0) == Status::Ok;
    const bool v1 = load_bank(1u, f1, s1, q1) == Status::Ok;
    if (!v0 && !v1) {
        schema_ = (cfg.schema_version == 0u) ? 1u : cfg.schema_version;
        const Status st = format();
        if (st != Status::Ok) return st;
        initialized_ = true;
        return Status::Ok;
    }

    if (v1 && (!v0 || q1 >= q0)) {
        active_bank_ = 1u; active_seq_ = q1; schema_ = s1; active_format_ = f1;
    } else {
        active_bank_ = 0u; active_seq_ = q0; schema_ = s0; active_format_ = f0;
    }

    const Status s = scan();
    if (s != Status::Ok) return s;

    // Старый формат: переписываем в v2, сохраняя значения.
    if (active_format_ == 1u) {
        schema_ = (cfg.schema_version == 0u) ? 1u : cfg.schema_version;
        needs_compact_ = true;
    }

    if (cfg.schema_version != 0u) {
        if (schema_ > cfg.schema_version) return Status::NotSupported;   // downgrade
        if (schema_ < cfg.schema_version) {
            if (cfg.migrate == nullptr) return Status::NotSupported;
            const Status m = cfg.migrate(cfg.migrate_ctx, schema_, cfg.schema_version);
            if (m != Status::Ok) return m;
            schema_ = cfg.schema_version;
            needs_compact_ = true;
        }
    }

    if (needs_compact_) {
        const Status c = compact();
        if (c != Status::Ok) return c;
    }
    initialized_ = true;
    return Status::Ok;
}

void Store::deinit() noexcept {
    if (storage_ != nullptr) storage_->end();
    storage_ = nullptr;
    initialized_ = false;
    count_ = 0u;
}

Status Store::append(uint32_t bank, uint32_t& pos, const char* key,
                     const void* data, Size len, uint16_t flags) noexcept {
    if (!valid_key_path(key) || (data == nullptr && len != 0u) ||
        len > kMaxValueLen) {
        return Status::InvalidArg;
    }
    if (cipher_ != nullptr && cipher_->overhead() > kMaxCipherOverhead) {
        return Status::NotSupported;
    }

    const Size key_len = std::strlen(key);
    const Size overhead = (cipher_ != nullptr && (flags & kFlagEncrypted) != 0u)
                        ? cipher_->overhead() : 0u;
    const Size value_len = len + overhead;
    if (value_len > kMaxValueLen + kMaxCipherOverhead) return Status::Overflow;

    const uint32_t data_len = kRecordHeaderSize +
        align4(static_cast<uint32_t>(key_len)) + static_cast<uint32_t>(value_len);
    const uint32_t total = ((data_len + gran_ - 1u) / gran_) * gran_;
    if (total > sizeof(record_) || data_len > total) return Status::Overflow;
    if (pos + total > bank * bank_size_ + bank_size_) return Status::Overflow;

    const uint64_t nonce = (uint64_t(active_seq_) << 32u) ^
                           (uint64_t(bank) << 63u) ^ uint64_t(pos);
    for (uint32_t i = 0u; i < total; ++i) record_[i] = 0u;
    for (uint32_t i = data_len; i < total; ++i) record_[i] = 0xFFu;
    put_u32(record_, kRecordMagic);
    record_[4] = static_cast<uint8_t>(key_len);
    record_[5] = static_cast<uint8_t>(flags);
    put_u16(record_ + 6u, static_cast<uint16_t>(value_len));
    put_u64(record_ + 8u, nonce);
    put_u32(record_ + 20u, 0u);

    std::memcpy(record_ + kRecordHeaderSize, key, key_len);
    const uint32_t value_off = kRecordHeaderSize + align4(static_cast<uint32_t>(key_len));

    if ((flags & kFlagEncrypted) != 0u) {
        if (cipher_ == nullptr) return Status::NotSupported;
        const Status st = cipher_->seal(nonce, key, key_len, data, len,
                                        record_ + value_off);
        if (st != Status::Ok) return st;
    } else if (len != 0u) {
        std::memcpy(record_ + value_off, data, len);
    }

    put_u32(record_ + 16u, 0u);
    put_u32(record_ + 16u, crc32(record_, data_len));

    const Status st = storage_->write(pos, record_, total);
    if (st != Status::Ok) return st;
    last_record_offset_ = pos;
    pos += total;
    return Status::Ok;
}

Status Store::set(const char* key, const void* data, Size len) noexcept {
    if (!valid_key_path(key) || len > kMaxValueLen ||
        (data == nullptr && len != 0u)) {
        return Status::InvalidArg;
    }
    if (cipher_ != nullptr && cipher_->overhead() > kMaxCipherOverhead) {
        return Status::NotSupported;
    }

    // Не пишем запись, если для неё нет места в таблице ключей: иначе
    // append() успел бы занять Flash, а ключ остался бы недоступен до
    // следующего сканирования.
    if (find_mut(key) == nullptr) {
        bool free_slot = false;
        for (const auto& e : entries_) {
            if (!e.used) { free_slot = true; break; }
        }
        if (!free_slot) return Status::Overflow;
    }

    const uint16_t flags = (cipher_ != nullptr) ? kFlagEncrypted : 0u;
    uint32_t pos = active_pos_;
    Status st = append(active_bank_, pos, key, data, len, flags);
    if (st == Status::Overflow) {
        st = compact();
        if (st != Status::Ok) return st;
        pos = active_pos_;
        st = append(active_bank_, pos, key, data, len, flags);
    }
    if (st != Status::Ok) return st;
    active_pos_ = pos;

    Entry* e = find_mut(key);
    if (e == nullptr) {
        for (auto& s : entries_) if (!s.used) { e = &s; break; }
    }
    if (e == nullptr) return Status::Overflow;

    const Size key_len = std::strlen(key);
    e->hash = hash_key(key, key_len);
    e->offset = last_record_offset_;
    e->value_len = static_cast<uint16_t>(len + ((cipher_ != nullptr) ? cipher_->overhead() : 0u));
    e->key_len = static_cast<uint8_t>(key_len);
    e->flags = static_cast<uint8_t>(flags);
    e->format = kFormatVersion;
    e->used = true;

    count_ = 0u;
    for (auto& s : entries_) if (s.used) ++count_;
    return Status::Ok;
}

Status Store::erase_key(const char* key) noexcept {
    if (!valid_key_path(key)) return Status::InvalidArg;
    if (find_mut(key) == nullptr) return Status::NotFound;

    uint32_t pos = active_pos_;
    Status st = append(active_bank_, pos, key, nullptr, 0u, kFlagDeleted);
    if (st == Status::Overflow) {
        st = compact();
        if (st != Status::Ok) return st;
        pos = active_pos_;
        st = append(active_bank_, pos, key, nullptr, 0u, kFlagDeleted);
    }
    if (st != Status::Ok) return st;
    active_pos_ = pos;

    Entry* e = find_mut(key);
    if (e != nullptr) e->used = false;
    count_ = 0u;
    for (auto& s : entries_) if (s.used) ++count_;
    return Status::Ok;
}

Status Store::compact() noexcept {
    const uint32_t other = active_bank_ ^ 1u;
    if (storage_->erase(other * bank_size_, bank_size_) != Status::Ok) {
        return Status::HardwareError;
    }

    uint32_t pos = other * bank_size_ + kBankHeaderSize;
    for (auto& e : entries_) {
        if (!e.used) continue;

        char key[kMaxKeyLen + 1]{};
        Status st = read_key(e, key);
        if (st != Status::Ok) return st;

        const uint16_t value_len = e.value_len;
        if ((e.flags & kFlagEncrypted) != 0u) {
            if (cipher_ == nullptr) return Status::NotSupported;
            uint64_t nonce = 0u;
            st = read_nonce(e, nonce);
            if (st != Status::Ok) return st;
            if (e.value_len > sizeof(record_)) return Status::Corruption;
            st = read_value(e, record_);
            if (st != Status::Ok) return st;
            Size plain_len = 0u;
            st = cipher_->open(nonce, key, std::strlen(key),
                               record_, e.value_len, plain_, plain_len);
            if (st != Status::Ok) return st;
            st = append(other, pos, key, plain_, plain_len, 0u);
        } else {
            if (value_len > sizeof(plain_)) return Status::Corruption;
            st = read_value(e, plain_);
            if (st != Status::Ok) return st;
            st = append(other, pos, key, plain_, value_len, 0u);
        }
        if (st != Status::Ok) return st;
    }

    const Status h = write_bank_header(other, active_seq_ + 1u, schema_, kFormatVersion);
    if (h != Status::Ok) return h;

    active_bank_ = other;
    active_seq_ += 1u;
    active_pos_ = pos;
    active_format_ = kFormatVersion;
    needs_compact_ = false;
    return scan();
}

Status Store::format() noexcept {
    for (uint32_t b = 0u; b < 2u; ++b) {
        if (storage_->erase(b * bank_size_, bank_size_) != Status::Ok) {
            return Status::HardwareError;
        }
    }
    active_bank_ = 0u;
    active_seq_ = 1u;
    active_pos_ = kBankHeaderSize;
    active_format_ = kFormatVersion;
    count_ = 0u;
    for (auto& e : entries_) e = Entry{};
    needs_compact_ = false;
    return write_bank_header(0u, 1u, schema_, kFormatVersion);
}

Status Store::get(const char* key, void* buffer, Size capacity, Size& len) noexcept {
    len = 0u;
    uint32_t idx = 0u;
    const Status f = find(key, idx);
    if (f != Status::Ok) return f;
    const Entry& e = entries_[idx];
    if (buffer == nullptr) return Status::InvalidArg;

    if ((e.flags & kFlagEncrypted) != 0u) {
        if (cipher_ == nullptr) return Status::NotSupported;
        const Size overhead = cipher_->overhead();
        if (e.value_len < overhead) return Status::Corruption;
        const Size plain_len = e.value_len - overhead;
        if (capacity < plain_len) return Status::NoMemory;
        uint64_t nonce = 0u;
        Status st = read_nonce(e, nonce);
        if (st != Status::Ok) return st;
        st = read_value(e, record_);
        if (st != Status::Ok) return st;
        Size got = 0u;
        st = cipher_->open(nonce, key, std::strlen(key), record_, e.value_len,
                           buffer, got);
        if (st != Status::Ok) return st;
        len = got;
        return Status::Ok;
    }

    if (capacity < e.value_len) return Status::NoMemory;
    const Status st = read_value(e, buffer);
    if (st != Status::Ok) return st;
    len = e.value_len;
    return Status::Ok;
}

bool Store::contains(const char* key) const noexcept {
    uint32_t idx = 0u;
    return find(key, idx) == Status::Ok;
}

Namespace Store::open_namespace(const char* name) noexcept {
    if (name == nullptr || name[0] == '\0') return Namespace(this, "");
    if (!valid_name(name)) return Namespace{};
    return Namespace(this, name);
}

Namespace::Namespace(Store* store, const char* name) noexcept : store_(store) {
    if (name == nullptr) { name_[0] = '\0'; return; }
    Size i = 0u;
    while (i < kMaxNamespaceLen && name[i] != '\0') {
        name_[i] = name[i];
        ++i;
    }
    name_[i] = '\0';
}

bool Namespace::full_key(const char* key, char out[kMaxKeyLen + 1]) const noexcept {
    if (store_ == nullptr || key == nullptr) return false;
    const Size klen = std::strlen(key);
    if (klen == 0u || klen > kMaxNameLen) return false;
    for (Size i = 0u; i < klen; ++i) {
        if (key[i] == '/') return false;
    }
    const Size nlen = std::strlen(name_);
    if (nlen == 0u) {
        if (klen > kMaxKeyLen) return false;
        std::memcpy(out, key, klen + 1u);
        return true;
    }
    if (nlen + 1u + klen > kMaxKeyLen) return false;
    Size p = 0u;
    for (Size i = 0u; i < nlen; ++i) out[p++] = name_[i];
    out[p++] = '/';
    for (Size i = 0u; i < klen; ++i) out[p++] = key[i];
    out[p] = '\0';
    return true;
}

Status Namespace::get(const char* key, void* buffer, Size capacity, Size& len) const noexcept {
    len = 0u;
    char full[kMaxKeyLen + 1]{};
    if (!full_key(key, full)) return Status::InvalidArg;
    return store_->get(full, buffer, capacity, len);
}

Status Namespace::set(const char* key, const void* data, Size len) const noexcept {
    char full[kMaxKeyLen + 1]{};
    if (!full_key(key, full)) return Status::InvalidArg;
    return store_->set(full, data, len);
}

Status Namespace::erase(const char* key) const noexcept {
    char full[kMaxKeyLen + 1]{};
    if (!full_key(key, full)) return Status::InvalidArg;
    return store_->erase_key(full);
}

bool Namespace::contains(const char* key) const noexcept {
    char full[kMaxKeyLen + 1]{};
    if (!full_key(key, full)) return false;
    return store_->contains(full);
}

Status HalNvs::init() noexcept {
    return store_ != nullptr ? Status::Ok : Status::NotSupported;
}
Status HalNvs::read(const char* key, void* buffer, Size capacity, Size& len) noexcept {
    return store_ != nullptr ? store_->get(key, buffer, capacity, len)
                             : Status::NotSupported;
}
Status HalNvs::write(const char* key, const void* data, Size len) noexcept {
    return store_ != nullptr ? store_->set(key, data, len) : Status::NotSupported;
}
Status HalNvs::erase(const char* key) noexcept {
    return store_ != nullptr ? store_->erase_key(key) : Status::NotSupported;
}

} // namespace embed::nvs
