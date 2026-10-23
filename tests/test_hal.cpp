// tests/test_hal.cpp
// ---------------------------------------------------------------------------
// Phase 1: HAL-интерфейсы, mock-реализации, SpiDevice, board facade и
// kernel API. Тест host-only: он не требует STM32 и не трогает реальные
// регистры.
// ---------------------------------------------------------------------------
#include "board.hpp"
#include "hal/SpiDevice.hpp"
#include "hal/mock/mock_hal.hpp"
#include "os/kernel/kernel_api.hpp"
#include "arch/armv7m/frame.hpp"

#include <cstdio>
#include <type_traits>

namespace {

int g_failed = 0;
int g_total  = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

struct NetCtx {
    int connected = 0;
    int got_ip = 0;
};

void net_cb(void* ctx, const embed::hal::NetEvent& e) {
    auto* c = static_cast<NetCtx*>(ctx);
    if (e.kind == embed::hal::NetEvent::Kind::Connected) ++c->connected;
    if (e.kind == embed::hal::NetEvent::Kind::GotIp) ++c->got_ip;
}

struct BleCtx {
    int notified = 0;
};

void ble_cb(void* ctx, const embed::hal::BleEvent& e) {
    auto* c = static_cast<BleCtx*>(ctx);
    if (e.kind == embed::hal::BleEvent::Kind::NotificationSent) ++c->notified;
}

void test_gpio() {
    (void)std::puts("gpio:");
    embed::hal::mock::MockGpioPin led;
    embed::hal::GpioConfig cfg{};
    cfg.mode = embed::hal::GpioMode::Output;
    cfg.initial = false;
    check("configure", led.configure(cfg) == embed::Status::Ok);
    check("configured", led.configured());
    led.set(true);
    check("set/get", led.get());
    led.toggle();
    check("toggle", !led.get());

    int irq = 0;
    auto cb = [](void* ctx) { ++*static_cast<int*>(ctx); };
    check("attach_irq", led.attach_irq(embed::hal::GpioEdge::Rising, cb, &irq) == embed::Status::Ok);
    led.trigger();
    check("irq callback", irq == 1);
    led.detach_irq();
    led.trigger();
    check("detach_irq", irq == 1);
}

void test_spi() {
    (void)std::puts("spi:");
    embed::hal::mock::MockSpi bus;
    embed::hal::SpiConfig cfg{};
    cfg.mode = 0u;
    cfg.hz = 1'000'000u;
    check("configure", bus.configure(cfg) == embed::Status::Ok);
    const uint8_t tx[3] = {0x01u, 0x02u, 0x03u};
    uint8_t rx[3]{};
    check("transfer", bus.transfer(tx, rx, 3u, 10u) == embed::Status::Ok);
    check("loopback", rx[0] == 1u && rx[1] == 2u && rx[2] == 3u);
    check("caps", (bus.caps() & embed::hal::SpiCapDma) != 0u);

    embed::hal::mock::MockGpioPin cs;
    embed::hal::GpioConfig cs_cfg{};
    cs_cfg.mode = embed::hal::GpioMode::Output;
    cs_cfg.initial = true;
    (void)cs.configure(cs_cfg);

    embed::hal::SpiDevice dev(bus, cs, true);
    const uint8_t cmd[1] = {0x9Fu};
    uint8_t id[1]{};
    check("SpiDevice transfer", dev.transfer(cmd, id, 1u, 10u) == embed::Status::Ok);
    check("SpiDevice CS released", cs.get());
    check("SpiDevice lock", bus.locks() == 1u);
    check("SpiDevice unlock", bus.unlocks() == 1u);
    check("SpiDevice id", id[0] == 0x9Fu);
}

void test_pwm() {
    (void)std::puts("pwm:");
    embed::hal::mock::MockPwm pwm;
    embed::hal::PwmConfig cfg{};
    cfg.frequency_hz = 1000u;
    cfg.duty_percent = 25u;
    check("configure", pwm.configure(cfg) == embed::Status::Ok);
    check("duty", pwm.duty() == 25u);
    check("start", pwm.start() == embed::Status::Ok && pwm.started());
    check("set_duty", pwm.set_duty(75u) == embed::Status::Ok && pwm.duty() == 75u);
    check("stop", pwm.stop() == embed::Status::Ok && !pwm.started());
}

void test_uart_i2c_adc() {
    (void)std::puts("uart/i2c/adc:");
    embed::hal::mock::MockUart uart;
    embed::hal::UartConfig cfg{};
    check("uart configure", uart.configure(cfg) == embed::Status::Ok);
    check("uart open", uart.open() == embed::Status::Ok);
    const char msg[] = "hi";
    embed::Size written = 0u;
    check("uart write", uart.write(msg, 2u, written, 10u) == embed::Status::Ok && written == 2u);
    check("uart tx", uart.tx_size() == 2u && uart.tx_at(0u) == 'h');
    const char in[] = "rx";
    uart.push_rx(in, 2u);
    char buf[4]{};
    embed::Size got = 0u;
    check("uart read", uart.read(buf, 4u, got, 10u) == embed::Status::Ok && got == 2u);
    check("uart rx", buf[0] == 'r' && buf[1] == 'x');

    embed::hal::mock::MockI2c i2c;
    embed::hal::I2cConfig icfg{};
    check("i2c configure", i2c.configure(icfg) == embed::Status::Ok);
    const uint8_t data[2] = {0xAAu, 0x55u};
    check("i2c write", i2c.write(0x50u, data, 2u, 10u) == embed::Status::Ok);
    uint8_t out[2]{};
    embed::Size n = 0u;
    check("i2c read", i2c.read(0x50u, out, 2u, n, 10u) == embed::Status::Ok && n == 2u);
    check("i2c data", out[0] == 0xAAu && out[1] == 0x55u);

    embed::hal::mock::MockAdc adc;
    embed::hal::AdcConfig acfg{};
    check("adc configure", adc.configure(acfg) == embed::Status::Ok);
    uint16_t value = 0u;
    check("adc read", adc.read(3u, value) == embed::Status::Ok && value == 300u);
}

void test_storage() {
    (void)std::puts("flash/nvs:");
    embed::hal::mock::MockFlash flash;
    const uint8_t data[2] = {0x12u, 0x34u};
    check("flash write", flash.write(0u, data, 2u) == embed::Status::Ok);
    uint8_t out[2]{};
    check("flash read", flash.read(0u, out, 2u) == embed::Status::Ok);
    check("flash data", out[0] == 0x12u && out[1] == 0x34u);
    check("flash erase", flash.erase(0u, 2u) == embed::Status::Ok);
    (void)flash.read(0u, out, 2u);
    check("flash erased", out[0] == 0xFFu && out[1] == 0xFFu);

    embed::hal::mock::MockNvs nvs;
    check("nvs init", nvs.init() == embed::Status::Ok);
    const char value[] = "mcframe";
    check("nvs write", nvs.write("name", value, 8u) == embed::Status::Ok);
    char read[8]{};
    embed::Size len = 0u;
    check("nvs read", nvs.read("name", read, sizeof(read), len) == embed::Status::Ok && len == 8u);
    check("nvs data", read[0] == 'm');
    check("nvs erase", nvs.erase("name") == embed::Status::Ok);
    check("nvs missing", nvs.read("name", read, sizeof(read), len) == embed::Status::NotFound);
}

void test_network_ble() {
    (void)std::puts("network/ble:");
    embed::hal::mock::MockNetwork net;
    embed::hal::NetworkConfig cfg{};
    cfg.ssid = "test";
    cfg.password = "secret";
    check("net init", net.init(cfg) == embed::Status::Ok);
    NetCtx nctx{};
    check("net subscribe", net.subscribe(&net_cb, &nctx) == embed::Status::Ok);
    check("net connect", net.connect() == embed::Status::Ok);
    check("net state", net.state() == embed::hal::NetState::Connected);
    check("net events", nctx.connected == 1 && nctx.got_ip == 1);
    check("net disconnect", net.disconnect() == embed::Status::Ok);
    check("net idle", net.state() == embed::hal::NetState::Idle);

    embed::hal::mock::MockBle ble;
    embed::hal::BleConfig bcfg{};
    bcfg.name = "mcframe";
    check("ble init", ble.init(bcfg) == embed::Status::Ok);
    const uint8_t adv[] = {0x02u, 0x01u, 0x06u};
    check("ble advertise", ble.advertise({adv, 3u}) == embed::Status::Ok && ble.advertising());
    const embed::hal::BleUuid uuid{};
    check("ble service", ble.add_service(uuid) == embed::Status::Ok && ble.has_service());
    embed::hal::BleGattService gs{};
    gs.count = 1u; gs.chars[0].properties = 3u;
    check("ble gatt server", ble.add_gatt_service(gs) == embed::Status::Ok &&
          ble.gatt_chars() == 1u);
    check("ble gatt value", ble.set_value(7u, "v", 1u) == embed::Status::Ok &&
          ble.value_handle() == 7u && ble.value_len() == 1u);
    BleCtx bctx{};
    check("ble subscribe", ble.subscribe(&ble_cb, &bctx) == embed::Status::Ok);
    const uint8_t value[] = {0x2Au};
    check("ble notify", ble.notify(1u, 2u, value, 1u) == embed::Status::Ok);
    check("ble event", bctx.notified == 1 && ble.last_conn() == 1u && ble.last_attr() == 2u);
}

void frame_fn(void*) noexcept {}

void test_arch_frame() {
    (void)std::puts("arch frame:");
    uint32_t stack[64]{};
    static int marker = 0;
    void (*fn)(void*) = &frame_fn;
    uint32_t* base = &stack[64];

    uint32_t* sp = embed::arch::armv7m::build_task_frame(base, fn, &marker, true);
    check("FPU frame size", (base - sp) == 50u);
    check("FPU R4", sp[0] == 0x04040404u);
    check("FPU R11", sp[7] == 0x11111111u);
    check("FPU S16..S31 zero", sp[8] == 0u && sp[23] == 0u);
    check("FPU S0..S15 zero", sp[24] == 0u && sp[39] == 0u);
    check("FPU FPSCR/reserved", sp[40] == 0u && sp[41] == 0u);
    check("FPU R0 ctx", sp[42] == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&marker)));
    check("FPU PC", sp[48] == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn)));
    check("FPU xPSR Thumb", sp[49] == 0x01000000u);

    uint32_t* np = embed::arch::armv7m::build_task_frame(base, fn, &marker, false);
    check("no-FPU frame size", (base - np) == 16u);
    check("no-FPU R4", np[0] == 0x04040404u);
    check("no-FPU R0 ctx", np[8] == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&marker)));
    check("no-FPU PC", np[14] == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn)));
    check("no-FPU xPSR", np[15] == 0x01000000u);
}

void test_board_and_kernel() {
    (void)std::puts("board/kernel:");
    board::init();
    board::led().set(true);
    check("board led", board::led().get());
    const char text[] = "ok";
    embed::Size n = 0u;
    check("board console", board::console().write(text, 2u, n, 10u) == embed::Status::Ok);

    embed::hal::SpiConfig scfg{};
    check("board spi", board::spi1().configure(scfg) == embed::Status::Ok);
    embed::hal::PwmConfig pcfg{};
    check("board pwm", board::pwm1().configure(pcfg) == embed::Status::Ok);

    static_assert(std::is_same<embed::kernel::task, embed::task>::value,
                  "kernel::task must alias backend task");
    static_assert(std::is_same<embed::kernel::mutex, embed::mutex>::value,
                  "kernel::mutex must alias backend mutex");
    embed::kernel::message_queue<uint32_t, 2> q;
    check("kernel queue", q.push(1u) == embed::Status::Ok);
    embed::kernel::semaphore<> sem;
    check("kernel semaphore", sem.value() == 0u);
    embed::kernel::event_flags ef;
    check("kernel event flags", ef.value() == 0u);
}

} // namespace

int main() {
    (void)std::puts("=== MCFrame Phase 1 HAL tests ===");
    test_gpio();
    test_spi();
    test_pwm();
    test_uart_i2c_adc();
    test_storage();
    test_network_ble();
    test_arch_frame();
    test_board_and_kernel();
    (void)std::printf("\n%d/%d проверок пройдено", g_total - g_failed, g_total);
    if (g_failed == 0) (void)std::printf(" — все тесты пройдены\n");
    else               (void)std::printf(" — ЕСТЬ ОШИБКИ\n");
    return g_failed == 0 ? 0 : 1;
}
