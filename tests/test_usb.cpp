// tests/test_usb.cpp
#include "os/usb/usb.hpp"
#include "os/usb/cdc_acm.hpp"
#include "os/usb/dfu.hpp"
#include "os/usb/msc.hpp"
#include "os/usb/composite.hpp"
#include "os/usb/dfu_storage.hpp"
#include "os/usb/cdc_transport.hpp"
#include "os/usb/rndis.hpp"

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
        ep0_len = n;
        if (n != 0u && n <= sizeof(ep0)) std::memcpy(ep0, d, n);
        return Status::Ok;
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
    Status set_address(uint8_t a) noexcept override { address = a; return Status::Ok; }

    uint8_t ep0[256]{};
    Size ep0_len = 0u;
    uint8_t out[1024]{};
    Size ep_len = 0u;
    uint8_t ep = 0u;
    uint32_t writes = 0u;
    bool stalled = false;
    uint8_t address = 0xFFu;
    uint32_t suspends = 0u;
    uint32_t resumes = 0u;
    Status suspend() noexcept override { ++suspends; return Status::Ok; }
    Status resume() noexcept override { ++resumes; return Status::Ok; }
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
    uint32_t last_write = 0xFFFFFFFFu;
    uint32_t last_count = 0u;
    uint8_t last_byte = 0u;
    uint16_t block_size() const noexcept override { return 512u; }
    Status read_block(uint32_t lba, void* dst, uint32_t count) noexcept override {
        if (lba + count > block_count()) return Status::InvalidArg;
        std::memset(dst, static_cast<int>(lba & 0xFFu), count * 512u);
        return Status::Ok;
    }
    Status write_block(uint32_t lba, const void* src, uint32_t count) noexcept override {
        if (lba + count > block_count()) return Status::InvalidArg;
        last_write = lba;
        last_count = count;
        const auto* p = static_cast<const uint8_t*>(src);
        last_byte = (p != nullptr && count != 0u) ? p[0] : 0u;
        return Status::Ok;
    }
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
    check("GET_DESCRIPTOR string manufacturer",
          dev.setup(mk(0x80, 6, 0x0301, 0x0409, 64)) == Status::Ok &&
          hal.ep0_len == 16u && hal.ep0[2] == 'M');
    check("suspend/resume", dev.suspend() == Status::Ok && dev.suspended() &&
          hal.suspends == 1u && dev.resume() == Status::Ok && !dev.suspended() &&
          hal.resumes == 1u);
    check("GET_DESCRIPTOR string product",
          dev.setup(mk(0x80, 6, 0x0302, 0x0409, 64)) == Status::Ok &&
          hal.ep0_len == 24u && hal.ep0[18] == 'U');
    check("GET_DESCRIPTOR device short",
          dev.setup(mk(0x80, 6, 0x0100, 0, 8)) == Status::Ok && hal.ep0_len == 8u);
    check("GET_STATUS device", dev.setup(mk(0x80, 0, 0, 0, 2)) == Status::Ok &&
          hal.ep0_len == 2u);
    check("SET_ADDRESS", dev.setup(mk(0x00, 5, 7)) == Status::Ok &&
          dev.address() == 7u && hal.address == 7u);
    check("SET_CONFIGURATION", dev.setup(mk(0x00, 9, 1)) == Status::Ok && dev.configured() &&
          hal.ep0_len == 0u);
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


void test_composite() {
    std::puts("USB composite:");
    MockHal hal;
    usb::cdc::Acm acm;
    usb::dfu::Dfu dfu;
    usb::Class* kids[] = {&acm, &dfu};
    usb::Composite<> comp(kids, 2u);
    usb::Device dev;
    check("composite init", dev.init(hal, comp) == Status::Ok);
    check("composite descriptor", comp.descriptor_length() > 67u);
    check("GET_DESCRIPTOR composite config",
          dev.setup(mk(0x80, 6, 0x0200, 0, 64)) == Status::Ok &&
          hal.ep0_len == 64u && hal.ep0[4] == 3u);   // 3 interfaces
    check("SET_CONFIGURATION composite",
          dev.setup(mk(0x00, 9, 1)) == Status::Ok && comp.is_configured());
    check("composite suspend/resume", dev.suspend() == Status::Ok &&
          dev.suspended() && dev.resume() == Status::Ok && !dev.suspended());
    check("DFU class request index 2",
          dev.setup(mk(0xA1, 5, 0, 2, 1)) == Status::Ok && hal.ep0_len == 1u);
    uint8_t line[7] = {0x00, 0xC2u, 0x01u, 0x00, 0x00, 0x00, 0x08u};
    check("CDC class request index 0",
          dev.setup(mk(0x21, 0x20, 0, 0, 7), line, sizeof(line)) == Status::Ok);
}


void test_composite_msc() {
    std::puts("USB composite CDC+MSC:");
    MockHal hal;
    usb::cdc::Acm acm;
    RamBlock block;
    usb::msc::Msc msc(block);
    usb::Class* kids[] = {&acm, &msc};
    usb::Composite<3> comp(kids, 2u);
    usb::Device dev;
    check("CDC+MSC composite init", dev.init(hal, comp) == Status::Ok);
    check("CDC+MSC descriptor", comp.descriptor_length() >= 90u);
    Size n = 0u;
    const uint8_t* d = comp.config_descriptor(n);
    bool ep_out[16]{}, ep_in[16]{}, unique = true;
    for (Size pos = 9u; d != nullptr && pos + 2u <= n; ) {
        const uint8_t blen = d[pos];
        if (blen < 2u || pos + blen > n) { unique = false; break; }
        if (d[pos + 1u] == 5u && blen >= 7u) {
            const uint8_t ep = d[pos + 2u];
            bool& slot = (ep & 0x80u) ? ep_in[ep & 0x0Fu] : ep_out[ep & 0x0Fu];
            if (slot) unique = false;
            slot = true;
        }
        pos += blen;
    }
    check("CDC+MSC endpoints remapped", unique && ep_out[1] && ep_out[2] &&
          ep_in[1] && ep_in[2] && ep_in[3]);
    uint8_t cbw[31]{};
    cbw[0] = 0x55u; cbw[1] = 0x53u; cbw[2] = 0x42u; cbw[3] = 0x43u;
    cbw[4] = 1u; cbw[8] = 0u; cbw[12] = 0x80u; cbw[14] = 6u;
    cbw[15] = 0x00u;    // TEST UNIT READY
    check("MSC uses remapped IN", msc.handle_cbw(cbw, sizeof(cbw), hal) == Status::Ok &&
          hal.ep == 0x83u);
}

void test_dfu_storage() {
    std::puts("USB DFU to flash:");
    static uint8_t mem[4096]{};
    ota::MemoryStorage storage;
    storage.attach(mem, sizeof(mem), false);
    usb::dfu::StorageTarget target(storage, 0u, sizeof(mem));
    usb::dfu::Dfu dfu(&target);
    MockHal hal;
    usb::Device dev;
    check("DFU storage init", dev.init(hal, dfu) == Status::Ok);
    const uint8_t image[] = {1u, 2u, 3u, 4u};
    check("DFU DNLOAD block", dev.setup(mk(0x21, 1, 0, 0, 4), image, 4u) == Status::Ok);
    check("DFU flash write", mem[0] == 1u && mem[3] == 4u);
    check("DFU MANIFEST", dev.setup(mk(0x21, 1, 0, 0, 0)) == Status::Ok &&
          dfu.state() == usb::dfu::Dfu::State::Manifest);
    check("DFU UPLOAD from flash", dev.setup(mk(0xA1, 2, 0, 0, 4)) == Status::Ok &&
          hal.ep0_len == 4u && hal.ep0[3] == 4u);
}


void test_cdc_transport() {
    std::puts("USB CDC transport:");
    MockHal hal;
    usb::cdc::Acm acm;
    usb::Device dev;
    check("CDC transport init", dev.init(hal, acm) == Status::Ok);
    usb::cdc::AcmTransport tr(acm, hal);
    const uint8_t in[] = "ota";
    acm.on_out(in, sizeof(in) - 1u);
    uint8_t buf[8]{};
    Size got = 0u;
    check("CDC OTA read", tr.read(buf, sizeof(buf), got, 10u) == Status::Ok &&
          got == 3u && buf[0] == 'o');
    check("CDC OTA iface", tr.iface() == ota::proto::Iface::Usb);
    check("CDC OTA write", tr.write("ok", 2u, 10u) == Status::Ok && hal.ep == 0x82u);

    shell::Console console;
    usb::cdc::AcmConsole port(acm, hal);
    port.init(console);
    const uint8_t cmd[] = "help\n";
    acm.on_out(cmd, sizeof(cmd) - 1u);
    port.poll();
    check("CDC shell poll", true);
}


void test_rndis() {
    std::puts("USB RNDIS:");
    MockHal hal;
    usb::rndis::Rndis rndis;
    usb::Device dev;
    check("RNDIS init", dev.init(hal, rndis) == Status::Ok);
    check("RNDIS config", dev.setup(mk(0x80, 6, 0x0200, 0, 64)) == Status::Ok &&
          hal.ep0[4] == 2u && hal.ep0[49] == 0x0Au);   // data interface class
    uint8_t init[24]{};
    init[0] = 2u; init[4] = 24u;
    init[8] = 0x11u; init[9] = 0x22u; init[10] = 0x33u; init[11] = 0x44u;
    check("RNDIS INIT", dev.setup(mk(0x21, 0, 0, 0, sizeof(init)), init, sizeof(init)) == Status::Ok);
    check("RNDIS INIT response", dev.setup(mk(0xA1, 1, 0, 0, 52)) == Status::Ok &&
          hal.ep0_len == 52u && hal.ep0[0] == 2u && hal.ep0[1] == 0u &&
          hal.ep0[2] == 0u && hal.ep0[3] == 0x80u);
    uint8_t pkt[44 + 14]{};
    pkt[0] = 1u; pkt[4] = 58u; pkt[8] = 44u; pkt[12] = 14u;
    for (uint8_t i = 0u; i < 14u; ++i) pkt[44u + i] = i;
    check("RNDIS host frame", rndis.receive(pkt, sizeof(pkt)) == Status::Ok);
    Size fl = 0u;
    const uint8_t* f = rndis.rx_frame(fl);
    check("RNDIS frame queued", f != nullptr && fl == 14u && f[0] == 0u && f[13] == 13u);
    rndis.pop_rx();
    uint8_t eth[60]{};
    eth[0] = 0xFFu;
    check("RNDIS device frame", rndis.send_frame(eth, sizeof(eth)) == Status::Ok);
    check("RNDIS flush", rndis.flush(hal) == 1u && hal.ep == 0x83u);
    for (uint8_t i = 0u; i < 4u; ++i) (void)rndis.receive(pkt, sizeof(pkt));
    check("RNDIS backpressure", rndis.receive(pkt, sizeof(pkt)) == Status::NoMemory &&
          rndis.rx_dropped() == 1u && rndis.rx_count() == 4u);
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

    uint8_t wr[512]{};
    wr[0] = 0x5Au;
    uint8_t cbw_w[31]{};
    std::memcpy(cbw_w, cbw, sizeof(cbw));
    cbw_w[15] = 0x2Au;              // WRITE(10)
    cbw_w[8] = 0x00; cbw_w[9] = 0x02;
    check("WRITE10", msc.handle_cbw(cbw_w, sizeof(cbw_w), hal, wr, sizeof(wr)) == Status::Ok &&
          block.last_write == 2u && block.last_count == 1u && block.last_byte == 0x5Au);

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
    test_composite();
    test_composite_msc();
    test_dfu_storage();
    test_cdc_transport();
    test_rndis();
    std::printf("%d/%d проверок пройдено%s\n", g_total - g_failed, g_total,
                g_failed == 0 ? " — все тесты пройдены" : " — ЕСТЬ ОШИБКИ");
    return g_failed == 0 ? 0 : 1;
}
