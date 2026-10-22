// shell_builtins.cpp
#if defined(__GNUC__)
#pragma GCC optimize("Os")
#endif
#include "shell.hpp"

#include "../core/memory/memory_manager.hpp"
#include "../core/task/sheduler.hpp"
#include "../log/log.hpp"
#include "../power/power.hpp"

#include <cstdint>

namespace {

using embed::Size;
using embed::Status;
using embed::TaskHandler;
using embed::task_state;

char state_char(task_state st) noexcept {
    switch (st) {
        case task_state::idle:      return 'I';
        case task_state::ready:     return 'R';
        case task_state::running:   return 'r';
        case task_state::suspended: return 'S';
        case task_state::sleeping:  return 's';
        case task_state::blocked:   return 'B';
        case task_state::done:      return 'D';
        default:                    return '?';
    }
}

void cmd_help(embed::shell::Console& c, const char* /*args*/) noexcept {
    c.write("commands:\n");
    c.help();
}

void cmd_ps(embed::shell::Console& c, const char* /*args*/) noexcept {
    embed::scheduler& s = embed::scheduler::instance();

    TaskHandler tasks[embed::kMaxTasks + 1u]{};
    const Size n = s.task_snapshot(tasks, embed::kMaxTasks + 1u);

    const uint64_t total_ticks = s.tick_count();
    c.write("name       st prio/base ticks       cpu% sw  stack_free\n");
    for (Size i = 0u; i < n; ++i) {
        TaskHandler t = tasks[i];
        if (t == nullptr) continue;

        uint64_t runtime = 0u;
        uint32_t switches = 0u;
        {
            // 64-битный счётчик читаем под Lock: тик пишет его в ISR.
            embed::Lock l;
            runtime = t->runtime_ticks();
            switches = t->switches();
        }
        const uint32_t cpu = (total_ticks != 0u)
            ? static_cast<uint32_t>((runtime * 100u) / total_ticks)
            : 0u;

        c.write(t->name() != nullptr ? t->name() : "?");
        c.write(" ");
        char st[2] = { state_char(t->state()), '\0' };
        c.write(st);
        c.write(" ");
        c.write_u32(t->priority());
        c.write("/");
        c.write_u32(t->base_priority());
        c.write(" ");
        c.write_u64(runtime);
        c.write(" ");
        c.write_u32(cpu);
        c.write(" ");
        c.write_u32(switches);
        c.write(" ");
        c.write_u32(static_cast<uint32_t>(
            embed::g_memory_manager.task_stack_high_water(t)));
        c.write("\n");
    }
}

void cmd_free(embed::shell::Console& c, const char* /*args*/) noexcept {
    using embed::g_memory_manager;
    c.write("heap:  total=");
    c.write_u32(static_cast<uint32_t>(g_memory_manager.heap_total()));
    c.write(" free=");
    c.write_u32(static_cast<uint32_t>(g_memory_manager.heap_free_bytes()));
    c.write(" used=");
    c.write_u32(static_cast<uint32_t>(g_memory_manager.heap_used()));
    c.write("\nstack: total=");
    c.write_u32(static_cast<uint32_t>(g_memory_manager.stack_pool_total()));
    c.write(" used=");
    c.write_u32(static_cast<uint32_t>(g_memory_manager.stack_pool_used()));
    c.write("\ntasks: ");
    c.write_u32(static_cast<uint32_t>(embed::scheduler::instance().task_count()));
    c.write("\n");
}

void cmd_stats(embed::shell::Console& c, const char* /*args*/) noexcept {
    using embed::power::Supervisor;
    embed::scheduler& s = embed::scheduler::instance();

    c.write("ticks=");
    c.write_u64(s.tick_count());
    c.write(" switches=");
    c.write_u32(static_cast<uint32_t>(s.switch_requests()));
    c.write("\n");

    const auto st = Supervisor::instance().stats();
    c.write("load=");
    c.write_u32(st.load_percent);
    c.write("% peak=");
    c.write_u32(st.peak_load_percent);
    c.write("% avg=");
    c.write_u32(st.avg_load_percent);
    c.write("% wfi=");
    c.write_u32(st.sleep_entries);
    c.write(" stop=");
    c.write_u32(st.deep_sleep_entries);
    c.write(" stop_ms=");
    c.write_u32(st.deep_sleep_ms);
    c.write("\n");
}

void cmd_log(embed::shell::Console& c, const char* /*args*/) noexcept {
    embed::log::Entry e{};
    bool any = false;
    while (embed::log::pop(e)) {
        any = true;
        c.write("[");
        c.write(embed::log::level_name(e.level));
        c.write("] ");
        c.write(e.tag);
        c.write(": ");
        c.write(e.text);
        c.write("\n");
    }
    if (!any) c.write("log: empty\n");
}

void cmd_reboot(embed::shell::Console& c, const char* /*args*/) noexcept {
    const embed::platform::Ops& ops = embed::platform::ops();
    if (ops.reset == nullptr) {
        c.write("reboot: not supported\n");
        return;
    }
    c.write("rebooting...\n");
    ops.reset();
    for (;;) { }
}

} // namespace

namespace embed::shell {

namespace {
const Command kBuiltins[] = {
    {"help",   "list commands",                 &cmd_help},
    {"ps",     "tasks: state, prio, CPU, stack", &cmd_ps},
    {"free",   "heap/stack pool usage",         &cmd_free},
    {"stats",  "scheduler and power statistics", &cmd_stats},
    {"log",    "dump recent log ring",          &cmd_log},
    {"reboot", "reset the MCU",                 &cmd_reboot},
};
} // namespace

const Command* builtins(Size& count) noexcept {
    count = sizeof(kBuiltins) / sizeof(kBuiltins[0]);
    return kBuiltins;
}

} // namespace embed::shell
