// platforms/mock/platform_mock.cpp
// ---------------------------------------------------------------------------
// Полная реализация платформенного контракта для сборки/отладки на ПК.
// Задачи планировщика здесь НЕ переключаются реально: контекстные функции
// сообщают об этом через Status и не делают вид, что умеют переключаться.
// Для тестов логики ядра это правильнее, чем молчаливый no-op.
// ---------------------------------------------------------------------------
#include "../../os/iplatform.hpp"
#include "../../os/core/memory/memory_manager.hpp"
#include "boot_mock.hpp"                  // модель перехода к другой прошивке
#include "flash_mock.hpp"                 // модели Flash/внешней памяти + FlashOps
#include "platform_power_mock.hpp"

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace embed::mock_platform {

// ── Регионы памяти, отдаваемые ядру (аналог символов линкера) ────────────────
// Размеры выбраны как на реальном STM32F103RB (20 КБ SRAM), чтобы mock-прогон
// выявлял те же проблемы нехватки памяти, что и на железе.
constexpr Size kHeapBytes      = 8u * 1024u;
constexpr Size kStackPoolWords = 1u * 1024u;

alignas(16) static uint8_t  g_heap[kHeapBytes];
alignas(16) static uint32_t g_stack_pool[kStackPoolWords];

// ── Критическая секция: считаем уровень вложенности ─────────────────────────
static uint32_t g_crit_nest = 0u;

} // namespace embed::mock_platform

namespace embed::platform {

// ───────────────────────────── Критическая секция ────────────────────────────
static uint32_t mock_critical_enter() OS_PLATFORM_FN {
    return mock_platform::g_crit_nest++;
}
static void mock_critical_exit(uint32_t /*state*/) OS_PLATFORM_FN {
    if (mock_platform::g_crit_nest > 0u) --mock_platform::g_crit_nest;
}

// ───────────────────────────────── Ядра ─────────────────────────────────────
static uint32_t mock_core_count() OS_PLATFORM_FN { return 1u; }
static uint32_t mock_current_core() OS_PLATFORM_FN { return 0u; }

// ──────────────────────────────── Контекст ──────────────────────────────────
// На хосте переключение контекста невозможно без ассемблера, поэтому
// возвращаем nullptr-подобный результат, а abort() вызывается только если
// планировщик реально попытается стартовать задачу.
static void mock_start_first_task(uint32_t* /*sp*/) OS_PLATFORM_FN {
    // Не abort(): приложение может вызывать start() в тестах и ожидать
    // возврата. Логика ядра тестируется без реального переключения.
}
static void mock_trigger_context_switch() OS_PLATFORM_FN {}
static uint32_t* mock_save_context() OS_PLATFORM_FN { return nullptr; }
static void mock_load_context(uint32_t* /*sp*/) OS_PLATFORM_FN {}

// ───────────────────────────────── Время ────────────────────────────────────
static void mock_timer_init() OS_PLATFORM_FN {}
static bool mock_timer_can_period_ms(uint32_t period_ms) OS_PLATFORM_FN {
    return period_ms >= 1u && period_ms <= 60000u;
}
static void mock_timer_set_period_ms(uint32_t /*period_ms*/) OS_PLATFORM_FN {}
static void mock_timer_start() OS_PLATFORM_FN {}
static void mock_timer_stop() OS_PLATFORM_FN {}

// ───────────────────────────────── Память ───────────────────────────────────
static uint8_t* mock_heap_base() OS_PLATFORM_FN { return mock_platform::g_heap; }
static Size mock_heap_capacity() OS_PLATFORM_FN { return mock_platform::kHeapBytes; }
static uint32_t* mock_stack_pool_base() OS_PLATFORM_FN { return mock_platform::g_stack_pool; }
static Size mock_stack_pool_capacity() OS_PLATFORM_FN { return mock_platform::kStackPoolWords; }

// ────────────────────────────── Внешняя память ──────────────────────────────
static bool mock_has_ext_ram() OS_PLATFORM_FN { return false; }
static uint32_t* mock_ext_ram_base() OS_PLATFORM_FN { return nullptr; }
static Size mock_ext_ram_capacity() OS_PLATFORM_FN { return 0u; }

static bool mock_has_ext_rom() OS_PLATFORM_FN { return true; }
static Size mock_ext_rom_capacity() OS_PLATFORM_FN {
    return mock_platform::ext_rom_model().capacity();
}
static bool mock_ext_rom_read(uint32_t offset, uint8_t* buf, uint32_t len) OS_PLATFORM_FN {
    return mock_platform::ext_rom_model().read(offset, buf, len);
}
static bool mock_ext_rom_write(uint32_t offset, const uint8_t* buf, uint32_t len) OS_PLATFORM_FN {
    return mock_platform::ext_rom_model().program(offset, buf, len);
}
static bool mock_ext_rom_erase(uint32_t offset, uint32_t len) OS_PLATFORM_FN {
    return mock_platform::ext_rom_model().erase(offset, len);
}

// ─────────────────────────────── Стек задачи ────────────────────────────────
static void mock_stack_paint(uint32_t* stack, uint32_t words) OS_PLATFORM_FN {
    if (!stack || words == 0u) return;
    // Дно хранит магическое значение (детекция переполнения),
    // остальное — паттерн для расчёта high-water mark.
    stack[0] = 0xDEADBEEFu;
    for (uint32_t i = 1u; i < words; ++i) stack[i] = 0xA5A5A5A5u;
}

static uint32_t* mock_stack_init_frame(uint32_t* sp, void (*fn)(void*), void* ctx) OS_PLATFORM_FN {
    if (!sp || !fn) return nullptr;
    // На хосте аппаратного фрейма нет: кладём в «стек» описание задачи,
    // чтобы логика ядра могла проверить, что sp != nullptr.
    sp -= 4;
    sp[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
    sp[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
    sp[2] = 0u;
    sp[3] = 0u;
    return sp;
}

// ─────────────────────────────── Простой ────────────────────────────────────
// На хосте останавливать нечего: считаем вызовы — по ним тест проверяет, что
// супервизор энергосбережения действительно отдал простой платформе.
static void mock_idle() OS_PLATFORM_FN {
    ++embed::mock_platform::power_model().idle_calls;
}

// ─────────────────────────── Привязка АЦП ──────────────────────────────────
static void mock_adc_bind(void (* /*on_irq*/)() noexcept) OS_PLATFORM_FN {}

// На стенде обработчиков прерываний нет: тесты вызывают точки входа драйвера
// напрямую (onRxIrq/onCompleteIrq), поэтому привязка — no-op.
static void mock_i2c_bind(void (* /*on_ev*/)() noexcept,
                          void (* /*on_er*/)() noexcept) OS_PLATFORM_FN {}

// ───────────────────────────────── Лог ──────────────────────────────────────
static void mock_log(const char* str) OS_PLATFORM_FN {
    if (!str) return;
    (void)std::fputs(str, stdout);
    (void)std::fputc('\n', stdout);   // иначе сообщения склеиваются
}

// ─────────────────────────────── Таблица Ops ────────────────────────────────
// Энергосбережение и сторожевой таймер собираются отдельными файлами
// (platform_power.cpp, platform_watchdog.cpp): их модели «железа» нужны и
// тестам, поэтому живут в своих TU.
const PowerOps& mock_power_ops() noexcept;
const WatchdogOps& mock_watchdog_ops() noexcept;
static const Ops kOps{
    /* critical_enter        */ &mock_critical_enter,
    /* critical_exit         */ &mock_critical_exit,
    /* core_count            */ &mock_core_count,
    /* current_core          */ &mock_current_core,
    /* start_first_task      */ &mock_start_first_task,
    /* trigger_context_switch*/ &mock_trigger_context_switch,
    /* save_context          */ &mock_save_context,
    /* load_context          */ &mock_load_context,
    /* timer_init            */ &mock_timer_init,
    /* timer_can_period_ms   */ &mock_timer_can_period_ms,
    /* timer_set_period_ms   */ &mock_timer_set_period_ms,
    /* timer_start           */ &mock_timer_start,
    /* timer_stop            */ &mock_timer_stop,
    /* heap_base             */ &mock_heap_base,
    /* heap_capacity         */ &mock_heap_capacity,
    /* stack_pool_base       */ &mock_stack_pool_base,
    /* stack_pool_capacity   */ &mock_stack_pool_capacity,
    /* has_ext_ram           */ &mock_has_ext_ram,
    /* ext_ram_base          */ &mock_ext_ram_base,
    /* ext_ram_capacity      */ &mock_ext_ram_capacity,
    /* has_ext_rom           */ &mock_has_ext_rom,
    /* ext_rom_capacity      */ &mock_ext_rom_capacity,
    /* ext_rom_read          */ &mock_ext_rom_read,
    /* ext_rom_write         */ &mock_ext_rom_write,
    /* ext_rom_erase         */ &mock_ext_rom_erase,
    /* stack_paint           */ &mock_stack_paint,
    /* stack_init_frame      */ &mock_stack_init_frame,
    /* idle                  */ &mock_idle,
    /* adc_bind              */ &mock_adc_bind,
    /* i2c_bind              */ &mock_i2c_bind,
    /* log                   */ &mock_log,
    /* power                 */ &mock_power_ops(),
    /* watchdog              */ &mock_watchdog_ops(),
    /* boot                  */ &mock_platform::mock_boot_ops(),
    /* flash                 */ &mock_platform::mock_flash_ops(),
};

const Ops& ops() OS_PLATFORM_FN { return kOps; }

bool validate() OS_PLATFORM_FN {
    // Единственное, что нельзя проверить компилятором: заполнена ли таблица
    // энергосбережения. Её отсутствие проявилось бы разыменованием nullptr
    // в idle-задаче — то есть падением в простое.
    if (kOps.power == nullptr) return false;
    if (kOps.power->caps == nullptr) return false;
    if (kOps.power->sysclk_hz == nullptr) return false;
    if (kOps.power->set_profile == nullptr) return false;
    if (kOps.power->deep_sleep == nullptr) return false;
    if (kOps.power->deep_sleep_horizon_ms == nullptr) return false;
    if (kOps.power->deep_sleep_granularity_ms == nullptr) return false;

    // Таблица сторожевого таймера: её отсутствие проявилось бы падением в
    // прерывании тика — то есть в коде, который и должен спасать от зависаний.
    if (kOps.watchdog == nullptr) return false;
    if (kOps.watchdog->caps == nullptr) return false;
    if (kOps.watchdog->closest_timeout_ms == nullptr) return false;
    if (kOps.watchdog->start == nullptr) return false;
    if (kOps.watchdog->kick == nullptr) return false;
    if (kOps.watchdog->stop == nullptr) return false;
    if (kOps.watchdog->note_write == nullptr) return false;
    if (kOps.watchdog->note_read == nullptr) return false;
    if (kOps.watchdog->read_reset_cause == nullptr) return false;

    // Переход к другой прошивке: его отсутствие проявилось бы отказом
    // загрузчика в момент, когда исправлять уже нечего.
    if (kOps.i2c_bind == nullptr) return false;
    if (kOps.boot == nullptr) return false;
    if (kOps.boot->image_base == nullptr) return false;
    if (kOps.boot->image_plausible == nullptr) return false;
    if (kOps.boot->jump == nullptr) return false;
    if (kOps.boot->relocate_vectors == nullptr) return false;

    // Встроенная Flash: её отсутствие проявилось бы отказом записи посреди
    // обновления — то есть тогда, когда слот уже стёрт.
    if (kOps.flash == nullptr) return false;
    if (kOps.flash->caps == nullptr) return false;
    if (kOps.flash->capacity == nullptr) return false;
    if (kOps.flash->page_size == nullptr) return false;
    if (kOps.flash->write_granularity == nullptr) return false;
    if (kOps.flash->unlock == nullptr) return false;
    if (kOps.flash->lock == nullptr) return false;
    if (kOps.flash->erase == nullptr) return false;
    if (kOps.flash->program == nullptr) return false;
    if (kOps.flash->read == nullptr) return false;
    return true;
}

} // namespace embed::platform
