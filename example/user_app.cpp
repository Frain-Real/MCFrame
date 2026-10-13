// example/user_app.cpp
// ---------------------------------------------------------------------------
// Пример приложения: две задачи, самотест подсистем и отчёт о RAM.
//
// Что демонстрируется (и чего НЕ было в прежнем примере):
//  * TCB и стеки статические — куча для задач не нужна;
//  * регистрация задачи ОДНИМ вызовом add_task(t, stack, words), который
//    сам инициализирует стек и переводит задачу в ready;
//  * сон через scheduler::sleep_for() с проверкой кода возврата;
//  * измерение стека по водяному знаку, а не «на глаз».
// ---------------------------------------------------------------------------
#include "user_app.hpp"

#include "os/adc/adc.hpp"
#include "os/config/time_config.hpp"
#include "os/core/memory/memory_manager.hpp"

#include <atomic>

namespace {

// ── Размеры стеков задач ──
//
// Подобраны по водяному знаку (см. user_app_report_ram): задача, которая
// делает немного вычислений и уходит в sleep_for, укладывается в 48-64 слова.
// Прежние 192 и 256 слов (768 и 1024 Б) были взяты «с запасом» и съедали
// 1792 Б RAM.
constexpr uint32_t kBlinkStackWords  = 48u;    // 192 Б
constexpr uint32_t kWorkerStackWords = 64u;    // 256 Б

// ── Задачи ──
embed::task     g_blink_tcb;
uint32_t        g_blink_stack[kBlinkStackWords];

embed::task     g_worker_tcb;
uint32_t        g_worker_stack[kWorkerStackWords];

std::atomic<uint32_t> g_blink_count{0};
std::atomic<uint32_t> g_worker_count{0};

void blink_task(void* /*ctx*/) noexcept {
    embed::scheduler& sched = embed::scheduler::instance();
    for (;;) {
        g_blink_count.fetch_add(1u, std::memory_order_relaxed);
        // sleep_for сам инициирует переключение контекста.
        (void)sched.sleep_for(500);
    }
}

void worker_task(void* /*ctx*/) noexcept {
    embed::scheduler& sched = embed::scheduler::instance();
    for (;;) {
        g_worker_count.fetch_add(1u, std::memory_order_relaxed);
        (void)sched.sleep_for(1000);
    }
}

// ── АЦП ──
// Внешний буфер ping-pong предоставляет приложение: так его размер и
// размещение контролирует пользователь, а драйвер не тащит статические
// килобайты. 2 канала × блок 4 кадра × 2 половины = 16 отсчётов = 32 Б.
struct AdcKernel {
    static void yield() noexcept {
        (void)embed::scheduler::instance().sleep_for(1);
    }
};
struct AdcHal { uint8_t reserved = 0; };

using Adc = embed::ADC<AdcKernel, AdcHal, 2, 4>;

alignas(32) uint16_t g_adc_dma_buf[Adc::kDmaTotalSamples];
Adc                  g_adc;
AdcHal               g_adc_hal{};

} // namespace

// ─────────────────────────────── Регистрация ────────────────────────────────

embed::Status user_app_register(embed::scheduler& sched) noexcept {
    embed::Status st = g_blink_tcb.init(&blink_task, nullptr, "blink",
                                        kBlinkStackWords, /*priority*/ 2u);
    if (st != embed::Status::Ok) return st;

    st = g_worker_tcb.init(&worker_task, nullptr, "worker",
                           kWorkerStackWords, /*priority*/ 3u);
    if (st != embed::Status::Ok) return st;

    if (!sched.add_task(&g_blink_tcb, g_blink_stack, kBlinkStackWords)) {
        return embed::Status::NoMemory;
    }
    if (!sched.add_task(&g_worker_tcb, g_worker_stack, kWorkerStackWords)) {
        return embed::Status::NoMemory;
    }
    return embed::Status::Ok;
}

// ──────────────────────────────── Самотест ──────────────────────────────────

embed::Status user_app_selftest() noexcept {
    using namespace embed;

    // Куча может быть не сконфигурирована (регион = 0 в линкере):
    // тогда проверять нечего, это штатная конфигурация.
    if (!g_memory_manager.heap_available()) return Status::Ok;

    void* a = g_memory_manager.heap_alloc(32u);
    void* b = g_memory_manager.heap_alloc(48u);
    if (!a || !b) return Status::NoMemory;

    g_memory_manager.heap_free(a);
    g_memory_manager.heap_free(b);

    if (!g_memory_manager.heap_check()) return Status::Corruption;
    if (g_memory_manager.heap_faults() != 0u) return Status::Corruption;

    // Запрос больше кучи обязан вернуть nullptr, а не «успех» с испорченной
    // статистикой.
    void* too_big = g_memory_manager.heap_alloc(g_memory_manager.heap_total() + 1u);
    if (too_big != nullptr) {
        g_memory_manager.heap_free(too_big);
        return Status::Fault;
    }
    return Status::Ok;
}

// ─────────────────────────────────── АЦП ────────────────────────────────────

embed::Status user_app_adc_demo() noexcept {
    using namespace embed;

    Adc::Config cfg;
    cfg.mode            = Adc::Mode::Polling;
    cfg.dma_buffer      = g_adc_dma_buf;
    cfg.dma_buffer_size = Adc::kDmaTotalSamples;

    if (g_adc.init(cfg) != Status::Ok) return Status::Fault;
    if (g_adc.initHardware(&g_adc_hal, sizeof(g_adc_hal)) != Status::Ok) {
        return Status::HardwareError;
    }

    const uint8_t channels[2] = {0u, 1u};
    (void)platform_adc_setChannels(channels, 2u);

    if (g_adc.start() != Status::Ok) return Status::HardwareError;

    Adc::Frame frame{};
    const Status st = g_adc.readFrame(frame, /*timeout*/ 1000u);

    (void)g_adc.stop();
    return (st == Status::Ok) ? Status::Ok : st;
}

// ─────────────────────────────── Отчёт о RAM ────────────────────────────────

namespace {

void log_u32(const char* label, uint32_t value) {
    char buf[24];
    unsigned n = 0u;
    if (value == 0u) { buf[n++] = '0'; }
    while (value > 0u && n < sizeof(buf) - 1u) {
        buf[n++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    }
    for (unsigned i = 0; i < n / 2u; ++i) {
        const char t = buf[i];
        buf[i] = buf[n - 1u - i];
        buf[n - 1u - i] = t;
    }
    buf[n] = '\0';

    embed::platform_log_line(label);
    embed::platform_log_line(buf);
}

} // namespace

void user_app_report_ram() noexcept {
    using namespace embed;

    platform_log_line("--- RAM ---");

    log_u32("static core capacities, bytes:",
            static_cast<uint32_t>(scheduler::instance().static_bytes()
                                  + g_memory_manager.static_bytes()
                                  + config::OsTimer::static_bytes()));

    log_u32("  scheduler.static_bytes:",
            static_cast<uint32_t>(scheduler::instance().static_bytes()));
    log_u32("  memory_manager.static_bytes:",
            static_cast<uint32_t>(g_memory_manager.static_bytes()));
    log_u32("  OsTimer.static_bytes:",
            static_cast<uint32_t>(config::OsTimer::static_bytes()));

    log_u32("heap total, bytes:",  static_cast<uint32_t>(g_memory_manager.heap_total()));
    log_u32("heap free, bytes:",   static_cast<uint32_t>(g_memory_manager.heap_free_bytes()));
    log_u32("stack pool, words:",  static_cast<uint32_t>(g_memory_manager.stack_pool_total()));

    // Водяные знаки: сколько слов стека РЕАЛЬНО осталось свободными на пике.
    log_u32("blink stack words: free at peak",
            static_cast<uint32_t>(g_memory_manager.task_stack_high_water(&g_blink_tcb)));
    log_u32("worker stack words: free at peak",
            static_cast<uint32_t>(g_memory_manager.task_stack_high_water(&g_worker_tcb)));
    log_u32("blink stack overflowed:",
            g_memory_manager.task_stack_overflow(&g_blink_tcb) ? 1u : 0u);
    log_u32("worker stack overflowed:",
            g_memory_manager.task_stack_overflow(&g_worker_tcb) ? 1u : 0u);

    platform_log_line("-----------");
}
