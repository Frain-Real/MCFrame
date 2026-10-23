// tests/test_ota_meta.cpp
// ---------------------------------------------------------------------------
// Метаданные OTA: журнал в одном секторе, append вместо стирания.
// ---------------------------------------------------------------------------
#include "os/ota/ota_storage.hpp"

#include <cstdio>
#include <cstring>

using namespace embed;

namespace {

int g_failed = 0;
int g_total = 0;
void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

class CountingStorage final : public ota::Storage {
public:
    CountingStorage(ota::Storage& inner, uint32_t cap, uint32_t page)
        : inner_(inner), cap_(cap), page_(page) {}
    uint32_t capacity() const noexcept override { return cap_; }
    uint32_t page_size() const noexcept override { return page_; }
    uint32_t write_granularity() const noexcept override { return 1u; }
    const char* name() const noexcept override { return "counting"; }
    Status erase(uint32_t off, uint32_t len) noexcept override {
        if (fail_after_ == 0u) return Status::HardwareError;
        if (fail_after_ != 0xFFFFFFFFu) --fail_after_;
        ++erases_;
        return inner_.erase(off, len);
    }
    Status write(uint32_t off, const void* data, uint32_t len) noexcept override {
        if (fail_after_ == 0u) return Status::HardwareError;
        if (fail_after_ != 0xFFFFFFFFu) --fail_after_;
        ++writes_;
        return inner_.write(off, data, len);
    }
    Status read(uint32_t off, void* data, uint32_t len) noexcept override {
        return inner_.read(off, data, len);
    }
    void fail_after(uint32_t n) noexcept { fail_after_ = n; }
    uint32_t erases() const noexcept { return erases_; }
    uint32_t writes() const noexcept { return writes_; }
private:
    ota::Storage& inner_;
    uint32_t cap_;
    uint32_t page_;
    uint32_t erases_ = 0u;
    uint32_t writes_ = 0u;
    uint32_t fail_after_ = 0xFFFFFFFFu;
};


/// Модель обрыва питания: primary уже стёрт, запись в него падает, backup
/// остаётся целым. Проверяем именно окно между erase(primary) и write.
class PowerLossStorage final : public ota::Storage {
public:
    explicit PowerLossStorage(ota::MemoryStorage& inner) : inner_(inner) {}
    uint32_t capacity() const noexcept override { return inner_.capacity(); }
    uint32_t page_size() const noexcept override { return inner_.page_size(); }
    uint32_t write_granularity() const noexcept override { return inner_.write_granularity(); }
    const char* name() const noexcept override { return "power-loss"; }
    Status erase(uint32_t off, uint32_t len) noexcept override {
        if (off == 0u) primary_erased_ = true;
        return inner_.erase(off, len);
    }
    Status write(uint32_t off, const void* data, uint32_t len) noexcept override {
        if (fail_primary_ && primary_erased_ && off < 16u * 1024u) {
            if (allowed_primary_writes_ == 0u) return Status::HardwareError;
            if (allowed_primary_writes_ != 0xFFFFFFFFu) --allowed_primary_writes_;
        }
        return inner_.write(off, data, len);
    }
    Status read(uint32_t off, void* data, uint32_t len) noexcept override {
        return inner_.read(off, data, len);
    }
    void fail_primary(bool on) noexcept { fail_primary_ = on; }
    void clear_primary_erased() noexcept { primary_erased_ = false; }
    void allow_primary_writes(uint32_t n) noexcept { allowed_primary_writes_ = n; }
private:
    ota::MemoryStorage& inner_;
    bool fail_primary_ = false;
    bool primary_erased_ = false;
    uint32_t allowed_primary_writes_ = 0u;
};

constexpr uint32_t kCap = 64u * 1024u;
uint8_t g_mem[kCap]{};

ota::SlotRecord make_rec(uint32_t version) {
    ota::SlotRecord r{};
    r.slot = 0u;
    r.state = static_cast<uint8_t>(ota::proto::State::Idle);
    r.fw_version = version;
    r.image_size = 1024u;
    r.image_crc32 = 0x12345678u;
    return r;
}

ota::SlotRecord make_rec_slot(uint8_t slot, uint32_t version) {
    ota::SlotRecord r = make_rec(version);
    r.slot = slot;
    return r;
}

} // namespace

int main() {
    std::printf("=== OTA meta journal ===\n");

    ota::MemoryStorage mem;
    mem.attach(g_mem, sizeof(g_mem), false);
    CountingStorage storage(mem, sizeof(g_mem), 1024u);
    ota::SlotStore store;

    check("init 16K meta", store.init(&storage, 0u, 16u * 1024u) == Status::Ok);
    check("meta size", store.meta_size() == 16u * 1024u);

    ota::SlotRecord first = make_rec(1u);
    check("first store", store.store(0u, first) == Status::Ok);
    check("append does not erase", storage.erases() == 0u);
    check("one record is one write", storage.writes() == 1u);

    ota::SlotRecord out{};
    check("load first", store.load(0u, out) == Status::Ok && out.fw_version == 1u);
    check("other slot empty", store.load(1u, out) == Status::NotFound);

    // 204 записи по 80 Б — больше 16K: должна случиться компакция.
    for (uint32_t i = 2u; i <= 300u; ++i) {
        ota::SlotRecord r = make_rec(i);
        if (store.store(0u, r) != Status::Ok) {
            check("fill store", false);
            break;
        }
    }
    check("compaction happened", storage.erases() >= 1u);
    check("latest survives compaction",
          store.load(0u, out) == Status::Ok && out.fw_version == 300u);

    // Обрыв записи: предыдущая запись цела.
    ota::SlotRecord keep = make_rec(301u);
    check("store before failure", store.store(0u, keep) == Status::Ok);
    storage.fail_after(0u);
    ota::SlotRecord bad = make_rec(999u);
    check("write failure rejected", store.store(0u, bad) != Status::Ok);
    storage.fail_after(0xFFFFFFFFu);
    check("previous value intact after failure",
          store.load(0u, out) == Status::Ok && out.fw_version == 301u);

    // check_layout с meta 16K и слотами F4 (64K / 512K) на 1 МиБ.
    static uint8_t big_mem[1024u * 1024u]{};
    ota::MemoryStorage big;
    big.attach(big_mem, sizeof(big_mem), false);
    uint32_t so[2] = {64u * 1024u, 512u * 1024u};
    uint32_t sc[2] = {448u * 1024u, 384u * 1024u};
    const ota::LayoutCheck lc = ota::check_layout(big, 16u * 1024u,
                                                   16u * 1024u, so, sc);
    check("F4 layout check", lc.ok && lc.image_limit == 384u * 1024u);

#if MCFRAME_META_BACKUP
    // Power-loss посреди компакции: primary стёрт, backup уже записан.
    ota::MemoryStorage loss_mem;
    loss_mem.attach(g_mem, sizeof(g_mem), false);
    PowerLossStorage loss(loss_mem);
    ota::SlotStore ls;
    check("backup init",
          ls.init(&loss, 0u, 16u * 1024u, 16u * 1024u, 16u * 1024u) == Status::Ok);
    // Пишем, пока очередной store не потребует компакции: именно в этот
    // момент primary будет стёрт, а запись в него — искусственно оборвана.
    uint32_t committed = 0u;
    bool lost_power = false;
    for (uint32_t i = 1u; i <= 1000u; ++i) {
        loss.clear_primary_erased();
        loss.fail_primary(true);
        ota::SlotRecord r = make_rec(i);
        const Status st = ls.store(0u, r);
        if (st != Status::Ok) {
            lost_power = true;
            committed = i - 1u;
            break;
        }
        loss.fail_primary(false);
        committed = i;
    }
    check("power loss during compaction", lost_power && committed > 0u);
    loss.fail_primary(false);
    ota::SlotRecord before{};
    check("backup latest before loss",
          ls.load(0u, before) == Status::Ok && before.fw_version == committed);

    // Новый экземпляр: primary пуст/повреждён, должен прочитать backup.
    ota::SlotStore recovered;
    check("backup recovery init",
          recovered.init(&loss, 0u, 16u * 1024u, 16u * 1024u, 16u * 1024u) == Status::Ok);
    ota::SlotRecord rec{};
    check("backup recovery keeps latest",
          recovered.load(0u, rec) == Status::Ok && rec.fw_version == committed);
    check("backup offset/size", recovered.backup_offset() == 16u * 1024u &&
                                recovered.backup_size() == 16u * 1024u);
#endif

    // Обрыв после первой из двух записей снимка: primary содержит одну
    // валидную запись, backup — две. Это нельзя принять за «меньше слотов».
    ota::MemoryStorage partial_mem;
    partial_mem.attach(g_mem, sizeof(g_mem), false);
    PowerLossStorage partial(partial_mem);
    ota::SlotStore ps;
    check("partial backup init",
          ps.init(&partial, 0u, 16u * 1024u, 16u * 1024u, 16u * 1024u) == Status::Ok);
    for (uint32_t i = 1u; i <= 300u; ++i) {
        (void)ps.store(0u, make_rec(i));
        (void)ps.store(1u, make_rec_slot(1u, i));
    }
    uint32_t partial_committed = 300u;
    bool partial_failed = false;
    for (uint32_t i = 301u; i <= 1000u; ++i) {
        partial.clear_primary_erased();
        partial.fail_primary(true);
        partial.allow_primary_writes(1u);
        if (ps.store(0u, make_rec(i)) != Status::Ok) {
            partial_failed = true;
            partial_committed = i - 1u;
            break;
        }
        partial.fail_primary(false);
    }
    partial.fail_primary(false);
    check("partial rewrite fails", partial_failed);
    ota::SlotStore prs;
    check("partial recovery init",
          prs.init(&partial, 0u, 16u * 1024u, 16u * 1024u, 16u * 1024u) == Status::Ok);
    ota::SlotRecord r0{}, r1{};
    check("partial recovery slot0",
          prs.load(0u, r0) == Status::Ok && r0.fw_version == partial_committed);
    check("partial recovery slot1",
          prs.load(1u, r1) == Status::Ok && r1.fw_version == 300u);

    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
