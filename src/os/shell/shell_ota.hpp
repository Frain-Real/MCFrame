// shell_ota.hpp
// ---------------------------------------------------------------------------
// Опциональные команды OTA: состояние сессии и счётчики приёма.
// ---------------------------------------------------------------------------
#pragma once

#include "shell.hpp"
#include "../ota/ota.hpp"

namespace embed::shell {

inline void cmd_ota(Console& c, const char* /*args*/) noexcept {
    auto& svc = ota::Service::instance();
    if (!svc.initialized()) {
        c.write("ota: not initialized\n");
        return;
    }

    const ota::Stats st = svc.stats();
    c.write("state=");
    c.write_u32(static_cast<uint32_t>(svc.state()));
    c.write(" session=");
    c.write(svc.session_active() ? "yes" : "no");
    c.write(" slot=");
    c.write_u32(svc.session_slot());
    c.write(" offset=");
    c.write_u32(svc.next_offset());
    c.write("\n");

    c.write("frames=");
    c.write_u32(st.frames);
    c.write(" bad=");
    c.write_u32(st.bad_frames);
    c.write(" bytes=");
    c.write_u32(st.bytes_written);
    c.write(" verify_ok=");
    c.write_u32(st.verify_ok);
    c.write(" verify_fail=");
    c.write_u32(st.verify_fail);
    c.write(" activations=");
    c.write_u32(st.activations);
    c.write("\n");
}

inline void add_ota_commands(Console& c) noexcept {
    static const Command cmds[] = {
        {"ota", "OTA session state and counters", &cmd_ota},
    };
    (void)c.add(cmds, 1u);
}

} // namespace embed::shell
