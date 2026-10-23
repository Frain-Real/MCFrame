// Phase 6: NVS — две копии банка, CRC, wear leveling, атомарность.
#include "os/nvs/nvs.hpp"
#include "os/nvs/nvs_cipher.hpp"

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
uint8_t g_mem2[2u * kBank]{};
uint8_t g_mem3[2u * 8192u]{};
uint8_t g_mem4[2u * kBank]{};

bool contains_bytes(const uint8_t* hay, Size hay_len, const char* needle) {
    const Size n = std::strlen(needle);
    if (n == 0u || n > hay_len) return false;
    for (Size i = 0u; i + n <= hay_len; ++i) {
        if (std::memcmp(hay + i, needle, n) == 0) return true;
    }
    return false;
}

nvs::Store* g_migrate_store = nullptr;
Status migrate_add_key(void*, uint16_t from, uint16_t to) noexcept {
    if (from == 1u && to == 2u && g_migrate_store != nullptr) {
        return g_migrate_store->set("schema_added", "yes", 3u);
    }
    return Status::InvalidArg;
}
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


    // ── Namespaces: одинаковые имена не сталкиваются ──
    {
        ota::MemoryStorage mem2;
        mem2.attach(g_mem2, sizeof(g_mem2), false);
        nvs::Store ns;
        check("ns init", ns.init({&mem2, kBank}) == Status::Ok);
        nvs::Namespace wifi = ns.open_namespace("wifi");
        nvs::Namespace app  = ns.open_namespace("app");
        check("ns valid", wifi.valid() && app.valid());
        check("ns wifi set", wifi.set("ssid", "home", 4u) == Status::Ok);
        check("ns app set", app.set("ssid", "dev", 3u) == Status::Ok);
        char nb[16]{};
        Size nl = 0u;
        check("ns wifi get", wifi.get("ssid", nb, sizeof(nb), nl) == Status::Ok &&
                             nl == 4u && std::memcmp(nb, "home", 4u) == 0);
        check("ns app get", app.get("ssid", nb, sizeof(nb), nl) == Status::Ok &&
                            nl == 3u && std::memcmp(nb, "dev", 3u) == 0);
        check("ns does not leak to default", !ns.contains("ssid"));
        check("ns key count", ns.key_count() == 2u);
        check("bad namespace rejected", !ns.open_namespace("bad/name").valid());
        check("empty namespace is default", ns.open_namespace("").valid());
    }

    // ── Blob больше 64 байт и 64 ключа ──
    {
        ota::MemoryStorage mem3;
        mem3.attach(g_mem3, sizeof(g_mem3), false);
        nvs::Store blobs;
        check("blob init", blobs.init({&mem3, 8192u}) == Status::Ok);
        uint8_t blob[300]{};
        for (uint32_t i = 0u; i < sizeof(blob); ++i) blob[i] = static_cast<uint8_t>(i * 7u);
        check("blob set", blobs.set_blob("fw/blob", blob, sizeof(blob)) == Status::Ok);
        uint8_t out[300]{};
        Size out_len = 0u;
        check("blob get", blobs.get_blob("fw/blob", out, sizeof(out), out_len) == Status::Ok &&
                          out_len == sizeof(blob) && std::memcmp(out, blob, sizeof(blob)) == 0);
        uint8_t small[100]{};
        check("blob too small buffer", blobs.get_blob("fw/blob", small, sizeof(small), out_len) ==
                                       Status::NoMemory);

        char k[8]{};
        char v[8]{};
        bool all_ok = true;
        for (uint32_t i = 0u; i < 63u; ++i) {
            std::snprintf(k, sizeof(k), "k%02u", static_cast<unsigned>(i));
            std::snprintf(v, sizeof(v), "v%02u", static_cast<unsigned>(i));
            if (blobs.set(k, v, std::strlen(v)) != Status::Ok) all_ok = false;
        }
        check("63 keys set", all_ok);
        check("keys count (blob + 63)", blobs.key_count() == 64u);
        check("last key readable", blobs.contains("k62"));
    }

    // ── Schema version + миграция ──
    {
        ota::MemoryStorage mem4;
        mem4.attach(g_mem4, sizeof(g_mem4), false);
        nvs::Store v1;
        nvs::Config c1{&mem4, kBank};
        c1.schema_version = 1u;
        check("schema v1 init", v1.init(c1) == Status::Ok);
        check("schema v1 set", v1.set("old_key", "old", 3u) == Status::Ok);
        check("schema v1 value", v1.schema_version() == 1u);
        v1.deinit();

        nvs::Store v2;
        g_migrate_store = &v2;
        nvs::Config c2{&mem4, kBank};
        c2.schema_version = 2u;
        c2.migrate = &migrate_add_key;
        check("schema v2 migrate", v2.init(c2) == Status::Ok);
        check("schema v2 version", v2.schema_version() == 2u);
        check("schema old survives", v2.contains("old_key"));
        check("schema added key", v2.contains("schema_added"));
        v2.deinit();

        // Без callback понижение/повышение схемы обязано быть явной ошибкой.
        nvs::Store v3;
        nvs::Config c3{&mem4, kBank};
        c3.schema_version = 3u;
        check("schema without migrate rejected",
              v3.init(c3) == Status::NotSupported);
        v3.deinit();
    }

    // ── Шифрование: открытый текст не лежит в носителе ──
    {
        ota::MemoryStorage mem5;
        static uint8_t mem5_data[2u * kBank]{};
        mem5.attach(mem5_data, sizeof(mem5_data), false);

        uint8_t key[32]{};
        for (uint32_t i = 0u; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(i + 1u);
        nvs::AesCtrHmac cipher(key);

        nvs::Config ecfg{&mem5, kBank};
        ecfg.cipher = &cipher;
        nvs::Store enc;
        check("enc init", enc.init(ecfg) == Status::Ok);
        const char* secret = "TOP-SECRET-123";
        check("enc set", enc.set("secret", secret, std::strlen(secret)) == Status::Ok);
        char ebuf[32]{};
        Size elen = 0u;
        check("enc get", enc.get("secret", ebuf, sizeof(ebuf), elen) == Status::Ok &&
                         elen == std::strlen(secret) &&
                         std::memcmp(ebuf, secret, elen) == 0);
        check("enc plaintext absent", !contains_bytes(mem5_data, sizeof(mem5_data), secret));
        check("enc empty set", enc.set("empty", nullptr, 0u) == Status::Ok);
        check("enc empty get", enc.get("empty", ebuf, sizeof(ebuf), elen) == Status::Ok &&
                               elen == 0u);
        enc.deinit();

        nvs::Store enc2;
        check("enc reinit", enc2.init(ecfg) == Status::Ok);
        check("enc persists", enc2.get("secret", ebuf, sizeof(ebuf), elen) == Status::Ok &&
                              elen == std::strlen(secret));
        enc2.deinit();

        uint8_t wrong_key[32]{};
        for (uint32_t i = 0u; i < sizeof(wrong_key); ++i) wrong_key[i] = static_cast<uint8_t>(0xA0u + i);
        nvs::AesCtrHmac wrong(wrong_key);
        nvs::Config wcfg{&mem5, kBank};
        wcfg.cipher = &wrong;
        nvs::Store bad;
        check("enc wrong key init", bad.init(wcfg) == Status::Ok);
        check("enc wrong key rejected",
              bad.get("secret", ebuf, sizeof(ebuf), elen) == Status::Corruption);
        bad.deinit();
    }

    // ── InternalFlashStorage: тот же путь, что у board::nvs() ──
    {
        ota::InternalFlashStorage intflash(120u * 1024u, 8u * 1024u);
        nvs::Store flash_store;
        check("internal flash init", flash_store.init({&intflash, 4096u}) == Status::Ok);
        check("internal flash set", flash_store.set("cfg", "value", 5u) == Status::Ok);
        char fb[16]{};
        Size fl = 0u;
        check("internal flash get", flash_store.get("cfg", fb, sizeof(fb), fl) == Status::Ok &&
                                    fl == 5u && std::memcmp(fb, "value", 5u) == 0);
        flash_store.deinit();

        nvs::Store flash_again;
        check("internal flash reinit", flash_again.init({&intflash, 4096u}) == Status::Ok);
        check("internal flash persists", flash_again.contains("cfg"));
        flash_again.deinit();
    }

    // ── FlashSectorStorage (F4: два 16-КБ банка в отдельных секторах) ──
    {
        nvs::FlashSectorStorage sector{0u, 32u * 1024u, 16u * 1024u};
        nvs::Store s;
        check("sector init", s.init({&sector, 16u * 1024u}) == Status::Ok);
        check("sector set", s.set("f4", "nvs", 3u) == Status::Ok);
        char b[8]{};
        Size n = 0u;
        check("sector get", s.get("f4", b, sizeof(b), n) == Status::Ok &&
                            n == 3u && std::memcmp(b, "nvs", 3u) == 0);
        s.deinit();
        nvs::Store s2;
        check("sector reinit", s2.init({&sector, 16u * 1024u}) == Status::Ok);
        check("sector persists", s2.contains("f4"));
        s2.deinit();
    }

    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
