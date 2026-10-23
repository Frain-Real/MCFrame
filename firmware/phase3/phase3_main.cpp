// firmware/phase3/phase3_main.cpp
// ---------------------------------------------------------------------------
// Phase 3 self-test: board::init(), LED, SPI transfer, PWM.
// Без внешнего loopback проверяется, что драйверы конфигурируют регистры и
// не зависают; реальный loopback — на плате (HIL).
// ---------------------------------------------------------------------------
#include "board.hpp"
#include "os/common.hpp"
#include "os/config/time_config.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/core/task/sheduler.hpp"

#include <cstdint>

namespace {

void semihost_exit(bool ok) noexcept {
    register uint32_t r0 __asm__("r0") = 0x18u;
    register uint32_t r1 __asm__("r1") = ok ? 0x20026u : 0x20023u;
    __asm__ volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
    for (;;) __asm__ volatile("nop");
}

void task(void*) noexcept {
    board::init_peripherals();
    board::led().set(true);

    embed::hal::SpiConfig sc{};
    sc.hz = 1000000u;
    sc.mode = 0u;
    bool ok = board::spi1().configure(sc) == embed::Status::Ok;

    uint8_t tx[2] = {0x9Fu, 0x00u};
    uint8_t rx[2]{};
    if (ok) ok = board::spi_cs().configure({embed::hal::GpioMode::Output, embed::hal::GpioPull::None, embed::hal::GpioSpeed::Low, true}) == embed::Status::Ok;
    if (ok) ok = board::spi1().transfer(tx, rx, 2u, 100u) == embed::Status::Ok;

    embed::hal::PwmConfig pc{};
    pc.frequency_hz = 1000u;
    pc.duty_percent = 50u;
    if (ok) ok = board::pwm1().configure(pc) == embed::Status::Ok;
    if (ok) ok = board::pwm1().start() == embed::Status::Ok;

    semihost_exit(ok);
}

embed::task g_task;
uint32_t g_stack[256];

} // namespace

int main() {
    if (embed::g_memory_manager.init(1024u, 128u) != embed::Status::Ok) return 1;
    embed::scheduler& s = embed::scheduler::instance();
    if (s.init() != embed::Status::Ok) return 1;
    if (g_task.init(&task, nullptr, "p3", 256u, 0u) != embed::Status::Ok) return 1;
    if (!s.add_task(&g_task, g_stack, 256u)) return 1;
    embed::config::OsTimer::Config cfg{};
    if (embed::config::OsTimer::Init(cfg) != embed::Status::Ok) return 1;
    if (embed::config::OsTimer::Start() != embed::Status::Ok) return 1;
    (void)s.start();
    return 1;
}
