// os/boot/boot_app.cpp
// ---------------------------------------------------------------------------
// Обвязка загрузчика со стороны приложения. См. boot_app.hpp — там про то,
// почему это отдельный модуль, а не часть boot.cpp.
// ---------------------------------------------------------------------------
#include "boot_app.hpp"

namespace embed::boot {

Status fill_ota_config(ota::Config& ota_cfg, const Config& boot_cfg) noexcept {
    if (boot_cfg.storage == nullptr) return Status::InvalidArg;
    if (!boot_cfg.layout.valid()) return Status::InvalidArg;

    ota_cfg.storage          = boot_cfg.storage;
    ota_cfg.meta_offset      = boot_cfg.layout.meta_offset;
    ota_cfg.meta_size        = boot_cfg.layout.meta_size;
    ota_cfg.slot_offset[0]   = boot_cfg.layout.slot_offset[0];
    ota_cfg.slot_capacity[0] = boot_cfg.layout.slot_size[0];
    ota_cfg.slot_offset[1]   = boot_cfg.layout.slot_offset[1];
    ota_cfg.slot_capacity[1] = boot_cfg.layout.slot_size[1];
    ota_cfg.note_base        = boot_cfg.note_base;

    // Активный слот — тот, из которого СЕЙЧАС исполняется код (VTOR), а не
    // константа сборки: приложение, собранное для слота A и запущенное из слота
    // B, «подтвердило» бы чужую загрузку, и счётчик попыток не сбросился бы.
    const uint8_t slot = Loader::instance().current_slot();
    ota_cfg.active_slot = slot;

    return (slot == Layout::kNoSlot) ? Status::NotFound : Status::Ok;
}

bool ota_is_available(const Config& boot_cfg) noexcept {
    (void)boot_cfg;
    if (!ota::Service::instance().initialized()) return false;
    return Loader::instance().current_slot() != Layout::kNoSlot;
}

Status confirm_self(const Config& boot_cfg) noexcept {
    const uint8_t slot = Loader::instance().current_slot();
    if (slot == Layout::kNoSlot) {
        // Заводская прошивка: подтверждать нечего.
        return Status::NotFound;
    }
    if (!ota::Service::instance().initialized()) return Status::Unavailable;

    ota::BootInfo bi{};
    const Status bst = ota::Service::instance().boot_check(bi);
    if (bst != Status::Ok) return bst;

    if (!bi.valid) {
        // Метки о слоте нет: загрузчик пустил нас как «образ без метки» и
        // считал попытки в резервных регистрах. То, что мы дошли до этой
        // строки, — доказательство работоспособности, значит счётчик нужно
        // обнулить, иначе после нескольких перезагрузок загрузчик перестанет
        // грузить исправную прошивку.
        const platform::WatchdogOps* wd = platform::ops().watchdog;
        if (wd != nullptr && wd->note_write != nullptr) {
            (void)wd->note_write(boot_cfg.note_base + ota::kNoteIdxMagic,
                                 ota::kNoteMagic);
            (void)wd->note_write(boot_cfg.note_base + ota::kNoteIdxSlot, slot);
            (void)wd->note_write(boot_cfg.note_base + ota::kNoteIdxAttempts, 0u);
            (void)wd->note_write(boot_cfg.note_base + ota::kNoteIdxState, 0xFFu);
        }
        return Status::Ok;
    }

    if (!bi.trial) {
        // Загрузка не пробная: подтверждать нечего.
        return Status::Ok;
    }

    const Status cst = ota::Service::instance().confirm_running();
    return (cst == Status::Ok) ? Status::Ok : Status::NotSupported;
}

} // namespace embed::boot
