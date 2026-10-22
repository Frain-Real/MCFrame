// shell_watchdog.hpp
// ---------------------------------------------------------------------------
// Опциональные команды сторожа: подключаются приложением, чтобы shell без
// watchdog не тянул его код в образ.
// ---------------------------------------------------------------------------
#pragma once

#include "shell.hpp"
#include "../watchdog/watchdog.hpp"

namespace embed::shell {

inline void cmd_wd(Console& c, const char* /*args*/) noexcept {
    auto& svc = watchdog::Service::instance();
    const watchdog::Stats st = svc.stats();

    c.write("participants=");
    c.write_u32(st.participants);
    c.write(" hardware=");
    c.write(st.hardware ? "yes" : "no");
    c.write("\nchecks=");
    c.write_u32(st.checks);
    c.write(" kicks=");
    c.write_u32(st.kicks);
    c.write(" misses=");
    c.write_u32(st.misses);
    c.write(" warnings=");
    c.write_u32(st.warnings);
    c.write("\nstalls=");
    c.write_u32(st.stalls);
    c.write(" refused=");
    c.write_u32(st.refused_stalls);
    c.write(" max_late_ms=");
    c.write_u32(st.max_late_ms);
    c.write(" slept_ms=");
    c.write_u32(st.slept_ms);
    c.write("\n");

    const watchdog::Fault f = svc.last_fault();
    if (f.valid) {
        c.write("last fault: participant=");
        c.write_u32(f.participant);
        c.write(" late_ms=");
        c.write_u32(f.late_ms);
        c.write(" boot=");
        c.write_u32(f.boot_count);
        c.write("\n");
    }
}

inline void add_watchdog_commands(Console& c) noexcept {
    static const Command cmds[] = {
        {"wd", "watchdog participants and last fault", &cmd_wd},
    };
    (void)c.add(cmds, 1u);
}

} // namespace embed::shell
