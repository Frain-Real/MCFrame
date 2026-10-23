// tests/test_usb.cpp
#include "os/usb/usb.hpp"
#include "os/usb/cdc_acm.hpp"
#include "os/usb/dfu.hpp"
#include "os/usb/msc.hpp"

#include <cstdio>
#include <cstring>

using namespace embed;

namespace {
int g_total = 0;
int g_failed = 0;
void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

class MockHal final : public usb::Hal {
public:
    Status ep0_write(const void* d, Size n) noexcept override {
        ep0_len = n; if (n <= sizeof(ep0)) std::memcpy(ep0, d, n); return Status::Ok;
    }
    Status ep0_read(void* d, Size cap, Size& got) noexcept override {
        (void)d; (void)cap; got = 0u; return Status::NotSupported;
    }
    Status ep_write(uint8_t e, const void* d, Size n) noexcept override {
        ep = e; ep_len = n; if (n <= sizeof(out)) std::memcpy(out, d, n); ++writes;
        return Status::Ok;
    }
    Status ep_read(uint8_t, void*, Size, Size& got) noexcept override {
        got = 0u; return Status::NotSupported;
    }
    void stall_ep0() noexcept override { stalled = true; }

    uint8_t ep0[256]{};
    Size ep0_len = 0u;
    uint8_t out[1024]{};
    Size ep_len = 0u;
    uint8_t ep = 0u;
    uint32_t writes = 0u;
    bool stalled = false;
};

usb::Setup mk(uint8_t type, uint8_t req, uint16_t value = 0u,
              uint16_t index = 0u, uint16_t len = 0u) {
    usb::Setup s{};
    s.type = type; s.request = req; s.value = value; s.index = index; s.length = len;
    return s;
}

class RamBlock final : public usb::msc::BlockDevice {
public:
    uint32_t block_count() const noexcept override { return 128u; }
    uint16_t block_size() const noexcept override { return 512u; }
    Status read_block(uint32_t lba, void* dst, uint32_t count) noexcept override {
        if (lba + count > block_count()) return Status::InvalidArg;
        std::memset(dst, static_cast<int>(lba & 0xFFu), count * 512u);
        return Status::Ok;
    }
    Status write_block(uint32_t, const void*, uint32_t) noexcept override { return Status::NotSupported; }
};

void test_core_cdc() {
    std::puts("USB core + CDC-ACM:");
    MockHal hal;
    usb::cdc::Acm acm;
    usb::Device dev;
    check("init", dev.init(hal, acm) == Status::Ok);
    check("GET_DESCRIPTOR device",
          dev.setup(mk(0x80, 6, 0x0100, 0, 18)) == Status::Ok && hal.ep0_len == 18u &&
          hal.ep0[0] == 18u && hal.ep0[1] == 1u);
    check("GET_DESCRIPTOR config",
          dev.setup(mk(0x80, 6, 0x0200, 0, 67)) == Status::Ok && hal.ep0_len == 67u &&
          hal.ep0[1] == 2u && hal.ep0[2] == 67u);
    check("SET_CONFIGURATION", dev.setup(mk(0x00, 9, 1)) == Status::Ok && dev.configured());
    check("class configured", acm.is_configured());

    uint8_t line[7] = {0x00, 0xC2u, 0x01u, 0x00, 0x00, 0x00, 0x08u};
    check("SET_LINE_CODING",
          dev.setup(mk(0x21, 0x20, 0, 0, 7), line, sizeof(line)) == Status::Ok);
    check("GET_LINE_CODING",
          dev.setup(mk(0xA1, 0x21, 0, 0, 7)) == Status::Ok && hal.ep0_len == 7u &&
          std::memcmp(hal.ep0, line, 7) == 0);

    const uint8_t in[] = "ping";
    acm.on_out(in, sizeof(in) - 1u);
    char buf[8]{};
    const Size n = acm.read(buf, sizeof(buf));
    check("CDC OUT->read", n == 4u && std::memcmp(buf, "ping", 4) == 0);

    Size sent = 0u;
    check("CDC write", acm.write(hal, "pong", 4u, sent) == Status::Ok && sent == 4u &&
          hal.ep == usb::cdc::Acm::kEpIn && hal.ep_len == 4u);
}

void test_dfu() {
    std::puts("USB DFU:");
    MockHal hal;
    usb::dfu::Dfu dfu;
    usb::Device dev;
    check("init", dev.init(hal, dfu) == Status::Ok);
    check("GET_DESCRIPTOR dfu config",
          dev.setup(mk(0x80, 6, 0x0200, 0, 18)) == Status::Ok && hal.ep0_len == 18u);

    const uint8_t image[] = {1u, 2u, 3u, 4u};
    check("DNLOAD", dev.setup(mk(0x21, 1, 0, 0, 4), image, sizeof(image)) == Status::Ok);
    check("size", dfu.size() == 4u);
    check("GETSTATE",
          dev.setup(mk(0xA1, 5, 0, 0, 1)) == Status::Ok && hal.ep0_len == 1u &&
          hal.ep0[0] == static_cast<uint8_t>(usb::dfu::Dfu::State::DfuIdle));
    check("GETSTATUS",
          dev.setup(mk(0xA1, 3, 0, 0, 6)) == Status::Ok && hal.ep0_len == 6u);
    check("UPLOAD",
          dev.setup(mk(0xA1, 2, 0, 0, 4)) == Status::Ok && hal.ep0_len == 4u &&
          std::memcmp(hal.ep0, image, 4) == 0);
    check("ABORT", dev.setup(mk(0x21, 6)) == Status::Ok);
}

void test_msc() {
    std::puts("USB MSC:");
    MockHal hal;
    RamBlock block;
    usb::msc::Msc msc(block);
    usb::Device dev;
    check("init", dev.init(hal, msc) == Status::Ok);
    check("GET_DESCRIPTOR config",
          dev.setup(mk(0x80, 6, 0x0200, 0, 32)) == Status::Ok && hal.ep0_len == 32u);
    check("GET_MAX_LUN", dev.setup(mk(0xA1, 0xFE, 0, 0, 1)) == Status::Ok &&
          hal.ep0_len == 1u && hal.ep0[0] == 0u);

    uint8_t cbw[31]{};
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C'; // 0x43425355 LE
    cbw[4] = 0x11; cbw[5] = 0x22; cbw[6] = 0x33; cbw[7] = 0x44;
    cbw[8] = 0x00; cbw[9] = 0x02; // 512 bytes
    cbw[12] = 0x80; cbw[13] = 0; cbw[14] = 10;
    cbw[15] = 0x28; // READ(10)
    cbw[17] = 0; cbw[18] = 0; cbw[19] = 0; cbw[20] = 2; // LBA 2
    cbw[22] = 0; cbw[23] = 1; // 1 block
    check("READ10", msc.handle_cbw(cbw, sizeof(cbw), hal) == Status::Ok);
    check("data written", hal.writes >= 2u);
    check("data/CSW", hal.ep_len == 13u && hal.out[0] == 'U' && hal.out[1] == 'S');

    uint8_t cbw2[31]{};
    std::memcpy(cbw2, cbw, sizeof(cbw));
    cbw2[15] = 0x12; // INQUIRY
    cbw2[8] = 36; cbw2[9] = 0; cbw2[10] = 0; cbw2[11] = 0;
    hal.writes = 0u;
    check("INQUIRY", msc.handle_cbw(cbw2, sizeof(cbw2), hal) == Status::Ok);
    check("INQUIRY data", hal.writes >= 2u);
}

} // namespace

int main() {
    std::puts("=== USB device ===");
    test_core_cdc();
    test_dfu();
    test_msc();
    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
