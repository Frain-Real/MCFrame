// boards/common/shell_board.hpp
// ---------------------------------------------------------------------------
// Опциональные shell-команды платы: gpio/spi/pwm. Подключаются приложением,
// чтобы shell без них не тянул драйверы.
// ---------------------------------------------------------------------------
#pragma once

#include "board.hpp"
#include "os/shell/shell.hpp"

namespace board_shell {

inline bool eq(const char* a, const char* b) noexcept {
    if (a == nullptr || b == nullptr) return a == b;
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return *a == *b;
}

inline bool has_prefix(const char* s, const char* prefix) noexcept {
    if (s == nullptr || prefix == nullptr) return false;
    while (*prefix != '\0') {
        if (*s != *prefix) return false;
        ++s; ++prefix;
    }
    return true;
}

inline bool parse_u32(const char* s, uint32_t& out) noexcept {
    if (s == nullptr || *s == '\0') return false;
    uint32_t v = 0u;
    while (*s != '\0') {
        if (*s < '0' || *s > '9') return false;
        v = v * 10u + static_cast<uint32_t>(*s - '0');
        ++s;
    }
    out = v;
    return true;
}

inline void cmd_gpio(embed::shell::Console& c, const char* args) noexcept {
    if (args == nullptr) { c.write("usage: gpio led on|off|toggle | gpio cs on|off\n"); return; }
    if (has_prefix(args, "led")) {
        const char* v = args + 3;
        while (*v == ' ') ++v;
        if (eq(v, "on")) board::led().set(true);
        else if (eq(v, "off")) board::led().set(false);
        else if (eq(v, "toggle")) board::led().toggle();
        else { c.write("usage: gpio led on|off|toggle\n"); return; }
        c.write("led=");
        c.write(board::led().get() ? "on\n" : "off\n");
        return;
    }
    if (has_prefix(args, "cs")) {
        const char* v = args + 2;
        while (*v == ' ') ++v;
        if (eq(v, "on")) board::spi_cs().set(false);   // active low
        else if (eq(v, "off")) board::spi_cs().set(true);
        else { c.write("usage: gpio cs on|off\n"); return; }
        c.write("cs=");
        c.write(board::spi_cs().get() ? "high\n" : "low\n");
        return;
    }
    c.write("usage: gpio led|cs ...\n");
}

inline void cmd_spi(embed::shell::Console& c, const char* args) noexcept {
    uint32_t value = 0u;
    if (!parse_u32(args, value)) value = 0u;
    embed::hal::SpiConfig cfg{};
    cfg.hz = 1000000u;
    cfg.mode = 0u;
    if (board::spi1().configure(cfg) != embed::Status::Ok) {
        c.write("spi: configure failed\n");
        return;
    }
    const uint8_t tx = static_cast<uint8_t>(value);
    uint8_t rx = 0u;
    const embed::Status st = board::spi1().transfer(&tx, &rx, 1u, 100u);
    c.write("spi: tx=");
    c.write_hex(tx);
    c.write(" rx=");
    c.write_hex(rx);
    c.write(st == embed::Status::Ok ? " ok\n" : " error\n");
}

inline void cmd_pwm(embed::shell::Console& c, const char* args) noexcept {
    uint32_t duty = 50u;
    if (args != nullptr && *args != '\0' && !parse_u32(args, duty)) {
        c.write("usage: pwm [0..100]\n");
        return;
    }
    if (duty > 100u) { c.write("usage: pwm [0..100]\n"); return; }
    embed::hal::PwmConfig cfg{};
    cfg.frequency_hz = 1000u;
    cfg.duty_percent = static_cast<uint8_t>(duty);
    if (board::pwm1().configure(cfg) != embed::Status::Ok ||
        board::pwm1().start() != embed::Status::Ok) {
        c.write("pwm: failed\n");
        return;
    }
    c.write("pwm: duty=");
    c.write_u32(duty);
    c.write("%\n");
}

inline void add_board_commands(embed::shell::Console& c) noexcept {
    static const embed::shell::Command cmds[] = {
        {"gpio", "gpio led on|off|toggle | gpio cs on|off", &cmd_gpio},
        {"spi",  "spi [byte] — one SPI transfer", &cmd_spi},
        {"pwm",  "pwm [0..100] — set PWM duty", &cmd_pwm},
    };
    (void)c.add(cmds, 3u);
}

} // namespace board_shell
