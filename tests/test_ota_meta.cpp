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
    uint32_t sc[2] = {448u * 1024u, 448u * 1024u};
    const ota::LayoutCheck lc = ota::check_layout(big, 16u * 1024u,
                                                   16u * 1024u, so, sc);
    check("F4 layout check", lc.ok && lc.image_limit == 448u * 1024u);

    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
