// platforms/mock/flash_mock.cpp
// ---------------------------------------------------------------------------
// Встроенная Flash и внешняя память mock-платформы: модели + таблица
// platform::FlashOps. См. flash_mock.hpp — там описано, ЧТО именно
// воспроизводит модель и почему тесты без этого ничего не проверяют.
// ---------------------------------------------------------------------------
#include "flash_mock.hpp"

namespace embed::mock_platform {

namespace {

// Буферы моделей — статические: тесты и пример не должны зависеть от кучи,
// а на МКУ её может не быть вовсе.
alignas(4) uint8_t g_flash_bytes[128u * 1024u];
alignas(4) uint8_t g_ext_rom_bytes[64u * 1024u];

FlashModel g_flash(g_flash_bytes, sizeof(g_flash_bytes), 1024u, 2u);
// Внешняя память: страница 4 КБ, запись побайтная, защиты от записи нет.
FlashModel g_ext_rom(g_ext_rom_bytes, sizeof(g_ext_rom_bytes), 4096u, 1u, false);

// ── Обёртки контракта platform::FlashOps ──
uint32_t flash_caps() OS_PLATFORM_FN {
    return static_cast<uint32_t>(platform::FlashCaps::Internal) |
           static_cast<uint32_t>(platform::FlashCaps::Lockable) |
           static_cast<uint32_t>(platform::FlashCaps::ErasePage);
}
uint32_t flash_base() OS_PLATFORM_FN { return 0u; }
uint32_t flash_capacity() OS_PLATFORM_FN { return g_flash.capacity(); }
uint32_t flash_page_size() OS_PLATFORM_FN { return g_flash.page_size(); }
uint32_t flash_granularity() OS_PLATFORM_FN { return g_flash.granularity(); }
bool     flash_unlock() OS_PLATFORM_FN { return g_flash.unlock(); }
void     flash_lock() OS_PLATFORM_FN { g_flash.lock(); }
bool     flash_erase(uint32_t addr, uint32_t len) OS_PLATFORM_FN {
    return g_flash.erase(addr, len);
}
bool     flash_program(uint32_t addr, const void* data, uint32_t len) OS_PLATFORM_FN {
    return g_flash.program(addr, data, len);
}
bool     flash_read(uint32_t addr, void* data, uint32_t len) OS_PLATFORM_FN {
    return g_flash.read(addr, data, len);
}

const platform::FlashOps kFlashOps{
    /* caps              */ &flash_caps,
    /* base              */ &flash_base,
    /* capacity          */ &flash_capacity,
    /* page_size         */ &flash_page_size,
    /* write_granularity */ &flash_granularity,
    /* unlock            */ &flash_unlock,
    /* lock              */ &flash_lock,
    /* erase             */ &flash_erase,
    /* program           */ &flash_program,
    /* read              */ &flash_read,
};

} // namespace

FlashModel& flash_model() noexcept {
    return g_flash;
}

FlashModel& ext_rom_model() noexcept {
    return g_ext_rom;
}

const platform::FlashOps& mock_flash_ops() noexcept {
    return kFlashOps;
}

} // namespace embed::mock_platform
