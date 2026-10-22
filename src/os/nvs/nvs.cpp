#include "nvs.hpp"
#include "../crypto/crc.hpp"
#include <cstring>

namespace embed::nvs {
namespace {

constexpr uint32_t kBankMagic = 0x4E564231u; // "NVB1"
constexpr uint16_t kBankVersion = 1u;

uint32_t crc32(const uint8_t* data, uint32_t len) noexcept {
    return crypto::Crc32Zlib::compute(data, len);
}
void put_u16(uint8_t* p, uint16_t v) noexcept { p[0]=uint8_t(v); p[1]=uint8_t(v>>8); }
void put_u32(uint8_t* p, uint32_t v) noexcept {
    p[0]=uint8_t(v); p[1]=uint8_t(v>>8); p[2]=uint8_t(v>>16); p[3]=uint8_t(v>>24);
}
uint16_t get_u16(const uint8_t* p) noexcept { return uint16_t(p[0]) | (uint16_t(p[1])<<8); }
uint32_t get_u32(const uint8_t* p) noexcept {
    return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}
uint32_t align4(uint32_t n) noexcept { return (n + 3u) & ~3u; }
bool valid_key(const char* key) noexcept {
    if (key == nullptr || key[0] == '\0') return false;
    for (uint32_t i = 0u; i < kMaxKeyLen; ++i) if (key[i] == '\0') return true;
    return key[kMaxKeyLen] == '\0';
}
} // namespace

Status Store::init(const Config& cfg) noexcept {
    if (cfg.storage == nullptr || cfg.bank_size == 0u) return Status::InvalidArg;
    storage_ = cfg.storage;
    const Status b = storage_->begin();
    if (b != Status::Ok) return b;
    page_ = storage_->page_size();
    if (page_ == 0u) page_ = 1u;
    bank_size_ = cfg.bank_size;
    if ((bank_size_ % page_) != 0u) bank_size_ = ((bank_size_ + page_ - 1u) / page_) * page_;
    if (bank_size_ * 2u > storage_->capacity()) return Status::InvalidArg;
    needs_compact_ = false;
    count_ = 0u;
    for (auto& e : entries_) e = Entry{};

    uint32_t seq0 = 0u, end0 = 0u, seq1 = 0u, end1 = 0u;
    const bool v0 = load_bank(0u, seq0, end0) == Status::Ok;
    const bool v1 = load_bank(1u, seq1, end1) == Status::Ok;
    if (!v0 && !v1) return format();

    if (v1 && (!v0 || seq1 > seq0)) { active_bank_ = 1u; active_seq_ = seq1; active_pos_ = end1; }
    else { active_bank_ = 0u; active_seq_ = seq0; active_pos_ = end0; }

    const Status s = scan();
    if (s != Status::Ok) return s;
    if (needs_compact_) return compact();
    return Status::Ok;
}

void Store::deinit() noexcept {
    if (storage_ != nullptr) storage_->end();
    storage_ = nullptr;
    count_ = 0u;
}

Status Store::load_bank(uint32_t bank, uint32_t& seq, uint32_t& end) noexcept {
    const uint32_t base = bank * bank_size_;
    uint8_t hdr[kBankHeaderSize]{};
    if (storage_->read(base, hdr, sizeof(hdr)) != Status::Ok) return Status::HardwareError;
    if (get_u32(hdr) != kBankMagic || get_u16(hdr + 4u) != kBankVersion) return Status::NotFound;
    if (crc32(hdr, 10u) != get_u32(hdr + 10u)) return Status::NotFound;
    seq = get_u32(hdr + 6u);
    (void)end;
    return Status::Ok;
}

Status Store::scan() noexcept {
    count_ = 0u;
    needs_compact_ = false;
    for (uint32_t n = 0u; n < 2u; ++n) {
        const uint32_t bank = (n == 0u) ? (active_bank_ ^ 1u) : active_bank_;
        uint8_t hdr[kBankHeaderSize]{};
        const uint32_t base = bank * bank_size_;
        if (storage_->read(base, hdr, sizeof(hdr)) != Status::Ok) continue;
        if (get_u32(hdr) != kBankMagic || get_u16(hdr + 4u) != kBankVersion) continue;
        if (crc32(hdr, 10u) != get_u32(hdr + 10u)) continue;
        const uint32_t seq = get_u32(hdr + 6u);
        uint32_t pos = base + kBankHeaderSize;
        const uint32_t end = base + bank_size_;
        bool dirty = false;
        while (pos + kRecordHeaderSize <= end) {
            uint8_t rh[kRecordHeaderSize]{};
            if (storage_->read(pos, rh, sizeof(rh)) != Status::Ok) { dirty = true; break; }
            if (get_u32(rh) != kRecordMagic || crc32(rh, kRecordHeaderSize - 4u) != get_u32(rh + 28u)) {
                dirty = true; break;
            }
            const uint16_t len = get_u16(rh + 20u);
            const uint32_t total = kRecordHeaderSize + align4(len);
            if (total > (end - pos)) { dirty = true; break; }
            char key[kMaxKeyLen + 1]{};
            for (uint32_t i = 0u; i < kMaxKeyLen; ++i) key[i] = char(rh[4u + i]);
            key[kMaxKeyLen] = '\0';
            const uint16_t flags = get_u16(rh + 22u);
            if (valid_key(key)) {
                Entry* slot = nullptr;
                for (auto& e : entries_) if (e.used && std::strcmp(e.key, key) == 0) { slot = &e; break; }
                if (slot == nullptr && !(flags & kFlagDeleted)) {
                    for (auto& e : entries_) if (!e.used) { slot = &e; break; }
                }
                if (slot != nullptr) {
                    if (flags & kFlagDeleted) {
                        slot->used = false;
                        slot->key[0] = '\0';
                    } else {
                        std::strncpy(slot->key, key, kMaxKeyLen);
                        slot->key[kMaxKeyLen] = '\0';
                        slot->len = len;
                        slot->flags = flags;
                        slot->seq = seq;
                        slot->offset = pos + kRecordHeaderSize;
                        slot->used = true;
                    }
                }
            }
            pos += total;
        }
        if (bank == active_bank_) {
            active_pos_ = dirty ? (base + kBankHeaderSize) : pos;
            if (dirty) needs_compact_ = true;
        }
    }
    count_ = 0u;
    for (auto& e : entries_) if (e.used) ++count_;
    return Status::Ok;
}

Status Store::write_bank_header(uint32_t bank, uint32_t seq) noexcept {
    uint8_t hdr[kBankHeaderSize]{};
    put_u32(hdr, kBankMagic);
    put_u16(hdr + 4u, kBankVersion);
    put_u32(hdr + 6u, seq);
    put_u32(hdr + 10u, crc32(hdr, 10u));
    return storage_->write(bank * bank_size_, hdr, sizeof(hdr));
}

Status Store::append(uint32_t bank, uint32_t& pos, const char* key,
                     const void* data, Size len, uint16_t flags) noexcept {
    if (!valid_key(key) || (data == nullptr && len != 0u) || len > kMaxValueLen) return Status::InvalidArg;
    const uint32_t total = kRecordHeaderSize + align4(len);
    if (pos + total > bank * bank_size_ + bank_size_) return Status::Overflow;
    uint8_t rec[kRecordHeaderSize + kMaxValueLen]{};
    put_u32(rec, kRecordMagic);
    std::strncpy(reinterpret_cast<char*>(rec + 4u), key, kMaxKeyLen);
    put_u16(rec + 20u, static_cast<uint16_t>(len));
    put_u16(rec + 22u, flags);
    put_u32(rec + 24u, 0u);
    put_u32(rec + 28u, crc32(rec, kRecordHeaderSize - 4u));
    if (len != 0u) std::memcpy(rec + kRecordHeaderSize, data, len);
    for (uint32_t i = kRecordHeaderSize + len; i < total; ++i) rec[i] = 0xFFu;
    const Status st = storage_->write(pos, rec, total);
    if (st != Status::Ok) return st;
    pos += total;
    return Status::Ok;
}

Status Store::compact() noexcept {
    const uint32_t other = active_bank_ ^ 1u;
    const uint32_t seq = active_seq_ + 1u;
    if (storage_->erase(other * bank_size_, bank_size_) != Status::Ok) return Status::HardwareError;

    // Records — до заголовка. Пока заголовок не записан, старый банк остаётся
    // активным, и сбой питания не теряет данные.
    uint32_t pos = other * bank_size_ + kBankHeaderSize;
    for (auto& e : entries_) {
        if (!e.used) continue;
        uint8_t value[kMaxValueLen]{};
        if (storage_->read(e.offset, value, e.len) != Status::Ok) return Status::HardwareError;
        const Status st = append(other, pos, e.key, value, e.len, 0u);
        if (st != Status::Ok) return st;
    }
    const Status h = write_bank_header(other, seq);
    if (h != Status::Ok) return h;

    active_bank_ = other;
    active_seq_ = seq;
    active_pos_ = pos;
    needs_compact_ = false;
    return scan();
}

Status Store::find(const char* key, uint32_t& idx) const noexcept {
    if (!valid_key(key)) return Status::InvalidArg;
    for (uint32_t i = 0u; i < kMaxKeys; ++i) {
        if (entries_[i].used && std::strcmp(entries_[i].key, key) == 0) { idx = i; return Status::Ok; }
    }
    return Status::NotFound;
}

Store::Entry* Store::find_mut(const char* key) noexcept {
    for (auto& e : entries_) if (e.used && std::strcmp(e.key, key) == 0) return &e;
    return nullptr;
}

Status Store::get(const char* key, void* buffer, Size capacity, Size& len) noexcept {
    len = 0u;
    uint32_t idx = 0u;
    const Status f = find(key, idx);
    if (f != Status::Ok) return f;
    const Entry& e = entries_[idx];
    if (buffer == nullptr || capacity < e.len) return Status::InvalidArg;
    if (storage_->read(e.offset, buffer, e.len) != Status::Ok) return Status::HardwareError;
    len = e.len;
    return Status::Ok;
}

Status Store::set(const char* key, const void* data, Size len) noexcept {
    if (!valid_key(key) || (data == nullptr && len != 0u) || len > kMaxValueLen) return Status::InvalidArg;
    uint32_t pos = active_pos_;
    Status st = append(active_bank_, pos, key, data, len, 0u);
    if (st == Status::Overflow) {
        st = compact();
        if (st != Status::Ok) return st;
        pos = active_pos_;
        st = append(active_bank_, pos, key, data, len, 0u);
    }
    if (st != Status::Ok) return st;
    active_pos_ = pos;
    Entry* e = find_mut(key);
    if (e == nullptr) {
        for (auto& s : entries_) if (!s.used) { e = &s; break; }
    }
    if (e == nullptr) return Status::Overflow;
    std::strncpy(e->key, key, kMaxKeyLen);
    e->key[kMaxKeyLen] = '\0';
    e->len = static_cast<uint16_t>(len);
    e->offset = pos - align4(len);   // value starts right after the record header
    e->flags = 0u;
    e->used = true;
    e->seq = active_seq_;
    count_ = 0u;
    for (auto& s : entries_) if (s.used) ++count_;
    return Status::Ok;
}

Status Store::erase_key(const char* key) noexcept {
    if (!valid_key(key)) return Status::InvalidArg;
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
    if (e != nullptr) { e->used = false; e->key[0] = '\0'; }
    count_ = 0u;
    for (auto& s : entries_) if (s.used) ++count_;
    return Status::Ok;
}

Status Store::format() noexcept {
    for (uint32_t b = 0u; b < 2u; ++b) {
        if (storage_->erase(b * bank_size_, bank_size_) != Status::Ok) return Status::HardwareError;
    }
    active_bank_ = 0u;
    active_seq_ = 1u;
    active_pos_ = kBankHeaderSize;
    count_ = 0u;
    for (auto& e : entries_) e = Entry{};
    needs_compact_ = false;
    return write_bank_header(0u, 1u);
}

bool Store::contains(const char* key) const noexcept {
    uint32_t idx = 0u;
    return find(key, idx) == Status::Ok;
}

Status HalNvs::init() noexcept { return store_ != nullptr ? Status::Ok : Status::NotSupported; }
Status HalNvs::read(const char* key, void* buffer, Size capacity, Size& len) noexcept {
    return store_ != nullptr ? store_->get(key, buffer, capacity, len) : Status::NotSupported;
}
Status HalNvs::write(const char* key, const void* data, Size len) noexcept {
    return store_ != nullptr ? store_->set(key, data, len) : Status::NotSupported;
}
Status HalNvs::erase(const char* key) noexcept {
    return store_ != nullptr ? store_->erase_key(key) : Status::NotSupported;
}

} // namespace embed::nvs
