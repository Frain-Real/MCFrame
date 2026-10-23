// tests/test_boot.cpp
// ---------------------------------------------------------------------------
// ЗАГРУЗЧИК: что грузить, можно ли это грузить и куда передаётся управление.
//
// ЧТО ЗДЕСЬ ГЛАВНОЕ
// Загрузчик — единственная подсистема, ошибка в которой оставляет устройство
// без работающей прошивки: неверное решение означает либо «прыжок в мусор»
// (отказ без диагностики), либо вечный цикл перезагрузок. Поэтому проверяется
// не «функция вернула Ok», а решения и их основания:
//   * что грузим, когда в слотах ничего нет / лежит мусор / лежит годный образ;
//   * считается ли попытка пробной загрузки и когда происходит откат;
//   * что происходит при порче образа ПОСЛЕ того, как он был признан годным;
//   * куда именно передаётся управление (адрес, VTOR, запрет прерываний);
//   * как ведёт себя окно ожидания хоста.
//
// РОЛИ
//   * носитель — модель встроенной Flash mock-платформы (страница 1 КБ, запись
//     только «в стёртое»): та же, что используется тестами OTA;
//   * «прыжок» — модель платформы (platforms/mock/boot_mock.hpp): она
//     запоминает адрес, VTOR и то, что прерывания были запрещены. На хосте
//     настоящий переход невозможен, но проверить его параметры — можно;
//   * метки — общий SlotStore (os/ota/ota_storage.hpp), который также
//     используется сервисом OTA: «два источника правды» здесь недопустимы.
// ---------------------------------------------------------------------------
#include "os/boot/boot.hpp"
#include "os/boot/boot_app.hpp"
#include "os/config/time_config.hpp"
#include "os/crypto/sha2.hpp"
#include "os/ota/ota.hpp"
#include "os/power/power.hpp"

#include "boot_mock.hpp"
#include "flash_mock.hpp"
#include "platform_power_mock.hpp"
#include "platform_watchdog_mock.hpp"   // резервные регистры (вторая память о попытках)

#include <cstdio>
#include <cstring>

using namespace embed;
using namespace embed::boot;

namespace {

int g_failed = 0;
int g_total  = 0;

void check(const char* what, bool ok) {
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

template <typename T>
void check_eq(const char* what, T got, T expected) {
    const bool ok = (got == expected);
    ++g_total;
    if (!ok) ++g_failed;
    (void)std::printf("  [%s] %-52s got=%lld expected=%lld\n",
                      ok ? "OK  " : "FAIL", what,
                      static_cast<long long>(got), static_cast<long long>(expected));
}

// ────────────────────────────── Раскладка ──────────────────────────────────
//
// Та же схема, что на плате (см. boot_layout.hpp): загрузчик, два слота,
// метаданные. Размеры приходят из CMake и совпадают с линкером.
constexpr Layout kLayout = kBoard;

/// Базовый адрес носителя, каким его видит ядро. На стенде модель Flash
/// начинается с нуля (platform::FlashOps::base() == 0), поэтому «адрес слота»
/// и «смещение слота» здесь совпадают. На STM32 база — 0x08000000, и вычисление
/// адреса делает один и тот же код сервиса (Loader::evaluate).
constexpr uint32_t kFlashBase = 0u;

ota::InternalFlashStorage g_flash;
ota::SlotStore       g_store;

Loader& L() { return Loader::instance(); }
mock_platform::FlashModel& F() { return mock_platform::flash_model(); }
mock_platform::BootModel&  B() { return mock_platform::boot_model(); }

// ─────────────────────────── «Образ прошивки» ──────────────────────────────
//
// Настоящий образ начинается с таблицы векторов: первое слово — начальный SP,
// второе — обработчик сброса (с битом Thumb). Именно по этим двум словам
// загрузчик отличает образ от стёртого участка, поэтому «фейковый образ» в
// тесте обязан быть устроен так же.

constexpr uint32_t kImageBody = 768u;          // «тело» прошивки после вектора
constexpr uint32_t kImageSize = 8u + kImageBody;

uint8_t g_body[kImageBody]{};

void make_body(uint8_t seed) {
    uint32_t x = 0x9E3779B9u ^ (static_cast<uint32_t>(seed) << 16);
    for (uint32_t i = 0u; i < kImageBody; ++i) {
        x = x * 1664525u + 1013904223u;
        g_body[i] = static_cast<uint8_t>(x >> 24);
    }
}

/// Положить «образ» в слот: вектор + тело, затем CRC32 всего образа.
uint32_t place_image(uint8_t slot, uint8_t seed) {
    make_body(seed);

    uint8_t image[kImageSize];
    const uint32_t slot_addr = kLayout.slot_offset[slot];

    // Начальный SP — как у настоящей прошивки: верх RAM.
    const uint32_t sp = mock_platform::kRamBase + mock_platform::kRamSize;
    // Обработчик сброса — внутри образа, в режиме Thumb (младший бит 1).
    const uint32_t reset = (kFlashBase + slot_addr + 0x101u) | 1u;

    std::memcpy(image, &sp, 4u);
    std::memcpy(image + 4u, &reset, 4u);
    std::memcpy(image + 8u, g_body, kImageBody);

    F().preload(slot_addr, image, kImageSize);
    return ota::proto::image_crc(image, kImageSize);
}

/// Записать метку слота (то, что оставило бы приложение или загрузчик).
void put_record(uint8_t slot, ota::proto::State state, uint32_t crc,
                uint32_t size = kImageSize, uint32_t attempts = 0u,
                uint8_t hash_kind = 0u, const uint8_t* sha = nullptr) {
    ota::SlotRecord rec{};
    rec.slot = slot;
    rec.state = static_cast<uint8_t>(state);
    rec.image_size = size;
    rec.image_crc32 = crc;
    rec.stored_bytes = size;
    rec.crc_at_stored = crc;
    rec.boot_attempts = attempts;
    rec.hash_kind = hash_kind;
    if (sha != nullptr) std::memcpy(rec.sha256, sha, 32u);
    if (ota::proto::state_is_loadable(state)) rec.flags = ota::kRecFlagCrcOk;
    (void)g_store.store(slot, rec);
}

bool read_record(uint8_t slot, ota::SlotRecord& out) {
    return g_store.load(slot, out) == Status::Ok;
}

/// Мир «с нуля»: стёртая Flash, сброшенные модели, загрузчик инициализирован.
Config make_cfg(uint32_t window_ms = 0u, uint32_t max_attempts = 3u) {
    Config c{};
    c.storage = &g_flash;
    c.layout  = kLayout;
    c.window_ms = window_ms;
    c.max_boot_attempts = max_attempts;
    return c;
}

void reset_world(const Config& cfg) {
    F().reset();
    B().reset();
    // Резервные регистры — «вторая память» о попытках загрузки: они переживают
    // сброс НАСТОЯЩЕГО устройства, поэтому между сценами их нужно очищать
    // отдельно (иначе счётчик из прошлого сценария выглядел бы как «устройство
    // уже пыталось грузить эту прошивку»).
    mock_platform::watchdog_model().reset();
    const Status st = g_store.init(&g_flash, kLayout.meta_offset);
    (void)st;
    const Status is = L().init(cfg);
    (void)is;
}

/// «Загружено загрузчиком в слот»: так приложение узнаёт свой слот (VTOR).
void simulate_running_from(uint8_t slot) {
    B().vectors = kFlashBase + kLayout.slot_offset[slot];
}

// ───────────────────────── 1. Ничего грузить нельзя ────────────────────────

void test_no_image() {
    (void)std::puts("пустые слоты:");

    const Config cfg = make_cfg();
    reset_world(cfg);

    Decision d{};
    check("init загрузчика", L().init(cfg) == Status::Ok);
    check("решение принято", L().evaluate(d) == Status::Ok);
    check("остаёмся в загрузчике", d.action == Action::Stay);
    check_eq<uint32_t>("причина: образа нет",
                       static_cast<uint32_t>(d.reason),
                       static_cast<uint32_t>(Reason::NoImage));
    check("грузить нечего", !d.can_load());
    check("прыжка не было", B().jumps == 0u);

    // Прыжок «в никуда» обязан быть отвергнут и на уровне API.
    Decision zero{};
    check("прыжок по нулевому адресу отвергнут", L().jump(zero) == Status::InvalidArg);
}

// ───────────────────── 2. Годный образ: переход и его параметры ────────────

void test_load_valid_image() {
    (void)std::puts("годный образ: решение и параметры перехода:");

    const Config cfg = make_cfg();
    reset_world(cfg);

    const uint32_t crc = place_image(0u, 1u);
    put_record(0u, ota::proto::State::Verified, crc);

    Decision d{};
    check("решение", L().evaluate(d) == Status::Ok);
    check("грузим", d.action == Action::Load);
    check_eq<uint32_t>("слот", d.slot, 0u);
    check_eq<uint32_t>("адрес образа",
                       d.address,
                       kFlashBase + kLayout.slot_offset[0]);
    check_eq<uint32_t>("размер образа", d.image_size, kImageSize);
    check("загрузка не пробная", !d.trial);
    check_eq<uint32_t>("причина: образ годен",
                       static_cast<uint32_t>(d.reason),
                       static_cast<uint32_t>(Reason::ImageOk));

    // ── Переход ──
    check("переход", L().jump(d) == Status::Ok);
    check_eq<uint32_t>("адрес перехода", B().jumped_to, d.address);
    check_eq<uint32_t>("таблица векторов переставлена", B().vectors, d.address);
    check("прерывания запрещены до перехода", B().irq_disabled);
    check("системный тик остановлен", B().systick_stopped);

    // Повторный evaluate того же мира не должен ничего менять.
    Decision d2{};
    (void)L().evaluate(d2);
    check("после перехода решение не изменилось", d2.action == Action::Load);
}

// ─────────────────── 3. Порча образа после «проверено» ─────────────────────

void test_image_damaged_after_verify() {
    (void)std::puts("образ испорчен ПОСЛЕ проверки:");

    const Config cfg = make_cfg();
    reset_world(cfg);

    const uint32_t crc = place_image(0u, 2u);
    put_record(0u, ota::proto::State::Verified, crc);

    // Носитель испортился (износ, сбой питания при записи соседнего блока).
    F().poke(kLayout.slot_offset[0] + 300u,
             static_cast<uint8_t>(g_body[292] ^ 0x5Au));

    Decision d{};
    check("решение", L().evaluate(d) == Status::Ok);
    check("грузить нельзя", d.action == Action::Stay);
    check_eq<uint32_t>("причина: образ негоден",
                       static_cast<uint32_t>(d.reason),
                       static_cast<uint32_t>(Reason::BadImage));
    check_eq<uint32_t>("отвергнуто по CRC", L().stats().bad_crc, 1u);

    // Метка обязана перестать говорить «годен»: иначе загрузчик возвращался бы
    // к этому слоту на каждой загрузке, а счётчик попыток этого не ловит.
    ota::SlotRecord rec{};
    check("запись слота перечитана", read_record(0u, rec));
    check("слот помечен негодным", !rec.verified());

    // ── Стёртый вектор: тот же путь, другая причина ──
    reset_world(cfg);
    const uint32_t crc2 = place_image(1u, 3u);
    put_record(1u, ota::proto::State::Verified, crc2);
    // Стираем первые 8 байт (вектор) — так выглядит «слот, который стёрли,
    // а метку снять не успели».
    F().erase(kLayout.slot_offset[1], 1024u);

    const uint32_t vec_before = L().stats().bad_vectors;
    Decision d2{};
    check("решение", L().evaluate(d2) == Status::Ok);
    check_eq<uint32_t>("причина: вектор не проходит",
                       static_cast<uint32_t>(d2.reason),
                       static_cast<uint32_t>(Reason::BadVector));
    check_eq<uint32_t>("отвергнуто по вектору (приращение)",
                       L().stats().bad_vectors - vec_before, 1u);
}

// ─────────── Резервная память о попытках: резервные регистры ──────────────
//
// Счётчик попыток живёт в носителе, но если запись в носитель не удалась,
// защита от вечного цикла перезагрузок сломалась бы МОЛЧА. Поэтому вторым
// источником служат регистры, переживающие сброс: загрузчик пишет туда то же
// число, а читатель берёт большее из двух.

/// Смещение резервных регистров по умолчанию: то же, что в ota::Config.
constexpr uint32_t ocfg_default_note_base() noexcept { return 5u; }

void test_attempts_note_fallback() {
    (void)std::puts("резервный счётчик попыток (регистры, переживающие сброс):");

    const Config cfg = make_cfg(0u, 3u);
    reset_world(cfg);

    const uint32_t crc = place_image(1u, 12u);
    put_record(1u, ota::proto::State::Activated, crc, kImageSize, /*attempts*/ 0u);

    // Прошлая загрузка дошла до предела и оставила отметку в регистрах, а
    // запись в носитель не удалась (например, носитель отказал).
    // Индексы — со смещением note_base (по умолчанию 5: регистры 0..4 заняты
    // сторожевым таймером), ровно так же, как их пишет сам сервис.
    const uint32_t base = ocfg_default_note_base();
    mock_platform::WatchdogModel& wm = mock_platform::watchdog_model();
    wm.notes[base + ota::kNoteIdxMagic]    = ota::kNoteMagic;
    wm.notes[base + ota::kNoteIdxAttempts] = 4u;

    // «Мы исполняемся из слота B»: активный слот берётся из VTOR, поэтому
    // приложение обязано видеть именно B.
    simulate_running_from(1u);

    ota::Config ocfg{};
    check("настройка OTA из раскладки", fill_ota_config(ocfg, cfg) == Status::Ok);
    ocfg.transport = nullptr;
    ocfg.keep_awake = false;
    ocfg.use_watchdog_stall = false;
    check_eq<uint32_t>("активный слот = B", ocfg.active_slot, 1u);
    check("сервис OTA поднят", ota::Service::instance().init(ocfg) == Status::Ok);

    ota::BootInfo bi{};
    check("boot_check", ota::Service::instance().boot_check(bi) == Status::Ok);
    check("счётчик взят из регистров, а не обнулён меткой",
          bi.boot_attempts >= 4u);
    check("попытки исчерпаны -> нужен откат", bi.rollback_needed);

    ota::Service::instance().shutdown();
}

// ───────────────── 4. Пробные загрузки, откат и «кирпич» ───────────────────

void test_trial_and_rollback() {
    (void)std::puts("пробная загрузка, счётчик попыток и откат:");

    const Config cfg = make_cfg(0u, /*max_attempts*/ 2u);
    reset_world(cfg);

    // В слоте A — работающая (подтверждённая) прошивка, в слоте B — новая,
    // активированная хостом: именно её загрузчик и обязан грузить.
    const uint32_t crc_a = place_image(0u, 4u);
    const uint32_t crc_b = place_image(1u, 5u);
    put_record(0u, ota::proto::State::Running, crc_a);
    put_record(1u, ota::proto::State::Activated, crc_b);

    Decision d{};
    check("решение", L().evaluate(d) == Status::Ok);
    check("грузим активированный слот", d.action == Action::Load && d.slot == 1u);
    check("загрузка пробная", d.trial);
    check_eq<uint32_t>("попытка №1", d.attempts, 1u);

    ota::SlotRecord rec{};
    (void)read_record(1u, rec);
    check_eq<uint32_t>("счётчик попыток записан в носитель", rec.boot_attempts, 1u);

    // Прошивка «не подтвердила себя»: сброс за сбросом.
    Decision d2{};
    check("вторая попытка", L().evaluate(d2) == Status::Ok && d2.attempts == 2u);
    check("вторая попытка всё ещё пробная", d2.trial);

    Decision d3{};
    check("третья попытка", L().evaluate(d3) == Status::Ok);
    check("попытки исчерпаны -> откат", d3.slot == 0u && d3.action == Action::Load);
    check("загрузка НЕ пробная (прежний слот подтверждён)", !d3.trial);
    check_eq<uint32_t>("откатов", L().stats().rollbacks, 1u);

    (void)read_record(0u, rec);
    check("прежний (подтверждённый) слот остался проверенным, а не «пробным»",
          rec.state == static_cast<uint8_t>(ota::proto::State::Verified));
    (void)read_record(1u, rec);
    check("сломанный слот снят с загрузки",
          !ota::proto::state_is_loadable(static_cast<ota::proto::State>(rec.state)));

    // ── Откатываться некуда ──
    reset_world(cfg);
    const uint32_t crc_b2 = place_image(1u, 6u);
    put_record(1u, ota::proto::State::Activated, crc_b2);

    // Попытки исчерпываются на третьем решении (max_attempts = 2): именно там
    // загрузчик обязан попытаться откатиться и обнаружить, что некуда.
    Decision exhausted{};
    for (uint32_t i = 0u; i < 3u; ++i) {
        (void)L().evaluate(exhausted);
    }
    check("попытки исчерпаны в одиночном слоте",
          exhausted.reason == Reason::NoRollback && exhausted.action == Action::Stay);
}

// ─────────────── 5. Как приложение узнаёт свой слот и подтверждает загрузку ──

void test_app_side() {
    (void)std::puts("приложение в слоте: свой слот, подтверждение загрузки:");

    const Config cfg = make_cfg();
    reset_world(cfg);

    const uint32_t crc = place_image(1u, 7u);
    put_record(1u, ota::proto::State::Activated, crc, kImageSize, /*attempts*/ 1u);

    // Загрузчик передал управление в слот B — то есть VTOR указывает туда.
    simulate_running_from(1u);
    check_eq<uint32_t>("приложение видит свой слот", L().current_slot(), 1u);

    // Настройка OTA для приложения: адреса и активный слот — из общего места.
    ota::Config ocfg{};
    check("fill_ota_config", fill_ota_config(ocfg, cfg) == Status::Ok);
    check_eq<uint32_t>("активный слот = слот приложения", ocfg.active_slot, 1u);
    check_eq<uint32_t>("адрес слота A из раскладки", ocfg.slot_offset[0], kLayout.slot_offset[0]);
    check_eq<uint32_t>("адрес метаданных из раскладки", ocfg.meta_offset, kLayout.meta_offset);

    // Подтвердиться без поднятого сервиса OTA нельзя: подтверждать нечем.
    check("confirm_self без сервиса OTA", confirm_self(cfg) == Status::Unavailable);

    ocfg.transport = nullptr;                 // приём в этом тесте не нужен
    ocfg.keep_awake = false;
    ocfg.use_watchdog_stall = false;
    check("сервис OTA поднят", ota::Service::instance().init(ocfg) == Status::Ok);

    check("подтверждение своей загрузки", confirm_self(cfg) == Status::Ok);

    ota::SlotRecord rec{};
    (void)read_record(1u, rec);
    check("слот подтверждён (Running)",
          rec.state == static_cast<uint8_t>(ota::proto::State::Running));
    check_eq<uint32_t>("счётчик попыток сброшен", rec.boot_attempts, 0u);

    // Приложение, работающее ВНЕ слота, не должно «подтверждать» чужую загрузку.
    simulate_running_from(0u);
    B().vectors = 0u;                          // образ вне слотов
    check_eq<uint32_t>("вне слотов слот не определяется",
                       L().current_slot(), static_cast<uint32_t>(Layout::kNoSlot));
    check("fill_ota_config вне слотов -> NotFound",
          fill_ota_config(ocfg, cfg) == Status::NotFound);
    check("confirm_self вне слотов -> NotFound", confirm_self(cfg) == Status::NotFound);

    ota::Service::instance().shutdown();
}

// ────────────────── 6. Окно ожидания хоста и SHA-256 ───────────────────────

uint32_t g_host_polls = 0u;
bool     g_host_busy  = false;

void host_poll_stub(void* /*ctx*/, uint32_t /*budget*/) noexcept { ++g_host_polls; }
bool host_active_stub(void* /*ctx*/) noexcept { return g_host_busy; }

void test_window_and_hash() {
    (void)std::puts("окно ожидания хоста и проверка по SHA-256:");

    // ── SHA-256: если он записан вместе с образом, он тоже проверяется ──
    {
        Config cfg = make_cfg();
        reset_world(cfg);

        const uint32_t crc = place_image(0u, 8u);

        uint8_t sha[32]{};
        crypto::Sha2<crypto::Sha2Variant::Sha256> h;
        uint8_t image[kImageSize];
        F().read(kLayout.slot_offset[0], image, kImageSize);
        h.update(image, kImageSize);
        h.finalize(sha);

        put_record(0u, ota::proto::State::Verified, crc, kImageSize, 0u,
                   static_cast<uint8_t>(ota::proto::HashKind::Sha256), sha);

        Decision d{};
        (void)L().evaluate(d);
        check("образ с верным SHA-256 грузится", d.action == Action::Load);

        // Тот же образ, но метка помнит ДРУГОЙ хеш: доверять нельзя.
        put_record(0u, ota::proto::State::Verified, crc, kImageSize, 0u,
                   static_cast<uint8_t>(ota::proto::HashKind::Sha256), sha);
        ota::SlotRecord rec{};
        (void)read_record(0u, rec);
        rec.sha256[0] = static_cast<uint8_t>(rec.sha256[0] ^ 0xFFu);
        (void)g_store.store(0u, rec);

        Decision d2{};
        (void)L().evaluate(d2);
        check("неверный SHA-256 -> не грузим", d2.action == Action::Stay);
        check_eq<uint32_t>("причина: образ негоден",
                           static_cast<uint32_t>(d2.reason),
                           static_cast<uint32_t>(Reason::BadImage));
    }

    // ── Окно ожидания хоста ──
    //
    // Окно нужно ровно для одного случая: обновление начинают СРАЗУ после
    // сброса, и уходить в приложение на полпути нельзя.
    {
        Config cfg = make_cfg(/*window_ms*/ 200u);
        reset_world(cfg);
        g_host_polls = 0u;
        g_host_busy = false;

        const uint32_t crc = place_image(0u, 9u);
        put_record(0u, ota::proto::State::Running, crc);

        Config cfg2 = cfg;
        cfg2.host_poll = &host_poll_stub;
        cfg2.host_active = &host_active_stub;
        check("init с окном", L().init(cfg2) == Status::Ok);

        check("таймер ОС запущен", config::OsTimer::Start() == Status::Ok);

        // Хост НЕ пришёл: окно истекает, и загрузчик уходит в приложение.
        Decision d{};
        bool saw_wait = false;
        for (uint32_t i = 0u; i < 300u; ++i) {
            config::OsTimer::TickIsr();
            (void)L().step(d, 10u);
            if (d.action == Action::Wait) saw_wait = true;
            if (d.reason == Reason::WindowExpired) break;
        }
        check("пока окно открыто — ждём, а не грузим", saw_wait);
        check("окно истекло -> грузим", d.action == Action::Load);
        check_eq<uint32_t>("причина: окно истекло",
                           static_cast<uint32_t>(d.reason),
                           static_cast<uint32_t>(Reason::WindowExpired));
        check("приём опрашивался", g_host_polls > 0u);

        // Хост РАБОТАЕТ: загрузчик обязан остаться и обслужить сессию.
        reset_world(cfg2);
        const uint32_t crc2 = place_image(0u, 10u);
        put_record(0u, ota::proto::State::Running, crc2);
        (void)L().init(cfg2);
        g_host_busy = true;

        Decision d2{};
        (void)L().step(d2, 10u);
        check("хост работает -> остаёмся в загрузчике",
              d2.action == Action::Stay && d2.reason == Reason::HostSession);
        check_eq<uint32_t>("сессия посчитана", L().stats().sessions, 1u);

        // Хост закончил (образ принят и активирован): загрузчик обязан
        // ПЕРЕСМОТРЕТЬ решение, а не остаться в приёме навсегда — так принятая
        // прошивка попадает в загрузку без «нажми reset».
        g_host_busy = false;
        Decision d3{};
        for (uint32_t i = 0u; i < 300u; ++i) {
            config::OsTimer::TickIsr();
            (void)L().step(d3, 10u);
            if (d3.action == Action::Load) break;
        }
        check("после окончания сессии решение пересмотрено и грузим",
              d3.action == Action::Load && d3.slot == 0u);
    }
}

// ─────── 8. Образ без метки: первая прошивка и заливка по SWD ──────────────
//
// У только что собранного устройства метаданных НЕТ: прошивка залита
// программатором. Если бы загрузчик в этом состоянии ничего не грузил,
// устройство осталось бы в загрузчике навсегда (а при выключенном приёме OTA —
// и вовсе без способа что-то залить). Поэтому «похожий на образ» слот
// допускается к загрузке со счётчиком попыток в резервных регистрах: не
// подтвердив себя, он будет помечен негодным.

void test_unrecorded_image() {
    (void)std::puts("образ без метки (первая прошивка):");

    const Config cfg = make_cfg(0u, 2u);
    reset_world(cfg);

    (void)place_image(0u, 13u);            // образ есть, метки нет

    Decision d{};
    check("решение", L().evaluate(d) == Status::Ok);
    check("грузим образ без метки", d.action == Action::Load && d.slot == 0u);
    check_eq<uint32_t>("причина: образ без метки",
                       static_cast<uint32_t>(d.reason),
                       static_cast<uint32_t>(Reason::FactoryImage));
    check("загрузка считается пробной", d.trial);
    check_eq<uint32_t>("попытка №1", d.attempts, 1u);

    // Отметка легла в резервные регистры (метки в носителе нет).
    mock_platform::WatchdogModel& wm = mock_platform::watchdog_model();
    check("отметка о попытке в резервных регистрах",
          wm.notes[cfg.note_base + ota::kNoteIdxMagic] == ota::kNoteMagic &&
          wm.notes[cfg.note_base + ota::kNoteIdxAttempts] == 1u);

    // Вторая попытка — счётчик растёт.
    Decision d2{};
    (void)L().evaluate(d2);
    check_eq<uint32_t>("попытка №2", d2.attempts, 2u);

    // Третья — предел: образ «похож», но не подтвердил себя ни разу.
    Decision d3{};
    (void)L().evaluate(d3);
    check("предел попыток -> не грузим", d3.action == Action::Stay);
    check("причина: образа нет", d3.reason == Reason::NoImage);

    ota::SlotRecord rec{};
    check("слот помечен негодным (метка заведена)",
          read_record(0u, rec) && !rec.verified());

    // ── Подтверждение снимает счётчик ──
    //
    // Приложение, загруженное таким образом и дошедшее до конца инициализации,
    // обязано обнулить этот счётчик: иначе исправная прошивка перестала бы
    // грузиться после нескольких перезагрузок.
    reset_world(cfg);
    (void)place_image(1u, 14u);
    simulate_running_from(1u);

    Decision d4{};
    (void)L().evaluate(d4);
    check("грузим образ без метки из слота B", d4.action == Action::Load && d4.slot == 1u);

    ota::Config ocfg{};
    (void)fill_ota_config(ocfg, cfg);
    ocfg.transport = nullptr;
    ocfg.keep_awake = false;
    ocfg.use_watchdog_stall = false;
    check("сервис OTA поднят", ota::Service::instance().init(ocfg) == Status::Ok);
    check("подтверждение загрузки без метки", confirm_self(cfg) == Status::Ok);
    check_eq<uint32_t>("счётчик попыток в регистрах обнулён",
                       mock_platform::watchdog_model().notes[cfg.note_base + ota::kNoteIdxAttempts],
                       0u);
    ota::Service::instance().shutdown();

    // ── Перемещение таблицы векторов на свой образ ──
    //
    // На стенде символа линкера `_os_image_base` нет (хостовый образ
    // линкуется обычным способом), поэтому функция обязана честно сказать
    // «адрес неизвестен» и НИЧЕГО не менять — иначе она записала бы в VTOR ноль.
    const uint32_t vectors_before = B().vectors;
    const Status rst = relocate_vectors_to_self();
    check("без символа линкера адрес образа неизвестен",
          rst == Status::NotFound);
    check_eq<uint32_t>("VTOR не тронут", B().vectors, vectors_before);
    check_eq<uint32_t>("и адрес образа не выдуман", linked_image_base(), 0u);

    // ── Политика «только то, что принято по OTA» ──
    Config strict = make_cfg();
    strict.allow_unrecorded_image = false;
    reset_world(strict);
    (void)place_image(0u, 15u);
    Decision d5{};
    (void)L().evaluate(d5);
    check("запрет грузить без метки -> остаёмся",
          d5.action == Action::Stay && d5.reason == Reason::NoImage);
}

// ───────────────────── 7. Раскладка и защита от «прыжка в себя» ────────────

void test_layout_guard() {
    (void)std::puts("раскладка: непригодные настройки отвергаются:");

    reset_world(make_cfg());

    Config bad = make_cfg();
    bad.storage = nullptr;
    check("нет носителя -> InvalidArg", L().init(bad) == Status::InvalidArg);

    // Слот, налезающий на загрузчик: первая же запись в слот стёрла бы его код.
    bad = make_cfg();
    bad.layout.slot_offset[0] = bad.layout.boot_size / 2u;
    check("слот внутри загрузчика -> InvalidArg", L().init(bad) == Status::InvalidArg);

    // Слоты друг на друге.
    bad = make_cfg();
    bad.layout.slot_offset[1] = bad.layout.slot_offset[0];
    check("слоты перекрываются -> InvalidArg", L().init(bad) == Status::InvalidArg);

    // Метаданные внутри слота: стирание метки уничтожило бы образ.
    bad = make_cfg();
    bad.layout.meta_offset = bad.layout.slot_offset[1] + 1024u;
    check("метаданные внутри слота -> InvalidArg", L().init(bad) == Status::InvalidArg);

    check("корректная раскладка принимается", L().init(make_cfg()) == Status::Ok);

    // Платформа может отказать в переходе (например образ не прошёл проверку
    // на её стороне) — это обязано быть видно, а не выглядеть как успех.
    B().jump_fails = true;
    Decision d{};
    const uint32_t crc = place_image(0u, 11u);
    put_record(0u, ota::proto::State::Verified, crc);
    (void)L().evaluate(d);
    const uint32_t jumps_before = L().stats().jumps;
    check("платформа отказала в переходе", L().jump(d) == Status::HardwareError);
    check_eq<uint32_t>("прыжков не засчитано",
                       L().stats().jumps - jumps_before, 0u);
}


void test_signature_policy() {
    (void)std::puts("политика подписи:");

    const Config base = make_cfg();
    reset_world(base);
    const uint32_t crc = place_image(0u, 23u);
    put_record(0u, ota::proto::State::Verified, crc);

    Config signed_cfg = base;
    signed_cfg.require_signature = true;
    signed_cfg.allow_unrecorded_image = false;
    check("signed config accepted", L().init(signed_cfg) == Status::Ok);

    Decision d{};
    check("unsigned record rejected", L().evaluate(d) == Status::Ok &&
          d.action == Action::Stay && d.reason == Reason::UnsignedImage);

    ota::SlotRecord rec{};
    check("record still readable", g_store.load(0u, rec) == Status::Ok);
    rec.state = static_cast<uint8_t>(ota::proto::State::Verified);
    rec.flags |= ota::kRecFlagSignOk;
    check("mark signed record", g_store.store(0u, rec) == Status::Ok);

    check("reinit signed", L().init(signed_cfg) == Status::Ok);
    check("signed record loads", L().evaluate(d) == Status::Ok && d.can_load() &&
          d.slot == 0u);
}

} // namespace

int main() {
    (void)std::puts("=== Загрузчик: решения и переход ===");

    config::OsTimer::Config tcfg{};
    tcfg.min_tick_ms = 1; tcfg.max_tick_ms = 1000;
    tcfg.default_tick_ms = 1; tcfg.alarm_tick_ms = 1000; tcfg.max_catchup = 3;
    if (config::OsTimer::Init(tcfg) != Status::Ok) {
        (void)std::puts("FAIL: timer init");
        return 1;
    }
    power::Config pcfg{};
    pcfg.allow_deep_sleep = false;
    (void)power::Supervisor::instance().init(pcfg);

    g_flash.set_window(0u, 0u);            // окно = вся встроенная Flash

    test_no_image();
    test_load_valid_image();
    test_image_damaged_after_verify();
    test_attempts_note_fallback();
    test_trial_and_rollback();
    test_app_side();
    test_window_and_hash();
    test_unrecorded_image();
    test_layout_guard();
    test_signature_policy();

    (void)std::printf("\nвсего проверок: %d, провалов: %d\n", g_total, g_failed);
    if (g_failed != 0) (void)std::puts("FAIL");
    return (g_failed == 0) ? 0 : 1;
}
