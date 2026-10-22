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

#ifndef MCFRAME_APP_SHELL
#define MCFRAME_APP_SHELL 0
#endif

#include "os/adc/adc.hpp"
#include "os/async/async_hal.hpp"
#include "os/async/async_port.hpp"
#include "os/boot/boot.hpp"
#include "os/boot/boot_app.hpp"
#include "os/config/time_config.hpp"
#include "os/ota/ota.hpp"
#include "os/ota/ota_transport.hpp"
#include "os/power/power.hpp"
#include "os/watchdog/watchdog.hpp"

#if MCFRAME_APP_SHELL
  #include "os/log/log.hpp"
  #include "os/shell/shell.hpp"
  #include "os/shell/shell_ota.hpp"
  #include "os/shell/shell_watchdog.hpp"
  #include "boards/common/shell_board.hpp"
#endif

// Плата — единственное место, где известны USART/I2C/пины. Приложение
// больше не включает uart_stm32.hpp/i2c_stm32.hpp напрямую.
#include "board.hpp"
#include "os/core/memory/memory_manager.hpp"
#include "os/core/primitives/types.hpp"   // embed::atomic_t — аналог std::atomic

namespace {

#if MCFRAME_APP_SHELL
embed::Status user_app_shell_register(embed::scheduler& sched) noexcept;
#endif

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

// Счётчики задач — embed::atomic_t, а не std::atomic: <atomic> тянет
// libstdc++ и не собирается для «голого» Cortex-M3. Инкремент идёт из задачи,
// читает счётчик отчёт о RAM — то есть из другого контекста.
embed::atomic_t<uint32_t> g_blink_count{0u};
embed::atomic_t<uint32_t> g_worker_count{0u};

// ── Сторожевой таймер ──
//
// Участники — то, что обязано работать. Отметка делается в КОНЦЕ успешного
// цикла: задача, которая отметилась и тут же упала, ничего не доказала бы.
// Срок взят с запасом к периоду работы задачи (спит 5000 мс — отмечается не
// реже 8000 мс): слишком туго выставленный срок превращает сторож в источник
// ложных сбросов, слишком свободный — в бесполезный счётчик.
embed::watchdog::Participant g_blink_wd;
embed::watchdog::Participant g_worker_wd;

constexpr uint32_t kBlinkWdTimeout  = 8000u;    // период задачи — 500 тиков (5 с)
constexpr uint32_t kWorkerWdTimeout = 15000u;   // период задачи — 1000 тиков (10 с)

void blink_task(void* /*ctx*/) noexcept {
    embed::scheduler& sched = embed::scheduler::instance();
    for (;;) {
        (void)g_blink_count.fetch_add(1u, embed::MemoryOrderRelaxed);
        // sleep_for сам инициирует переключение контекста.
        (void)sched.sleep_for(500);
        (void)g_blink_wd.check_in();
    }
}

void worker_task(void* /*ctx*/) noexcept {
    embed::scheduler& sched = embed::scheduler::instance();
    for (;;) {
        (void)g_worker_count.fetch_add(1u, embed::MemoryOrderRelaxed);
        (void)sched.sleep_for(1000);
        (void)g_worker_wd.check_in();
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

    // Задача приёма прошивки: она опрашивает интерфейс, объявленный в
    // настройке OTA (см. user_app_ota_init).
    st = user_app_ota_register(sched);
    if (st != embed::Status::Ok) return st;

#if MCFRAME_APP_SHELL
    st = user_app_shell_register(sched);
    if (st != embed::Status::Ok) return st;
#endif

    return embed::Status::Ok;
}

// ─────────────────────────── Сторожевой таймер ──────────────────────────────

// Счётчик предупреждений: пишется из прерывания, читается отчётом — поэтому
// atomic, а не «просто переменная».
embed::atomic_bool     g_watchdog_warned{false};
embed::atomic_t<uint32_t> g_watchdog_warnings{0u};

void user_app_watchdog_warning(void* /*arg*/) noexcept {
    // ВАЖНО: это прерывание тика перед сбросом устройства. Здесь нельзя ни
    // ждать, ни блокироваться, ни обмениваться по шинам. Логировать отсюда
    // тоже нельзя: вывод в ITM ждёт отладчик и мог бы «подвесить» ядро вместо
    // сброса — а сброс здесь и есть правильное поведение.
    (void)g_watchdog_warnings.fetch_add(1u, embed::MemoryOrderRelaxed);
    g_watchdog_warned.store(true, embed::MemoryOrderRelease);
}

embed::Status user_app_watchdog_register() noexcept {
    using namespace embed;

    if (g_blink_wd.init("blink", kBlinkWdTimeout) != Status::Ok) return Status::Fault;
    if (g_worker_wd.init("worker", kWorkerWdTimeout) != Status::Ok) return Status::Fault;
    return Status::Ok;
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

// ────────────────── UART и I2C: линейный код приложения ─────────────────────
//
// Главное, что здесь видно: прикладной код ПОСЛЕДОВАТЕЛЬНЫЙ и ОДИНАКОВЫЙ
// для STM32 и mock. Ни одного колбэка. Стратегия (Polling/Async/Dma)
// выбирается в Config драйвера, а не этим кодом.

namespace {

// Драйверы выбирает board.hpp: на STM32 — USART1/I2C1, на mock — эмуляция.
using UartDriver = board::Uart;
using I2cDriver  = board::I2c;

embed::async::UartPort<UartDriver> g_uart;
embed::async::I2cPort<I2cDriver>   g_i2c;

/// Стратегия интерфейсов для примера.
///
/// Одна и та же константа уходит в Config обоих драйверов и на STM32, и на
/// mock: у mock она проверяется (его «железо» фиксировано шаблоном), у STM32
/// — реально выбирает режим. Поменяйте на Async/Polling, и прикладной код
/// ниже не изменится ни на строку — это и есть смысл трёх стратегий.
constexpr embed::async::Strategy kDemoStrategy = embed::async::Strategy::Dma;

/// Датчик на шине I2C: регистр 0x00 — температура, 0x01 — конфигурация.
constexpr uint8_t kSensorAddr = 0x48u;
constexpr uint8_t kRegTemp    = 0x00u;
constexpr uint8_t kRegConfig  = 0x01u;

} // namespace

embed::Status user_app_async_demo() noexcept {
    using namespace embed;

    // ── Настройка интерфейсов ──
    UartDriver::Config ucfg{};
    ucfg.baudRate = 115200u;
    ucfg.strategy = kDemoStrategy;
    if (UartDriver::instance().configure(ucfg) != Status::Ok) return Status::HardwareError;
    if (UartDriver::instance().open() != Status::Ok) return Status::HardwareError;

    I2cDriver::Config icfg{};
    icfg.speed = I2cDriver::Speed::Fast400k;
    icfg.peripheralClockHz = 36000000u;          // APB1 при SYSCLK 72 МГц
    icfg.strategy = kDemoStrategy;
    if (I2cDriver::instance().configure(icfg) != Status::Ok) return Status::HardwareError;
    if (I2cDriver::instance().open() != Status::Ok) return Status::HardwareError;

    // ── UART: три передачи подряд, линейно ──
    if (g_uart.writeLine("MCFrame: async io demo") != Status::Ok) return Status::HardwareError;
    if (g_uart.write("uart: line mode ok") != Status::Ok)          return Status::HardwareError;

    // ── Через арбитр каналов DMA ──
    //
    // На STM32 каналы интерфейсов пересекаются (USART1_TX и I2C2_TX — это один
    // DMA1 Ch4), поэтому работа не отказывает при занятом канале, а встаёт в
    // очередь арбитра и выполняется, когда канал освободится. На mock
    // DMA-каналов нет вовсе, и обёртка честно отвечает NotSupported — поэтому
    // вызов только для МКУ.
#ifdef MCFRAME_TARGET_MCU
    if (g_uart.writeQueued("uart: queued ok", 15u, 100u) != Status::Ok) {
        return Status::HardwareError;
    }
#endif

    // ── I2C: читаем датчик ──
    // На mock устройство нужно зарегистрировать; на STM32 отвечает железо.
#ifndef MCFRAME_TARGET_MCU
    (void)embed::mock::g_i2c_bus.register_device(kSensorAddr);
    embed::mock::g_i2c_bus.regs[kRegTemp] = 25u;      // +25 °C
#endif

    uint8_t temperature = 0u;
    const Status st = g_i2c.readRegister(I2cAddress{kSensorAddr}, kRegTemp, temperature, 50u);
    if (st != Status::Ok) {
        // Датчика нет — для демонстрации это не ошибка.
        (void)g_uart.writeLine("i2c: sensor not found");
        return Status::Ok;
    }

    // ── Пишем конфигурацию датчика ──
    if (g_i2c.writeRegister(I2cAddress{kSensorAddr}, kRegConfig, 0x60u, 50u) != Status::Ok) {
        return Status::HardwareError;
    }

    // ── Сообщаем результат по UART ──
    if (g_uart.writeLine("i2c: sensor ok") != Status::Ok) return Status::HardwareError;

    // ── Проверка на mock-платформе ──
    // На железе данные уходят в линию, здесь их можно проверить:
    // все передачи и обмен по I2C действительно состоялись.
#ifndef MCFRAME_TARGET_MCU
    {
        using namespace embed;
        // bus() живёт у драйвера, а не у порта: порт — прикладной фасад.
        if (UartDriver::instance().bus().tx_len == 0u) return Status::Fault;
        if (temperature != 25u)                        return Status::Corruption;
        if (I2cDriver::instance().bus().regs[kRegConfig] != 0x60u) {
            return Status::Corruption;
        }
    }
#endif

    // В реальном приложении temperature ушло бы в протокол или лог.
    (void)temperature;
    return Status::Ok;
}

// ──────────────────────────── Отчёты: помощник ─────────────────────────────
//
// Печать числа без newlib: printf на МКУ тянет форматтер и float-подсистему,
// а отчёту нужно ровно одно беззнаковое число в десятичном виде.

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

// ────────────────────────────── OTA: обновление прошивки ─────────────────────
//
// Приложение — ВТОРАЯ половина схемы обновления. Первая — загрузчик
// (bootloader/): он запускается первым, проверяет образ в слоте и умеет
// откатиться, если новая прошивка не подтвердила себя.
//
// Что делает приложение:
//   * принимает образ по протоколу OTA (тот же ota::Service, что и загрузчик);
//   * ПОДТВЕРЖДАЕТ свою загрузку, когда дошло до конца инициализации, — без
//     этого загрузчик будет считать её пробной и в конце концов откатится;
//   * сообщает, ИЗ КАКОГО СЛОТА оно работает: слот берётся из VTOR (куда его
//     загрузили), а не из константы сборки — иначе приложение, собранное для
//     слота A и запущенное из B, «подтвердило» бы чужую загрузку.
//
// Раскладка берётся ИЗ ОДНОГО МЕСТА (os/boot/boot_layout.hpp + CMake): если
// загрузчик и приложение разойдутся в адресах, загрузчик будет стирать не тот
// слот, и это самая дорогая ошибка из возможных.

namespace {

// Раскладка — общая с загрузчиком и с линкером (см. cmake/firmware_layout.cmake).
constexpr embed::boot::Layout kLayout = embed::boot::kBoard;

/// Версия работающей прошивки. Уходит хосту в INFO и служит порогом для
/// запрета откатов: без него «обновление» старым образом затирало бы рабочую
/// версию, и устройство молча откатывалось.
constexpr uint32_t kFirmwareVersion = 0x00010000u;

embed::ota::InternalFlashStorage g_ota_flash;

using OtaUart = board::Uart;

/// Транспорт: интерфейс приёма. Порт берётся у драйвера, стратегия
/// (Polling/Async/Dma) — его дело; для OTA это невидимо.
[[maybe_unused]] embed::ota::StreamTransport<OtaUart> g_ota_transport{
    OtaUart::instance(), embed::ota::proto::Iface::Uart, 0u, "ota-uart"};

/// Участник сторожевого таймера: задача приёма обязана отмечаться.
/// Срок взят с запасом к периоду задачи (100 мс) — OTA не должна превращаться
/// в источник ложных сбросов, когда обновлений нет.
embed::watchdog::Participant g_ota_wd;
constexpr uint32_t kOtaWdTimeout = 5000u;

embed::task g_ota_tcb;
constexpr uint32_t kOtaStackWords = 96u;    // 384 Б: буферы сервиса статические
uint32_t g_ota_stack[kOtaStackWords];

embed::atomic_t<uint32_t> g_ota_polls{0u};

/// Настройка загрузчика со стороны приложения: носитель и раскладка.
embed::boot::Config& boot_cfg() noexcept {
    static embed::boot::Config cfg = [] {
        embed::boot::Config c{};
        c.storage = &g_ota_flash;
        c.layout  = kLayout;
        return c;
    }();
    return cfg;
}

void ota_task(void* /*ctx*/) noexcept {
    embed::scheduler& sched = embed::scheduler::instance();
    for (;;) {
        // Бюджет 50 мс: за это время сервис принимает столько кадров, сколько
        // успело прийти, и возвращает управление — задача не «съедает» цикл,
        // когда обновления нет.
        (void)embed::ota::Service::instance().poll(50u);
        (void)g_ota_polls.fetch_add(1u, embed::MemoryOrderRelaxed);
        (void)g_ota_wd.check_in();
        (void)sched.sleep_for(100);
    }
}

#if MCFRAME_APP_SHELL
// ─────────────────────────── Shell над UART ────────────────────────────────
// Один UART1 используется shell'ом, а не приёмом OTA: два читателя одного
// потока байт не могут корректно делить кадры. При MCFRAME_APP_SHELL=ON
// OTA-транспорт отключается (см. user_app_ota_init), OTA остаётся для
// проверки/активации уже лежащего образа.
embed::shell::Console g_shell;
embed::task           g_shell_tcb;
constexpr uint32_t    kShellStackWords = 192u;
uint32_t              g_shell_stack[kShellStackWords];

void shell_write(void* /*ctx*/, const char* text) noexcept {
    if (text == nullptr) return;
    embed::Size len = 0u;
    while (text[len] != '\0') ++len;
    embed::Size written = 0u;
    (void)OtaUart::instance().write(text, len, written);
}

void shell_task(void* /*ctx*/) noexcept {
    embed::scheduler& sched = embed::scheduler::instance();
    for (;;) {
        char c = '\0';
        embed::Size n = 0u;
        const embed::Status st = OtaUart::instance().read(&c, 1u, n, 10u);
        if (st == embed::Status::Ok && n == 1u) {
            g_shell.feed(c);
        } else {
            (void)sched.sleep_for(5u);
        }
    }
}

embed::Status user_app_shell_register(embed::scheduler& sched) noexcept {
    using namespace embed;

    OtaUart::Config cfg{};
    cfg.baudRate = 115200u;
    cfg.strategy = async::Strategy::Polling;
    if (OtaUart::instance().configure(cfg) != Status::Ok) return Status::HardwareError;
    if (OtaUart::instance().open() != Status::Ok) return Status::HardwareError;

    g_shell.init(&shell_write, nullptr);
#if !MCFRAME_APP_MINIMAL
    // В слоте 52 КБ команды wd/ota/board стоят дополнительного места; в
    // «полной» сборке они есть. ps/free/stats/log/reboot остаются всегда.
    shell::add_watchdog_commands(g_shell);
    shell::add_ota_commands(g_shell);
    board_shell::add_board_commands(g_shell);
#endif

    const Status st = g_shell_tcb.init(&shell_task, nullptr, "shell",
                                       kShellStackWords, /*priority*/ 3u);
    if (st != Status::Ok) return st;
    if (!sched.add_task(&g_shell_tcb, g_shell_stack, kShellStackWords)) {
        return Status::NoMemory;
    }
    g_shell.prompt();
    return Status::Ok;
}
#endif

} // namespace

// ─────────────────────────── Регистрация и инициализация ───────────────────

embed::Status user_app_ota_register(embed::scheduler& sched) noexcept {
    using namespace embed;

    const Status st = g_ota_tcb.init(&ota_task, nullptr, "ota", kOtaStackWords,
                                    /*priority*/ 4u);
    if (st != Status::Ok) return st;
    if (!sched.add_task(&g_ota_tcb, g_ota_stack, kOtaStackWords)) {
        return Status::NoMemory;
    }
    return Status::Ok;
}

embed::Status user_app_ota_init() noexcept {
    using namespace embed;

    // Окно носителя — вся встроенная Flash: смещения в раскладке заданы как
    // «адрес в пространстве Flash» (база прибавляется платформой).
    g_ota_flash.set_window(0u, 0u);

    ota::Config cfg{};
    // Адреса слотов, метаданных и АКТИВНЫЙ СЛОТ (по VTOR — тот, из которого мы
    // действительно работаем) даёт общая обвязка с загрузчиком.
    const Status fst = boot::fill_ota_config(cfg, boot_cfg());
    if (fst == Status::NotFound) {
        // Прошивка лежит вне слотов (залита программатором с 0x08000000):
        // обновляться из неё нечему и некуда. Это не ошибка — просто режим без
        // загрузчика.
        platform_log_line("MCFrame: OTA unavailable (image is not in a slot)");
        return Status::Ok;
    }
    if (fst != Status::Ok) return fst;

#if MCFRAME_APP_SHELL
    cfg.transport        = nullptr;             // UART занят shell'ом
#else
    cfg.transport        = &g_ota_transport;   // ПО ЧЕМУ принимается
#endif
    cfg.fw_version       = kFirmwareVersion;

    // Приложение занято обменом и датчиками: обновление не должно мешать
    // сторожу (стирание страницы идёт десятки миллисекунд) и не должно
    // уходить в глубокий сон посреди передачи.
    cfg.erase_stall_ms     = 1000u;
    cfg.use_watchdog_stall = true;
    cfg.keep_awake         = true;
    cfg.checkpoint_bytes   = 4096u;               // как на железе
    cfg.session_timeout_ms = 30000u;

    const Status st = ota::Service::instance().init(cfg);
    if (st != Status::Ok) return st;

    if (g_ota_wd.init("ota", kOtaWdTimeout) != Status::Ok) return Status::Fault;

    // ── Подтверждение собственной загрузки ──
    //
    // Загрузчик считает загрузку новой прошивки ПРОБНОЙ и разрешает её
    // ограниченное число раз. Подтверждение снимает счётчик; без него
    // устройство, у которого приложение поднимается, но не доходит до этой
    // строки, будет откатываться на прежний слот.
    //
    // ВАЖНО: подтверждать нужно ПОСЛЕ того, как приложение доказало, что
    // работает (самотест, поднятая периферия). Здесь подтверждение делается
    // сразу после инициализации OTA: в примере нет ничего, что могло бы «упасть
    // на старте»; в реальном приложении это место — ПОСЛЕ самотеста.
    const Status cst = boot::confirm_self(boot_cfg());
    if (cst == Status::NotSupported) {
        // Запись метки не прошла: загрузчик будет считать загрузку пробной и в
        // конце концов откатится. Молчать об этом нельзя.
        platform_log_line("MCFrame: OTA confirm failed (flash write?)");
    }
    return Status::Ok;
}

void user_app_report_ota() noexcept {
    using namespace embed;

    const ota::Stats st = ota::Service::instance().stats();

    platform_log_line("--- OTA ---");
    platform_log_line("storage:");
    platform_log_line(g_ota_flash.name());
    platform_log_line("interface:");
    platform_log_line(ota::proto::iface_text(ota::Service::instance().iface()));

    // Из какого слота работаем: по VTOR, а не по константе сборки.
    const uint8_t slot = boot::Loader::instance().current_slot();
    log_u32("running from slot:", (slot == boot::Layout::kNoSlot) ? 0xFFFFFFFFu : slot);
    log_u32("image base:", boot::Loader::instance().image_base());
    log_u32("slot A at:", kLayout.slot_offset[0]);
    log_u32("slot B at:", kLayout.slot_offset[1]);
    log_u32("metadata at:", kLayout.meta_offset);

    log_u32("state:", static_cast<uint32_t>(ota::Service::instance().state()));
    log_u32("frames:", st.frames);
    log_u32("bad frames:", st.bad_frames);
    log_u32("resync bytes:", st.resync_bytes);
    log_u32("sessions:", st.sessions);
    log_u32("resumed:", st.resumed);
    log_u32("bytes written:", st.bytes_written);
    log_u32("page erases:", st.page_erases);
    log_u32("verify ok:", st.verify_ok);
    log_u32("verify fail:", st.verify_fail);
    log_u32("readback fail:", st.readback_fail);
    log_u32("activations:", st.activations);
    log_u32("naks sent:", st.naks);

    for (uint8_t slot = 0u; slot < 2u; ++slot) {
        ota::SlotRecord rec{};
        if (ota::Service::instance().slot_record(slot, rec) != Status::Ok) continue;
        platform_log_line(slot == 0u ? "slot A:" : "slot B:");
        log_u32("  state:", rec.state);
        log_u32("  image size:", rec.image_size);
        log_u32("  image crc32:", rec.image_crc32);
        log_u32("  version:", rec.fw_version);
        log_u32("  stored bytes:", rec.stored_bytes);
        log_u32("  boot attempts:", rec.boot_attempts);
    }

    // Задача приёма: если счётчик не растёт, значит задача не исполняется —
    // это видно здесь, а не в «тихом» отказе обновления.
    log_u32("poll iterations:", g_ota_polls.load(embed::MemoryOrderRelaxed));
    platform_log_line("-----------");
}

// ─────────────────────────────── Отчёт о RAM ────────────────────────────────

void user_app_report_ram() noexcept {
    using namespace embed;

    platform_log_line("--- RAM ---");

    log_u32("static core capacities, bytes:",
            static_cast<uint32_t>(scheduler::instance().static_bytes()
                                  + g_memory_manager.static_bytes()
                                  + config::OsTimer::static_bytes()
                                  + power::Supervisor::static_bytes()
                                  + watchdog::Service::static_bytes()
                                  + ota::Service::static_bytes()));

    log_u32("  scheduler.static_bytes:",
            static_cast<uint32_t>(scheduler::instance().static_bytes()));
    log_u32("  memory_manager.static_bytes:",
            static_cast<uint32_t>(g_memory_manager.static_bytes()));
    log_u32("  OsTimer.static_bytes:",
            static_cast<uint32_t>(config::OsTimer::static_bytes()));
    log_u32("  power::Supervisor.static_bytes:",
            static_cast<uint32_t>(power::Supervisor::static_bytes()));
    log_u32("  watchdog::Service.static_bytes:",
            static_cast<uint32_t>(watchdog::Service::static_bytes()));
    // OTA: буферы кадров (приём и ответ), буфер проверки чтением, записи
    // состояния слотов и контекст SHA-256. Число статическое: сколько RAM
    // съест приём прошивки, известно заранее и не зависит от размера образа.
    log_u32("  ota::Service::static_bytes:",
            static_cast<uint32_t>(ota::Service::static_bytes()));

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

    // Счётчики задач: читаются из контекста отчёта, пишутся задачами —
    // поэтому только через atomic_t (load, не прямое чтение поля).
    log_u32("blink iterations:",  g_blink_count.load(embed::MemoryOrderRelaxed));
    log_u32("worker iterations:", g_worker_count.load(embed::MemoryOrderRelaxed));

    platform_log_line("-----------");
}

// ──────────────────────────── Отчёт сторожа ────────────────────────────────

void user_app_report_watchdog() noexcept {
    using namespace embed;

    watchdog::Service& wd = watchdog::Service::instance();
    const watchdog::Stats st = wd.stats();
    const watchdog::Fault f  = wd.last_fault();

    platform_log_line("--- WATCHDOG ---");
    log_u32("hardware timeout, ms:", wd.hardware_timeout_ms());
    log_u32("participants:", st.participants);
    log_u32("deadline checks:", st.checks);
    log_u32("hardware kicks:", st.kicks);

    // Просрочки и предупреждения: в норме — нули. Ненулевое значение означает,
    // что устройство УЖЕ перезагружалось по сторожу (или перезагрузится).
    log_u32("misses:", st.misses);
    log_u32("warnings:", st.warnings);
    log_u32("max late, ms:", st.max_late_ms);
    log_u32("rearms (снятий защёлки):", st.rearms);

    // Сон: сторож считает и в STOP, поэтому сон ограничен его сроком.
    log_u32("deep sleeps under watchdog:", st.deep_sleeps);
    log_u32("slept under watchdog, ms:", st.slept_ms);
    log_u32("sleep limit, ms:", wd.sleep_limit_ms());

    // Причина прошлой перезагрузки: ради этого отметка и пишется в резервные
    // регистры (они переживают сброс).
    log_u32("reset cause bits:", wd.reset_cause());
    log_u32("last fault known:", f.valid ? 1u : 0u);
    log_u32("last fault participant:", (f.participant == watchdog::kNoParticipant)
                                       ? 0xFFFFFFFFu : f.participant);
    log_u32("last fault late, ms:", f.late_ms);
    log_u32("boot count:", f.boot_count);

    // Предупреждения, пойманные колбэком ДО сброса: если они есть, а сброса не
    // случилось, значит приложение успело снять защёлку (rearm).
    log_u32("warning callbacks:", g_watchdog_warnings.load(embed::MemoryOrderRelaxed));
    log_u32("warning flag set:", g_watchdog_warned.load(embed::MemoryOrderAcquire) ? 1u : 0u);
    platform_log_line("----------------");
}

// ──────────────────────────── Отчёт энергосбережения ───────────────────────

void user_app_report_power() noexcept {
    using namespace embed;

    const power::Stats st = power::Supervisor::instance().stats();
    const power::Supervisor& sup = power::Supervisor::instance();

    platform_log_line("--- POWER ---");
    log_u32("sysclk, Hz:", sup.sysclk_hz());

    log_u32("windows measured:", st.windows);
    log_u32("window accumulated, ms:", st.window_ms);
    log_u32("busy in window, ms:", st.busy_ms);
    log_u32("idle in window, ms:", st.idle_ms);
    log_u32("load last window, %:", st.load_percent);
    log_u32("load peak, %:", st.peak_load_percent);
    log_u32("load average, %:", st.avg_load_percent);

    // ГЛАВНАЯ ЦИФРА ДЛЯ ВЫБОРА ПРОФИЛЯ ЧАСТОТЫ.
    //
    // Нагрузка обратно пропорциональна частоте, поэтому «что будет на 8 МГц»
    // — это арифметика, а не догадка. Значение больше 100 означает, что на
    // пониженной частоте система не успеет за реальным временем: профиль
    // LowPower брать нельзя, как бы он ни экономил ток.
    log_u32("load at 8 MHz, % (projected):",
            power::project_load_percent(st.peak_load_percent,
                                        sup.sysclk_hz(), 8000000u));

    log_u32("sleep (WFI) entries:", st.sleep_entries);
    log_u32("deep sleep entries:", st.deep_sleep_entries);
    log_u32("deep sleep total, ms:", st.deep_sleep_ms);

    // Причины отказов от глубокого сна: без них «мы не спим» выглядит как
    // ошибка супервизора, хотя обычно это чей-то незакрытый обмен или
    // слишком короткий простой.
    log_u32("refused: no wake source:", st.refused_no_source);
    log_u32("refused: horizon too short:", st.refused_short);
    log_u32("refused: keep-awake held:", st.refused_keep_awake);
    log_u32("refused: platform failed:", st.deep_sleep_failed);

    platform_log_line("-------------");
}
