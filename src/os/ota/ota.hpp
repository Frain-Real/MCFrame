// os/ota/ota.hpp
#pragma once
// ---------------------------------------------------------------------------
// OTA: ПРИЁМ НОВОЙ ПРОШИВКИ, ЕЁ ПРОВЕРКА И АКТИВАЦИЯ.
//
// Модуль отвечает на три вопроса приложения:
//   1. КУДА писать — Config::storage + раскладка (метаданные и два слота);
//   2. ПО ЧЕМУ принимать — Config::transport (UART/USB/память/никак);
//   3. ЧТО считать принятым — протокол в ota_protocol.hpp и правила ниже.
//
// ЧЕГО МОДУЛЬ НЕ ДЕЛАЕТ (сознательно)
//   * не ПЕРЕНОСИТ управление на новую прошивку: на STM32F103 это работа
//     загрузчика (перенести SP, поставить вектор сброса, прыгнуть), и делать
//     это из приложения «наполовину» — верный способ получить устройство,
//     которое не грузится вовсе. Модуль готовит слот и МЕТКАМИ сообщает, что
//     загружать: состояние слота, версия, число попыток. Решение «прыгать»
//     остаётся за тем, кто умеет это делать целиком (см. README, «OTA: приём
//     новой прошивки», раздел про активацию и откат);
//   * не шифрует образ: целостность (CRC32, SHA-256) и аутентичность
//     (подпись) — да, конфиденциальность — нет. Ключи и алгоритмы подписи
//     принадлежат приложению: модуль лишь передаёт ему ровно то, что подписано
//     (см. Config::verify_signature), а не «доверяет флагу в кадре»;
//   * не выделяет память: буферы приёмника/передатчика и состояние сессии —
//     статические поля сервиса, размер известен на этапе сборки
//     (Service::static_bytes).
//
// ─────────────────────── ЧТО ГАРАНТИРУЕТ ПРИЁМ ──────────────────────────────
//
//   * ПОЛНОТА. Слот помечается готовым, только если приняты ВСЕ байты и
//     сошлась CRC32 (при заявленном SHA-256 — и он; при подписи — и она).
//     Обрыв на 99 % даёт ровно то же, что обрыв на 1 %: образ не загрузится;
//   * СОХРАННОСТЬ. Если включена проверка прочитанного (verify_readback),
//     образ перечитывается из носителя и сверяется: «write вернул Ok» и
//     «в памяти лежит то, что писали» — разные утверждения, и на дешёвой
//     Flash второе иногда неверно;
//   * ПРОДОЛЖЕНИЕ. Прерванная передача продолжается с того, что РЕАЛЬНО
//     лежит в слоте (префикс перечитывается и сверяется по CRC32), а не с
//     того, что помнит хост. Проверку можно выключить (verify_resume_prefix),
//     и тогда верить придётся счётчику — это допустимо только на ОЗУ;
//   * РАЗДЕЛЕНИЕ ЦЕЛОСТНОСТИ И АУТЕНТИЧНОСТИ. Между END и SIGN слот уже
//     целый, но ЕЩЁ не активируемый: подпись проверяется отдельным шагом, и
//     до неё образ не помечается готовым;
//   * ЗАЩИТА ОТ «КИРПИЧА». Активируется только проверенный слот; пробная
//     загрузка считается (max_boot_attempts), и если новая прошивка не
//     подтвердила себя (confirm_running), модуль сообщает, что надо вернуться
//     к прежнему слоту (rollback). Отдельно запрещается откат на версию старее
//     работающей (allow_downgrade) — иначе «обновление» старым образом молча
//     возвращало бы устройство назад.
//
// ────────────────────────────── ПОРЯДОК РАБОТЫ ──────────────────────────────
//
//   инициализация (до старта задач):
//       ota::Config cfg{};
//       cfg.storage   = &g_flash;        // куда писать
//       cfg.transport = &g_uart_ota;     // по какому интерфейсу принимать
//       cfg.slot_offset[0] = ...; cfg.slot_capacity[0] = ...;   // раскладка
//       cfg.fw_version = FW_VERSION;
//       ota::Service::instance().init(cfg);
//
//   в задаче (или в цикле опроса интерфейса):
//       ota::Service::instance().poll(10);   // 10 мс — бюджет на приём
//
//   при старте (после инициализации, ДО полезной работы):
//       ota::BootInfo bi{};
//       if (ota::Service::instance().boot_check(bi) == Status::Ok && bi.trial) {
//           ... // эта загрузка пробная: подтвердить себя в конце init()
//       }
//       ...
//       ota::Service::instance().confirm_running();   // «я запустилась и жива»
// ---------------------------------------------------------------------------

#include "ota_protocol.hpp"
#include "ota_storage.hpp"
#include "ota_transport.hpp"

#include "../config/time_config.hpp"      // время для таймаутов сессии
#include "../core/primitives/types.hpp"   // atomic (счётчики читает отчёт)
#include "../crypto/sha2.hpp"             // SHA-256 образа, если он заявлен
#include "../power/power.hpp"             // удержание системы от глубокого сна
#include "../watchdog/watchdog.hpp"       // окно на длительное стирание

namespace embed::ota {

/// Что произошло (повод для приложения: залогировать, перезагрузиться...).
enum class Event : uint8_t {
    SessionStarted = 1u,   ///< сессия открыта (данные можно писать)
    SessionResumed = 2u,   ///< передача продолжена с уже записанного префикса
    ImageVerified  = 3u,   ///< образ принят и проверен целиком
    Activated      = 4u,   ///< слот помечен «грузить при следующей загрузке»
    Failed         = 5u,   ///< сессия провалена (запись/проверка)
    Confirmed      = 6u,   ///< работающая прошивка подтвердила себя
    Erased         = 7u,   ///< слот стёрт
    Rollback       = 8u,   ///< решено вернуться к прежнему слоту
};

inline const char* event_text(Event ev) noexcept {
    switch (ev) {
        case Event::SessionStarted: return "session-started";
        case Event::SessionResumed: return "session-resumed";
        case Event::ImageVerified:  return "image-verified";
        case Event::Activated:      return "activated";
        case Event::Failed:         return "failed";
        case Event::Confirmed:      return "confirmed";
        case Event::Erased:         return "erased";
        case Event::Rollback:       return "rollback";
    }
    return "?";
}

/// Проверка подписи образа. Реализуется приложением: модуль не знает, ECDSA
/// там, Ed25519 или HMAC, и не хранит ключей.
///
/// Подписывается ровно это сообщение (52 байта, LE):
///     "MCFRAMEO" | image_size(4) | image_crc32(4) | fw_version(4) | sha256(32)
/// Тег в начале — защита от подписи «похожего» сообщения из другого протокола.
/// Требуется, чтобы в START был заявлен HashKind::Sha256: подписывать CRC32
/// (линейную и подделываемую) нельзя.
///
/// Возврат: true — подпись верна.
using SignatureVerifier = bool (*)(void* ctx, uint8_t kind,
                                   const uint8_t* message, Size message_len,
                                   const uint8_t* signature, Size signature_len) noexcept;

/// Что известно о загрузке: читается из метаданных при старте.
struct BootInfo {
    bool           valid            = false;   ///< метаданные прочитаны
    uint8_t        slot             = kSlotNone;
    proto::State   state            = proto::State::Idle;
    uint32_t       fw_version       = 0u;
    uint32_t       image_size       = 0u;
    uint32_t       image_crc32      = 0u;
    uint32_t       boot_attempts    = 0u;
    bool           trial            = false;   ///< эта загрузка пробная
    bool           rollback_needed  = false;   ///< попытки исчерпаны: вернуться
};

/// Счётчики сервиса (диагностика и отчёты).
struct Stats {
    uint32_t frames          = 0u;   ///< принято целых кадров
    uint32_t bad_frames      = 0u;   ///< кадров отброшено (CRC/формат)
    uint32_t too_large       = 0u;   ///< кадров не влезло в буфер приёмника
    uint32_t resync_bytes    = 0u;   ///< байт отброшено при восстановлении синхронизации
    uint32_t transport_errors= 0u;   ///< отказов интерфейса
    uint32_t sessions        = 0u;   ///< открыто сессий
    uint32_t resumed         = 0u;   ///< сессий продолжено с префикса
    uint32_t resume_rejected = 0u;   ///< продолжение отвергнуто: префикс не сошёлся
    uint32_t expired         = 0u;   ///< сессий закрыто по простою
    uint32_t aborts          = 0u;   ///< сессий отменено
    uint32_t duplicates      = 0u;   ///< повторных DATA (идемпотентность)
    uint32_t gaps            = 0u;   ///< DATA с пропуском (разрыв)
    uint32_t bytes_written   = 0u;   ///< записано в носитель, байт
    uint32_t page_erases     = 0u;   ///< стёрто страниц
    uint32_t checkpoints     = 0u;   ///< сохранений состояния сессии
    uint32_t verify_ok       = 0u;   ///< образов проверено успешно
    uint32_t verify_fail     = 0u;   ///< образов не прошло проверку
    uint32_t readback_fail   = 0u;   ///< не сошлось прочитанное из носителя
    uint32_t signature_ok    = 0u;
    uint32_t signature_fail  = 0u;
    uint32_t activations     = 0u;
    uint32_t confirms        = 0u;
    uint32_t rollbacks       = 0u;
    uint32_t invalidations   = 0u;   ///< слотов помечено негодными
    uint32_t storage_errors  = 0u;
    uint32_t naks            = 0u;   ///< отправлено отказов
    uint32_t boot_checks     = 0u;
};

/// Настройка сервиса: это и есть ответ на «где хранить» и «по чему принимать».
struct Config {
    // ──────────── КУДА ПИШЕМ ────────────
    /// Носитель. Может быть InternalFlashStorage, ExtRomStorage, MemoryStorage
    /// или своей реализацией Storage.
    ///
    /// ВРЕМЯ ЖИЗНИ: носитель и интерфейс обязаны ЖИТЬ ДОЛЬШЕ сервиса — он
    /// хранит на них указатели и обращается к ним в poll(), а не только в
    /// init(). Объект, созданный на стеке, обязан быть «отпущен» вызовом
    /// Service::shutdown() до своего уничтожения; иначе следующий poll()
    /// обратится к уже не существующему объекту.
    Storage* storage = nullptr;

    /// Начало области метаданных.
    uint32_t meta_offset = 0u;

    /// Размер области метаданных. 0 — взять page_size()*4 (старые
    /// раскладки: две копии на слот). На F4 здесь 16 КБ — весь сектор под
    /// журнал append-only, а не четыре «страницы» по 4 КБ.
    uint32_t meta_size = 0u;

    /// Смещения и ёмкости слотов в адресном пространстве носителя.
    /// Слот — это НЕ «раздел целиком»: образ пишется с его начала, а место
    /// под метаданные берётся отдельно (иначе стирание записи состояния
    /// уничтожало бы принятые данные).
    uint32_t slot_offset[2]   = {0u, 0u};
    uint32_t slot_capacity[2] = {0u, 0u};

    /// Какой слот СЕЙЧАС РАБОТАЕТ (kSlotNone — неизвестно/загрузчик решает).
    /// Нужен для двух вещей: не писать в слот, из которого исполняется код
    /// (стирание такого слота убивает устройство), и понимать, куда
    /// возвращаться при откате.
    uint8_t active_slot = kSlotNone;

    /// Разрешить стирание активного слота. По умолчанию ЗАПРЕЩЕНО: без
    /// загрузчика это стирание самого себя. Включайте только если код
    /// исполняется из ОЗУ (загрузчик) и вы понимаете, что делаете.
    bool allow_active_erase = false;

    /// Разрешить запись версии не новее работающей (откат вручную).
    bool allow_downgrade = false;

    // ──────────── ПО ЧЕМУ ПРИНИМАЕМ ────────────
    /// Интерфейс приёма. nullptr — то же, что NullTransport: приём невозможен,
    /// модуль работает только с тем, что уже лежит в хранилище (VERIFY).
    /// Время жизни — см. storage выше.
    Transport* transport = nullptr;

    /// Предел payload одного кадра. 0 — взять из транспорта, а если и он
    /// молчит — MCFRAME_OTA_MAX_PAYLOAD. Уменьшается автоматически, если
    /// буфер сервиса меньше.
    uint16_t max_payload = 0u;

    // ──────────── ПРИЁМ ────────────
    /// Ожидание байт кадра (в poll). Меньше — чаще уступаем процессор, больше
    /// — меньше накладных расходов на медленном канале.
    uint32_t frame_timeout_ms = 2000u;

    /// Простой сессии, после которого она закрывается (оставаясь
    /// продолжаемой). 0 — не закрывать никогда.
    uint32_t session_timeout_ms = 30000u;

    /// Через сколько записанных байт сохранять состояние (позволяет продолжить
    /// после потери питания). 0 — только в начале и в конце. Меньше значения
    /// — чаще стирается страница метаданных; на 1 КБ страницах разумный
    /// минимум — 4096.
    uint32_t checkpoint_bytes = 4096u;

    /// Сколько времени разрешить сторожевому таймеру не кормиться во время
    /// стирания/записи страницы (см. watchdog::Service::allow_stall).
    /// 0 — не трогать сторожа (приложение обязано само позаботиться).
    uint32_t erase_stall_ms = 2000u;

    /// Не чаще, чем раз в это время, отвечаем NAK на испорченный кадр.
    /// Отвечать на каждый байт мусора — значит удваивать трафик на плохой
    /// линии; 0 — отвечать всегда.
    uint32_t bad_frame_nak_period_ms = 500u;

    // ──────────── ПРОВЕРКА ────────────
    /// Перечитывать записанное и сверять CRC32/SHA-256 перед объявлением
    /// образа годным. Для ОЗУ бессмысленно (is_volatile), для Flash — нет.
    bool verify_readback = true;

    /// Перечитывать и сверять префикс при продолжении передачи. Выключать
    /// допустимо только для носителя, который гарантирует запись (ОЗУ):
    /// иначе «продолжим с 32 КБ» может означать «продолжим с дырой».
    bool verify_resume_prefix = true;

    /// Требовать подпись образа. Обязан быть задан verify_signature.
    bool require_signature = false;
    SignatureVerifier verify_signature = nullptr;
    void* signature_ctx = nullptr;

    // ──────────── ЗАГРУЗКА ────────────
    /// Версия РАБОТАЮЩЕЙ прошивки: уходит в INFO и служит порогом для отказа
    /// в откате (если allow_downgrade == false).
    uint32_t fw_version = 0u;

    /// Сколько пробных загрузок новой прошивки допускается, прежде чем
    /// приложение обязано вернуться к прежнему слоту.
    uint32_t max_boot_attempts = 3u;

    /// Первый резервный регистр для отметки об обновлении. По умолчанию 5:
    /// регистры 0..4 заняты сторожевым таймером (watchdog::kNoteCount).
    uint8_t note_base = 5u;

    // ──────────── ИНТЕГРАЦИЯ ────────────
    /// Не позволять системе уходить в глубокий сон, пока идёт сессия.
    /// В STOP гаснет тактирование интерфейса, и приём «замирает».
    bool keep_awake = true;

    /// Ставить сторожу окно на длительную операцию (erase_stall_ms).
    bool use_watchdog_stall = true;

    /// Уведомление о событиях (см. Event). Вызывается из контекста задачи,
    /// которая обслуживает приём, — блокироваться в нём нельзя.
    Callback_h on_event = nullptr;
    void*      event_arg = nullptr;
};

/// Сервис OTA: один на приложение.
class Service {
public:
    static Service& instance() noexcept {
        static Service s;
        return s;
    }

    Service(const Service&)            = delete;
    Service& operator=(const Service&) = delete;

    /// Инициализация: проверяет раскладку (метаданные не перекрываются со
    /// слотами, образ влезает в слот), поднимает носитель и транспорт.
    ///
    /// Повторный вызов перенастраивает сервис: открытая сессия закрывается без
    /// записи состояния (данные в слоте остаются — их можно продолжить новым
    /// START с флагом Resume), носитель и интерфейс закрываются, после чего
    /// применяется новая настройка. Так приложение может переключить
    /// интерфейс приёма или сменить раскладку, не перезагружаясь.
    ///
    /// Возврат: Ok | InvalidArg (раскладка/настройка) |
    /// NotSupported (носитель не умеет страниц, транспорт недоступен) |
    /// HardwareError (носитель не поднялся).
    Status init(const Config& cfg) noexcept;

    /// Закрыть сессию, носитель и интерфейс, ничего не записывая.
    /// Что уже лежит в слоте — остаётся (это не «отмена»), но сессия теряется.
    void shutdown() noexcept;

    bool initialized() const noexcept { return inited_; }
    const Config& config() const noexcept { return cfg_; }

    /// Вид интерфейса приёма (для отчётов; уходит в INFO).
    proto::Iface iface() const noexcept;

    /// Обслужить приём: принять и обработать кадры, уложившись в budget_ms
    /// (0 — до первого кадра либо до таймаута интерфейса).
    ///
    /// Возврат: Ok (что-то обработано) | Timeout (данных не было) |
    /// NotSupported (транспорта нет) | ошибка интерфейса.
    /// Вызывается из задачи приложения в цикле.
    Status poll(uint32_t budget_ms) noexcept;

    /// Состояние сессии/слота.
    proto::State state() const noexcept { return state_; }
    /// Сколько байт непрерывно принято и записано.
    uint32_t next_offset() const noexcept { return next_offset_; }
    /// Слот, с которым сейчас работаем (kSlotNone — сессии нет).
    uint8_t session_slot() const noexcept { return slot_; }
    /// Идёт ли сессия (можно писать данные).
    bool session_active() const noexcept { return session_open_; }

    Stats stats() const noexcept;

    /// Последнее событие (см. Event) — удобно для отчётов и тестов.
    Event last_event() const noexcept { return last_event_; }

    /// Запись состояния слота (метаданные) — для отчётов и тестов.
    Status slot_record(uint8_t slot, SlotRecord& out) noexcept;

    // ──────────── Загрузка и активация ────────────

    /// Прочитать метаданные при старте и решить, что делать с загрузкой.
    /// Если активированный слот ещё не подтверждён — увеличивает счётчик
    /// попыток и помечает загрузку пробной (BootInfo::trial).
    Status boot_check(BootInfo& out) noexcept;

    /// «Прошивка запустилась и работает»: снимает признак пробной загрузки и
    /// сбрасывает счётчик попыток. Вызывать после успешной инициализации.
    Status confirm_running() noexcept;

    /// Пометить слот готовым к загрузке (образ должен быть проверен).
    Status activate(uint8_t slot) noexcept;

    /// Вернуться к прежнему слоту: активирует другой проверенный слот, а если
    /// такого нет — снимает активацию совсем (грузим то, что было прошито).
    Status rollback() noexcept;

    /// Стереть слот (образ становится негодным; активный слот — только при
    /// allow_active_erase).
    Status erase_slot(uint8_t slot) noexcept;

    /// Пометить слот НЕГОДНЫМ, не трогая данные: запись состояния переводится
    /// в Failed с нулевыми флагами проверок.
    ///
    /// Зачем это отдельная операция: загрузчик, обнаруживший что образ в слоте
    /// не проходит проверку (вектор или CRC32), обязан снять метку «годен» —
    /// иначе он возвращался бы к этому слоту каждую загрузку, а счётчик
    /// пробных загрузок этого не ловит (он про пробную ЗАГРУЗКУ, а не про
    /// порчу носителя). Стирать данные при этом незачем: слот всё равно
    /// перезапишут целиком, а стирание — самая долгая и самая опасная
    /// операция.
    Status invalidate_slot(uint8_t slot, uint16_t detail = 0u) noexcept;

    /// Проверить образ, УЖЕ лежащий в слоте: посчитать CRC32 (и SHA-256) по
    /// носителю и сравнить с ожидаемым. Это путь «интерфейса нет»: образ
    /// залил программатор или другой канал, устройству остаётся его признать.
    Status verify_stored(uint8_t slot, uint32_t size, uint32_t crc32,
                         const uint8_t* sha256) noexcept;

    /// Статический размер сервиса (буферы приёмника/передатчика, состояние).
    static constexpr Size static_bytes() noexcept { return sizeof(Service); }

private:
    Service() noexcept = default;

    // ── Приём ──
    Status serve(uint32_t budget_ms) noexcept;
    void   on_bytes(const uint8_t* data, Size len, bool& got_frame) noexcept;
    void   on_idle() noexcept;
    void   handle(const proto::Frame& frame) noexcept;

    // ── Обработчики операций ──
    void on_info(const proto::Frame& f) noexcept;
    void on_start(const proto::Frame& f) noexcept;
    void on_data(const proto::Frame& f) noexcept;
    void on_end(const proto::Frame& f) noexcept;
    void on_sign(const proto::Frame& f) noexcept;
    void on_abort(const proto::Frame& f) noexcept;
    void on_erase(const proto::Frame& f) noexcept;
    void on_verify(const proto::Frame& f) noexcept;
    void on_activate(const proto::Frame& f) noexcept;
    void on_confirm(const proto::Frame& f) noexcept;
    void on_state(const proto::Frame& f) noexcept;
    void on_read(const proto::Frame& f) noexcept;

    // ── Ответы ──
    void reply_ack(const proto::Response& r, const void* extra = nullptr,
                   Size extra_len = 0u) noexcept;
    void reply_nak(proto::Code code, uint16_t detail = 0u) noexcept;
    void reply(const proto::Op op, const proto::Response& r,
               const void* extra, Size extra_len) noexcept;
    /// Отправить кадр, полезная нагрузка которого УЖЕ лежит в tx_frame_.
    void send_frame(proto::Op op, Size payload_len) noexcept;
    void make_response(proto::Response& r) const noexcept;

    // ── Сессия ──
    Status open_session(const proto::Start& s, proto::Response& r, bool& resumed) noexcept;
    Status resume_session(const proto::Start& s, proto::Response& r) noexcept;
    Status start_fresh(const proto::Start& s, proto::Response& r) noexcept;
    void   close_session(bool expired) noexcept;
    void   fail_session(proto::Code code, uint16_t detail,
                        uint8_t slot = kSlotNone) noexcept;
    Status checkpoint(bool force) noexcept;
    Status write_image(uint32_t offset, const void* data, Size len) noexcept;
    Status ensure_erased(uint32_t upto) noexcept;
    /// Накопить принятые байты и записать ровно те порции, которые кратны
    /// грануле программирования носителя (см. write_gran_).
    Status stage_bytes(const uint8_t* data, Size len) noexcept;
    /// Записать остаток: пара добивается значением 0xFF. Вызывается только
    /// когда образ принят целиком, поэтому «добитый» байт лежит за образом, в
    /// свободной части слота, и никаких данных не портит.
    Status flush_pending() noexcept;
    Status verify_image(uint8_t slot, uint32_t image_size, uint32_t expect_crc,
                        const uint8_t* expect_sha, proto::HashKind kind,
                        uint32_t& out_crc) noexcept;
    Status read_slot_record(uint8_t slot, SlotRecord& out) noexcept;
    /// Посчитать CRC32 (и SHA-256) по префиксу образа, лежащего в слоте.
    Status verify_prefix(uint8_t slot, uint32_t len, uint32_t expect_crc,
                         proto::HashKind kind, const uint8_t sha_expected[32],
                         uint32_t& out_crc) noexcept;
    Status write_slot_record(const SlotRecord& rec) noexcept;
    SlotRecord build_record(uint8_t slot, proto::State state) const noexcept;
    /// Сбросить состояние сессии (слот, размеры, CRC, хеш, подпись).
    void   reset_session_state() noexcept;
    void   emit(Event ev) noexcept;
    void   note_reset(uint8_t slot, uint32_t attempts, uint32_t state) noexcept;

    // ── Время и интеграция ──
    uint64_t now_ms() const noexcept;
    void     stall_begin() noexcept;
    void     stall_end() noexcept;
    void     hold_awake(bool on) noexcept;

    /// Предел БЛОКА данных в кадре DATA: полезная нагрузка кадра включает
    /// 4 байта смещения, поэтому блок не может быть больше предела кадра.
    uint16_t max_block() const noexcept {
        return (max_payload_ > proto::kDataHeaderSize)
                 ? static_cast<uint16_t>(max_payload_ - proto::kDataHeaderSize)
                 : 0u;
    }

    static bool slot_valid(uint16_t slot) noexcept {
        return slot == 0u || slot == 1u;
    }
    uint8_t pick_slot(uint16_t requested) noexcept;

    Config cfg_{};
    bool   inited_ = false;

    Storage*   storage_ = nullptr;
    Transport* transport_ = nullptr;
    NullTransport null_transport_{};

    SlotStore slots_{};           ///< записи состояния двух слотов (общие с загрузчиком)
    uint32_t  page_ = 0u;
    uint32_t  image_limit_ = 0u;

    uint16_t max_payload_ = proto::kMaxPayload;

    proto::Decoder decoder_{};
    uint8_t rx_frame_[proto::decoder_buffer_size(proto::kMaxPayload)]{};
    uint8_t rx_chunk_[64]{};
    uint8_t tx_frame_[proto::decoder_buffer_size(proto::kMaxPayload)]{};
    /// Буфер для чтения обратно из носителя (проверка префикса и образа).
    /// Отдельный, а не заимствованный у декодера: в буфере декодера может
    /// лежать начало следующего кадра, и затирать его нельзя.
    uint8_t io_buf_[128]{};

    // ── Сессия ──
    bool         session_open_ = false;   ///< host начал сессию (START принят)
    uint8_t      slot_ = kSlotNone;       ///< слот ТЕКУЩЕЙ сессии
    /// Слот, запись которого разрешено менять вне сессии (после END, пока не
    /// пройдена подпись). Отдельное поле, а не slot_: кадр, пришедший ВНЕ
    /// сессии (например SIGN после перезагрузки), не должен «догадываться»,
    /// о каком слоте речь, и портить запись чужого слота.
    uint8_t      verified_slot_ = kSlotNone;
    /// Слот, который выбрал boot_check() — по нему confirm_running() понимает,
    /// какую загрузку подтверждают, даже если Config::active_slot не задан.
    uint8_t      boot_slot_ = kSlotNone;
    proto::State state_ = proto::State::Idle;
    uint32_t     image_size_ = 0u;
    uint32_t     expect_crc_ = 0u;
    uint32_t     fw_version_ = 0u;
    uint16_t     chunk_max_ = proto::kMaxPayload;
    proto::HashKind hash_kind_ = proto::HashKind::None;
    uint32_t     next_offset_ = 0u;
    /// Сколько байт ФИЗИЧЕСКИ записано в носитель (кратно write_gran_): именно
    /// это число уходит в запись состояния как «продолжать отсюда», потому что
    /// прочитать обратно можно только его — недобранная гранула живёт в буфере
    /// сервиса и после сброса исчезнет.
    uint32_t     written_ = 0u;
    /// «Сырое» состояние CRC32 по записанному префиксу [0, written_).
    uint32_t     crc_written_ = 0u;
    /// Грануля программирования носителя и буфер недобранной порции.
    ///
    /// ЗАЧЕМ БУФЕР: NOR-flash не умеет программировать одиночный байт (у
    /// STM32F1 — только half-word), а дописать половинку, часть которой уже
    /// записана, нельзя: это повторное программирование уже запрограммированной
    /// ячейки, которое спецификацией не разрешено. Поэтому сервис сам добирает
    /// порции, а носителю отдаёт только целые гранулы (последняя добивается
    /// значением 0xFF в свободной части слота).
    uint8_t      write_gran_ = 1u;
    uint8_t      pending_[8]{};
    uint8_t      pending_len_ = 0u;
    uint32_t     erased_upto_ = 0u;
    uint32_t     next_checkpoint_ = 0u;
    uint32_t     crc_ = 0u;               ///< «сырое» состояние CRC32 образа
    crypto::Sha2<crypto::Sha2Variant::Sha256> sha_{};
    uint8_t      sha_expected_[32]{};
    uint8_t      signature_[proto::kMaxSignature]{};
    uint16_t     signature_len_ = 0u;
    uint8_t      signature_kind_ = 0u;

    uint8_t  tx_seq_ = 0u;
    uint64_t last_frame_ms_ = 0u;
    uint64_t last_bad_nak_ms_ = 0u;
    bool     keep_awake_held_ = false;
    bool     stall_open_ = false;

    uint32_t skipped_seen_ = 0u;   ///< сколько байт синхронизации уже учтено
    uint32_t faults_seen_ = 0u;    ///< сколько отброшенных кадров уже учтено

    power::Supervisor::Lease lease_{};

    Stats stats_{};
    Event last_event_ = Event::SessionStarted;
};

} // namespace embed::ota
