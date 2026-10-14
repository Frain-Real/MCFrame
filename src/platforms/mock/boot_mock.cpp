// platforms/mock/boot_mock.cpp
// ---------------------------------------------------------------------------
// Модель перехода + таблица platform::BootOps для mock-сборки.
//
// ВАЖНО ПРО ПРОВЕРКУ ПРАВДОПОДОБИЯ ОБРАЗА
// Она читает ПЕРВЫЕ ДВА СЛОВА образа — начальный SP и обработчик сброса — из
// той же модели носителя, которой пользуется OTA (flash_mock.hpp). То есть
// «прыжок в мусор» проверяется на реальных байтах, которые тест положил в
// модель, а не на флаге в структуре.
// ---------------------------------------------------------------------------
#include "boot_mock.hpp"
#include "flash_mock.hpp"

namespace embed::mock_platform {

namespace {

BootModel g_boot{};

uint32_t boot_image_base() OS_PLATFORM_FN {
    return g_boot.vectors;
}

/// Похож ли участок на образ: SP указывает в RAM, обработчик сброса лежит
/// внутри участка и отмечен битом Thumb.
///
/// Ровно эта же логика на STM32 читает первые два слова Flash — и ровно она
/// спасает от «прыжка в 0xFF»: у стёртого участка SP равен 0xFFFFFFFF.
bool boot_image_plausible(uint32_t addr, uint32_t capacity) OS_PLATFORM_FN {
    if (capacity < 8u) return false;

    // Читаем через модель носителя (адрес — смещение в её пространстве).
    FlashModel& flash = flash_model();
    if (addr > flash.capacity() || capacity > (flash.capacity() - addr)) return false;

    uint32_t sp = 0u;
    uint32_t reset = 0u;
    if (!flash.read(addr, &sp, sizeof sp)) return false;
    if (!flash.read(addr + 4u, &reset, sizeof reset)) return false;

    // Начальный SP — в RAM и выровнен по 8 байт (требование ARM EABI).
    if (sp < kRamBase || sp > (kRamBase + kRamSize)) return false;
    if ((sp & 0x7u) != 0u) return false;

    // Обработчик сброса — внутри участка и в режиме Thumb (младший бит 1).
    if ((reset & 1u) == 0u) return false;
    const uint32_t fn = reset & ~1u;
    if (fn < addr || fn >= (addr + capacity)) return false;
    return true;
}

bool boot_jump(uint32_t addr) OS_PLATFORM_FN {
    if (g_boot.jump_fails) return false;
    g_boot.jumped_to = addr;
    g_boot.vectors = addr;
    g_boot.irq_disabled = true;
    g_boot.systick_stopped = true;
    ++g_boot.jumps;
    return true;
}

void boot_relocate_vectors(uint32_t addr) OS_PLATFORM_FN {
    g_boot.vectors = addr;
}

const platform::BootOps kBootOps{
    /* image_base        */ &boot_image_base,
    /* image_plausible   */ &boot_image_plausible,
    /* jump              */ &boot_jump,
    /* relocate_vectors  */ &boot_relocate_vectors,
};

} // namespace

BootModel& boot_model() noexcept {
    return g_boot;
}

const platform::BootOps& mock_boot_ops() noexcept {
    return kBootOps;
}

} // namespace embed::mock_platform
