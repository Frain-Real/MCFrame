// Phase 6: NVS — две копии банка, CRC, wear leveling, атомарность.
#include "os/nvs/nvs.hpp"

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

class FaultStorage final : public ota::Storage {
public:
    explicit FaultStorage(ota::Storage& inner) noexcept : inner_(inner) {}
    uint32_t capacity() const noexcept override { return inner_.capacity(); }
    uint32_t page_size() const noexcept override { return inner_.page_size(); }
    uint32_t write_granularity() const noexcept override { return inner_.write_granularity(); }
    const char* name() const noexcept override { return "fault"; }
    Status erase(uint32_t off, uint32_t len) noexcept override { return inner_.erase(off, len); }
    Status write(uint32_t off, const void* data, uint32_t len) noexcept override {
        if (fail_after_ == 0u) return Status::HardwareError;
        if (fail_after_ != 0xFFFFFFFFu) --fail_after_;
        return inner_.write(off, data, len);
    }
    Status read(uint32_t off, void* data, uint32_t len) noexcept override {
        return inner_.read(off, data, len);
    }
    void fail_after(uint32_t n) noexcept { fail_after_ = n; }
private:
    ota::Storage& inner_;
    uint32_t fail_after_ = 0xFFFFFFFFu;
};

constexpr uint32_t kBank = 4096u;
uint8_t g_mem[2u * kBank]{};
} // namespace

int main() {
    ota::MemoryStorage mem;
    mem.attach(g_mem, sizeof(g_mem), false);
    nvs::Store store;
    nvs::Config cfg{&mem, kBank};
    check("init", store.init(cfg) == Status::Ok);
    check("empty", !store.contains("wifi_ssid"));

    const char* ssid = "MCFrame";
    check("set ssid", store.set("wifi_ssid", ssid, std::strlen(ssid)) == Status::Ok);
    char buf[64]{};
    Size len = 0u;
    check("get ssid", store.get("wifi_ssid", buf, sizeof(buf), len) == Status::Ok &&
                      len == std::strlen(ssid) && std::memcmp(buf, ssid, len) == 0);

    const char* pass = "secret123";
    check("set pass", store.set("wifi_pass", pass, std::strlen(pass)) == Status::Ok);
    check("overwrite", store.set("wifi_ssid", "C3", 2u) == Status::Ok);
    check("get overwrite", store.get("wifi_ssid", buf, sizeof(buf), len) == Status::Ok &&
                           len == 2u && buf[0] == 'C' && buf[1] == '3');
    check("erase", store.erase_key("wifi_pass") == Status::Ok);
    check("erased", !store.contains("wifi_pass"));

    // Заполняем банк так, чтобы сработала компакция.
    char key[8]{};
    char value[64]{};
    for (uint32_t i = 0u; i < 12u; ++i) {
        std::snprintf(key, sizeof(key), "k%u", static_cast<unsigned>(i));
        std::memset(value, static_cast<int>('a' + i), sizeof(value));
        check("fill", store.set(key, value, sizeof(value)) == Status::Ok);
    }
    check("count", store.key_count() == 13u);
    check("old survives compaction",
          store.get("wifi_ssid", buf, sizeof(buf), len) == Status::Ok && len == 2u);

    // Перезапуск: те же данные читаются из носителя.
    store.deinit();
    nvs::Store again;
    check("reinit", again.init(cfg) == Status::Ok);
    check("persist", again.contains("wifi_ssid") && again.contains("k11"));

    // Сбой питания на записи: новая запись не появляется, старая цела.
    FaultStorage fault(mem);
    nvs::Store power;
    check("power init", power.init({&fault, kBank}) == Status::Ok);
    fault.fail_after(0u);
    const Status failed = power.set("wifi_ssid", "lost", 4u);
    check("write rejected", failed != Status::Ok);
    power.deinit();

    nvs::Store recover;
    check("recover init", recover.init(cfg) == Status::Ok);
    check("old value intact", recover.get("wifi_ssid", buf, sizeof(buf), len) == Status::Ok &&
                              len == 2u && buf[0] == 'C');

    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
