// bootloader/boot_main.cpp
// ---------------------------------------------------------------------------
// ЗАГРУЗЧИК MCFrame: точка входа, приём обновления и передача управления.
//
// ЧТО ДЕЛАЕТ ЭТОТ ОБРАЗ
//   1. поднимает минимальную часть ядра: время (таймауты протокола) и носитель;
//      планировщик, энергосбережение и сторож ему не нужны — он работает в
//      одном цикле и не «живёт долго»;
//   2. отдаёт приём прошивки тому же сервису OTA, что и приложение: один код на
//      оба образа, поэтому образ можно залить и «в приложение», и «в загрузчик»;
//   3. спрашивает у os/boot, что грузить: активированный слот, подтверждённый
//      или ничего (тогда он остаётся и ждёт хоста);
//   4. если грузить есть что — передаёт управление (VTOR, MSP и переход на
//      обработчик сброса делает платформа: platform::BootOps).
//
// ПОЧЕМУ ЦИКЛ ОДИН И БЕЗ ПЛАНИРОВЩИКА
// Всё, что загрузчик делает не так, оставляет устройство без работающей
// прошивки, а отлаживать это можно только программатором. Поэтому здесь нет ни
// переключения контекста, ни приоритетов, ни сна: цикл «принять — решить —
// прыгнуть» читается целиком, и в нём нечему «иногда не сработать».
//
// ЧЕГО ЗДЕСЬ НЕТ: стирания метаданных при старте (это уничтожило бы память об
// активации), «заводского» образа и самоперезагрузки после активации:
// перезагружает устройство приложение (или человек кнопкой), а загрузчик просто
// грузит активированный слот.
// ---------------------------------------------------------------------------
#include "os/boot/boot.hpp"
#include "os/config/time_config.hpp"
#include "os/ota/ota.hpp"

// ── Принимает ли ЗАГРУЗЧИК прошивку сам ──
//
// Загрузчик умеет две вещи: решить/проверить/передать управление (это нужно
// всегда) и ПРИНЯТЬ образ по протоколу OTA (это уже полноценный приёмник со
// SHA-256, CRC32 и драйвером интерфейса — десятки килобайт).
//
// На STM32F103RB (128 КБ) вторая возможность не влезает вместе с двумя слотами,
// поэтому по умолчанию она ВЫКЛЮЧЕНА: обновление принимает приложение (в нём
// тот же сервис OTA), а загрузчик грузит активированный слот, считает попытки и
// умеет откатиться. Включайте её (-DMCFRAME_BOOTLOADER_OTA=ON) там, где Flash
// хватает: тогда прошивку можно залить и в устройство, которое приложение
// которого не поднимается вовсе.
//
// Это НЕ две реализации: при включённой возможности используется тот же
// ota::Service, что и в приложении.
#ifndef MCFRAME_BOOTLOADER_OTA
#define MCFRAME_BOOTLOADER_OTA 0
#endif

#if MCFRAME_BOOTLOADER_OTA
  #include "os/boot/boot_app.hpp"     // fill_ota_config: адреса слотов и активный слот
  #include "os/ota/ota_transport.hpp"
  #ifdef MCFRAME_TARGET_MCU
    #include "uart_stm32.hpp"
using BootUart = embed::stm32::UartDriver1;
  #else
    #include "uart_mock.hpp"
using BootUart = embed::mock::UartMockPolling;
  #endif
#endif

namespace {

using namespace embed;

/// Носитель — встроенная Flash, окно = вся она: адреса слотов приходят из
/// раскладки и лежат в её адресном пространстве.
ota::InternalFlashStorage g_flash;

#if MCFRAME_BOOTLOADER_OTA
/// Интерфейс приёма (нужен только когда загрузчик принимает образ сам).
ota::StreamTransport<BootUart> g_link{BootUart::instance(), ota::proto::Iface::Uart,
                                      0u, "boot-uart"};

/// Мостик «окно ожидания -> приёмник».
///
/// Окно открывает загрузчик (os/boot), а принимает сервис OTA — и связывает их
/// именно эта пара колбэков. Так политика загрузки не зависит от протокола:
/// образ без приёмника просто не передаёт колбэки, и окно ему не нужно.
void host_poll(void* /*ctx*/, uint32_t budget_ms) noexcept {
    (void)ota::Service::instance().poll(budget_ms);
}

bool host_active(void* /*ctx*/) noexcept {
    return ota::Service::instance().session_active();
}
#endif

/// Раскладка: одна на загрузчик, приложение и линкер. Числа приходят из CMake
/// (cmake/firmware_layout.cmake) — из того же места, откуда их берёт линкер.
constexpr boot::Layout kLayout = boot::kBoard;

/// Сколько миллисекунд после сброса ждать хоста.
///
/// Окно имеет смысл только когда загрузчик принимает образ сам: если приём
/// умеет лишь приложение, ждать в загрузчике нечего — там некому отвечать.
/// 500 мс — компромисс: хост-инструмент, который «просто заливает прошивку»,
/// открывает порт и сразу шлёт INFO, и этого хватает; устройство же без хоста
/// теряет полсекунды на каждой загрузке.
#if MCFRAME_BOOTLOADER_OTA
constexpr uint32_t kWindowMs = 500u;
#else
constexpr uint32_t kWindowMs = 0u;
#endif

/// Шаг цикла и бюджет приёма, мс.
constexpr uint32_t kStepMs = 100u;

#if MCFRAME_BOOTLOADER_OTA
/// Поднять время: без него не работают таймауты кадров и сессии.
/// Обработчик SysTick уже определён платформой (он вызывает OsTimer::TickIsr),
/// поэтому здесь достаточно Init + Start.
Status init_time() noexcept {
    config::OsTimer::Config cfg{};
    cfg.min_tick_ms     = 1;
    cfg.max_tick_ms     = 1000;
    cfg.default_tick_ms = 1;
    cfg.alarm_tick_ms   = 1000;
    cfg.max_catchup     = 3;
    if (config::OsTimer::Init(cfg) != Status::Ok) return Status::Fault;
    return config::OsTimer::Start();
}

/// Настроить интерфейс приёма. Драйвер работает опросом: загрузчик не должен
/// зависеть от прерываний периферии — перед прыжком он их всё равно запретит.
void init_link() noexcept {
    BootUart::Config cfg{};
    cfg.baudRate = 115200u;
    (void)BootUart::instance().configure(cfg);
    (void)BootUart::instance().open();
}

/// Настройка сервиса OTA для загрузчика: ГДЕ хранить и ПО ЧЕМУ принимать.
Status init_ota(const boot::Config& bcfg) noexcept {
    ota::Config cfg{};
    // Адреса слотов и метаданных — из общей раскладки. Активного слота у
    // загрузчика нет (он не приложение), поэтому boot_check сам найдёт
    // активированный слот.
    const Status fst = boot::fill_ota_config(cfg, bcfg);
    if (fst != Status::Ok && fst != Status::NotFound) return fst;

    cfg.transport           = &g_link;
    cfg.fw_version          = 0u;         // версия кода загрузчика — не образа
    cfg.checkpoint_bytes    = 4096u;      // сессия в загрузчике короткая
    cfg.session_timeout_ms  = 5000u;      // хост молчит — не держим устройство
    cfg.keep_awake          = false;      // супервизора энергосбережения нет
    cfg.use_watchdog_stall  = false;      // и сторожа нет
    cfg.erase_stall_ms      = 0u;
    return ota::Service::instance().init(cfg);
}
#endif

} // namespace

#ifndef MCFRAME_BOOT_LOGS
#define MCFRAME_BOOT_LOGS 1
#endif
#if MCFRAME_BOOT_LOGS
#define BOOT_LOG(s) platform_log_line(s)
#else
#define BOOT_LOG(s) ((void)0)
#endif

int main() {
    using namespace embed;

    // ── 0. Таблица векторов — на свой образ ──
    // У загрузчика она и так с начала Flash, но вызов симметричен приложению и
    // защищает от случая «сброс без сброса VTOR» (например выход из
    // приложения, решившего перезагрузиться программно).
    (void)boot::relocate_vectors_to_self();

    // ── 1. Контракт платформы ──
    if (!platform::validate()) {
        BOOT_LOG("BOOT: platform contract incomplete");
        return 1;
    }

#if MCFRAME_BOOTLOADER_OTA
    // ── 2. Время: без него не работают таймауты протокола ──
    if (init_time() != Status::Ok) {
        BOOT_LOG("BOOT: timer init failed");
        return 1;
    }
#endif

    // ── 3. Загрузчик: раскладка и носитель ──
    boot::Config bcfg{};
    bcfg.storage   = &g_flash;
    bcfg.layout    = kLayout;
    bcfg.window_ms = kWindowMs;
#if MCFRAME_BOOTLOADER_OTA
    // Окно ожидания обслуживает приёмник (см. host_poll/host_active).
    bcfg.host_poll   = &host_poll;
    bcfg.host_active = &host_active;
#endif

    g_flash.set_window(0u, 0u);          // окно = вся встроенная Flash

    if (boot::Loader::instance().init(bcfg) != Status::Ok) {
        // Раскладка неверна — грузить нечего и стирать нельзя: это состояние
        // исправляют прошивкой, а не на бегу. Стоим и сообщаем.
        BOOT_LOG("BOOT: layout rejected");
        return 1;
    }

    // ── 4. Приём обновления (если загрузчик его умеет) ──
#if MCFRAME_BOOTLOADER_OTA
    if (init_ota(bcfg) != Status::Ok) {
        BOOT_LOG("BOOT: ota init failed");
        // Без приёма загрузчик всё ещё может загрузить то, что уже лежит.
    }
    init_link();
    BOOT_LOG("BOOT: ready");
#else
    // Приём выполняет приложение: у загрузчика для него нет ни Flash, ни места.
    // Загрузчику остаётся решить, кого грузить, — и он это делает сразу.
    BOOT_LOG("BOOT: ready (receive in application)");
#endif

#ifndef MCFRAME_TARGET_MCU
    // ── Стенд: прогоняем несколько шагов и выходим ──
    //
    // На хосте «прыжка» нет (он моделируется), а вечный цикл сделал бы прогон
    // бесконечным. Несколько шагов достаточно, чтобы увидеть решение и
    // обслужить хост, если он подключён (tests/usb-стенд не нужен).
    for (uint32_t i = 0u; i < 8u; ++i) {
        boot::Decision dec{};
        const Status st = boot::Loader::instance().step(dec, 10u);
        if (st == Status::Ok && dec.action == boot::Action::Load) {
            boot::Loader::instance().jump(dec);
            break;
        }
#if MCFRAME_BOOTLOADER_OTA
        (void)ota::Service::instance().poll(10u);
#else
        break;      // принимать нечего: решение уже принято
#endif
    }
    BOOT_LOG("BOOT: stand run finished");
    return 0;
#else
    // ── МКУ: цикл до перехода ──
    for (;;) {
        boot::Decision dec{};
        const Status st = boot::Loader::instance().step(dec, kStepMs);

        if (st == Status::Ok && dec.action == boot::Action::Load) {
            const Status jst = boot::Loader::instance().jump(dec);
            // Сюда попадаем, только если платформа отказала в переходе:
            // на МКУ управление уже принадлежит загруженному образу.
            (void)jst;
            BOOT_LOG("BOOT: jump refused");
        } else {
#if MCFRAME_BOOTLOADER_OTA
            // Грузить нечего (или открыто окно): принимаем обновление.
            (void)ota::Service::instance().poll(kStepMs);
#else
            // Грузить нечего, а принять образ загрузчик не умеет: остаёмся
            // здесь и ждём сброса после того, как обновление примет приложение
            // (или после прошивки программатором).
            for (volatile uint32_t i = 0u; i < 100000u; ++i) { }
#endif
        }
    }
#endif
}
