// platforms/stm32/boot_stm32.cpp
// ---------------------------------------------------------------------------
// Переход к образу прошивки: проверка вектора и передача управления.
// Последовательность шагов и ПОЧЕМУ она именно такая — в boot_stm32.hpp.
// ---------------------------------------------------------------------------
#include "boot_stm32.hpp"
#include "stm32_regs.hpp"

namespace embed::stm32::boot {

namespace {

/// Адрес начала Flash: образы лежат по абсолютным адресам, и «слот» — это
/// FLASH_BASE + смещение.
constexpr uint32_t kFlashBase = FLASH_BASE;

/// Границы SRAM у STM32F103RB: начальный SP образа обязан указывать сюда.
constexpr uint32_t kRamBase = 0x20000000u;
constexpr uint32_t kRamEnd  = kRamBase + 20u * 1024u;

/// Переход на обработчик сброса.
///
/// Написано на ассемблере, потому что компилятор вправе «оптимизировать»
/// передачу управления: он не знает, что после неё стек и таблица векторов уже
/// другие. Здесь же ровно одна инструкция перехода по регистру и ничего больше —
/// никаких литеральных пулов, к которым можно вернуться.
extern "C" __attribute__((naked, noreturn, used))
void embed_boot_launch(uint32_t reset_handler) noexcept {
    __asm volatile(
        "   bx r0        \n"   // R0 = адрес обработчика сброса (после снятия бита Thumb)
        "   b .          \n"   // недостижимо: если вернулись — некуда идти
    );
}

} // namespace

// ─────────────────────────── Текущий адрес образа ───────────────────────────

uint32_t image_base() noexcept {
    return SCB->VTOR;
}

// ─────────────────────────── Проверка правдоподобия ─────────────────────────

bool image_plausible(uint32_t addr, uint32_t capacity) noexcept {
    if (capacity < 8u) return false;
    if (addr < kFlashBase) return false;

    // Читаем через тот же указатель, что и всё остальное: на этом этапе
    // периферия ещё не нужна, а Flash отображена в адресное пространство.
    const auto* words = reinterpret_cast<volatile const uint32_t*>(addr);
    const uint32_t sp    = words[0];
    const uint32_t reset = words[1];

    // Начальный SP: в пределах SRAM и выровнен по 8 байт (требование ARM EABI —
    // невыровненный SP нарушит выравнивание аппаратного кадра исключения).
    if (sp < kRamBase || sp > kRamEnd) return false;
    if ((sp & 0x7u) != 0u) return false;

    // Обработчик сброса: внутри образа и в режиме Thumb.
    if ((reset & 1u) == 0u) return false;
    const uint32_t fn = reset & ~1u;
    if (fn < addr || fn >= (addr + capacity)) return false;

    return true;
}

// ────────────────────────────── Переход ────────────────────────────────────

void relocate_vectors(uint32_t addr) noexcept {
    SCB->VTOR = addr;
    __DSB();
    __ISB();
}

bool jump(uint32_t addr) noexcept {
    if (!image_plausible(addr, 0x1000000u)) return false;

    const auto* image = reinterpret_cast<volatile const uint32_t*>(addr);
    const uint32_t sp    = image[0];
    const uint32_t reset = image[1];

    // 1. Прерывания запрещены: между шагами любое из них ушло бы в таблицу
    //    векторов покидаемого образа.
    __disable_irq();
    __DSB();
    __ISB();

    // 2. Системный тик остановлен: его обработчик живёт в покидаемом образе.
    SysTick->CTRL = 0u;

    // 3. Снимаем защёлкнутые прерывания: иначе первое же из них сработает
    //    сразу после разрешения — возможно, раньше, чем новый образ будет готов.
    for (uint32_t i = 0u; i < 8u; ++i) {
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    NVIC->ICER[0] = 0xFFFFFFFFu;
    NVIC->ICER[1] = 0xFFFFFFFFu;

    // 4. Таблица векторов нового образа.
    relocate_vectors(addr);

    // 5. Стек — из образа (до этой строки стек ещё наш, и вызовы работают).
    __set_MSP(sp);

    // 6. Управление. Приоритеты исключений (PendSV/SysTick) новый образ
    //    настроит сам: платформа делает это в platform_timer_init().
    embed_boot_launch(reset & ~1u);

    // Недостижимо: возврат из образа возможен только через сброс.
    return true;
}

// ──────────────────────────── Таблица для ядра ──────────────────────────────

namespace {

uint32_t ops_image_base() OS_PLATFORM_FN { return image_base(); }
bool ops_image_plausible(uint32_t addr, uint32_t capacity) OS_PLATFORM_FN {
    return image_plausible(addr, capacity);
}
bool ops_jump(uint32_t addr) OS_PLATFORM_FN { return jump(addr); }
void ops_relocate(uint32_t addr) OS_PLATFORM_FN { relocate_vectors(addr); }

const platform::BootOps kBootOps{
    /* image_base       */ &ops_image_base,
    /* image_plausible  */ &ops_image_plausible,
    /* jump             */ &ops_jump,
    /* relocate_vectors */ &ops_relocate,
};

} // namespace

const platform::BootOps& ops() noexcept { return kBootOps; }

} // namespace embed::stm32::boot
