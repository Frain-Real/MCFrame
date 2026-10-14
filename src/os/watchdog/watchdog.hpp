// watchdog.hpp
#pragma once
// ---------------------------------------------------------------------------
// СТОРОЖЕВОЙ ТАЙМЕР: аппаратный сброс зависшего устройства + посмертная
// диагностика.
//
// ЧЕМ ЭТО ОТЛИЧАЕТСЯ ОТ «ПРОСТО КОРМИТЬ IWDG»
// Наивная схема (одна задача раз в секунду пишет в IWDG_KR) защищает только
// от полного отказа: если зависла ЛЮБАЯ другая задача, а «кормилец» жив,
// устройство не перезагрузится никогда. Поэтому здесь модель «участников»:
//
//   1. приложение регистрирует участников — то, что обязано работать
//      (задача опроса датчика, обработчик протокола, ...);
//   2. каждый участник в конце своего цикла работы отмечается check_in();
//   3. сервис в прерывании тика проверяет сроки ВСЕХ участников и кормит
//      железо ТОЛЬКО если живы все;
//   4. если кто-то опоздал — кормление прекращается, и аппаратный таймер
//      сбрасывает устройство через свой срок.
//
// Кто именно опоздал, известно ДО сброса: в режиме Warning сервис вызывает
// колбэк и записывает отметку в регистры, переживающие сброс (у STM32 — BKP),
// поэтому после перезагрузки last_fault() отвечает на вопрос «почему».
//
// ЧЕГО ЗДЕСЬ НЕТ И ПОЧЕМУ
//   * оконного сторожа (WWDG): он считает от PCLK1, а значит в глубоком сне
//     (STOP) останавливается и перестаёт защищать, а при смене частоты меняет
//     период. Роль окна «отметился раньше срока» играет min_interval_ms
//     у участника: слишком частая отметка — такой же признак беды, как
//     слишком редкая (задача крутится в цикле, но работу не делает);
//   * «паузы» сторожа без границы: единственный способ разрешить длительную
//     операцию — allow_stall(ms) с ОГРАНИЧЕННЫМ окном (Config::max_stall_ms).
//     Незакрытая пауза истечёт сама, поэтому маскировать зависание навсегда
//     ей не удастся, а каждый случай виден в статистике.
//
// СВЯЗЬ С ЭНЕРГОСБЕРЕЖЕНИЕМ
// Аппаратный таймер считает и в STOP (у STM32 он тактируется от LSI), но
// кормить его в глубоком сне некому — поэтому сервис сообщает супервизору
// энергосбережения, сколько можно не просыпаться (sleep_limit_ms(),
// регистрируется через power::Supervisor::add_sleep_limit). Без этого
// «энергосбережение» и «сторожевой таймер» складываются в сброс устройства
// посреди сна.
// ---------------------------------------------------------------------------

#include "../common.hpp"
#include "../iplatform.hpp"
#include "../config/time_config.hpp"
#include "../core/primitives/types.hpp"   // atomic_t: отметки пишут задачи, читает тик

namespace embed::watchdog {

/// Причина последнего сброса — словарь платформы.
using ResetCause = platform::ResetCause;

/// Режим работы сервиса.
enum class Mode : uint8_t {
    Off     = 0u,   ///< железо не запускаем: слежение и диагностика на стенде
    Warning = 1u,   ///< при просрочке — предупреждение (колбэк + отметка), затем сброс
    Reset   = 2u    ///< при просрочке — перестаём кормить: сбрасывает железо
};

/// Индекс «участник неизвестен».
inline constexpr uint32_t kNoParticipant = 0xFFFFFFFFu;

/// Ёмкость таблицы участников (настраивается под приложение).
#ifndef MCFRAME_WATCHDOG_SLOTS
#define MCFRAME_WATCHDOG_SLOTS 6u
#endif

/// Участник слежения: «то, что обязано работать».
///
/// Объект статический и принадлежит приложению; сервис хранит на него
/// указатель. Отметка check_in() безопасна из задачи и из прерывания —
/// поэтому поле времени отметки атомарное, а не «просто переменная».
class Participant {
public:
    Participant() noexcept = default;
    Participant(const Participant&)            = delete;
    Participant& operator=(const Participant&) = delete;

    /// Зарегистрировать участника: имя (для отчётов и посмертной отметки),
    /// срок отметки в мс и, необязательно, минимальный интервал между
    /// отметками (0 — не проверять).
    ///
    /// Требует, чтобы сервис был инициализирован раньше: до Service::init()
    /// регистрировать некуда.
    Status init(const char* name,
                uint32_t    timeout_ms,
                uint32_t    min_interval_ms = 0u) noexcept;

    /// «Я жив» — вызывать в конце успешного цикла работы, а НЕ в начале.
    ///
    /// Возврат: Ok | NotFound (участник не зарегистрирован) |
    /// AlreadyExists (отметка раньше min_interval_ms — учтена как нарушение).
    Status check_in() noexcept;

    /// Явный отказ: «я больше не буду отмечаться». Сервис перестаёт кормить
    /// железо, не дожидаясь срока, — так задача сообщает о неустранимой
    /// ошибке (например, отвал датчика), не изображая работоспособность.
    void fail() noexcept;

    const char* name() const noexcept { return name_; }
    uint32_t timeout_ms() const noexcept { return timeout_ms_; }
    uint32_t min_interval_ms() const noexcept { return min_interval_ms_; }
    uint32_t checks() const noexcept { return checks_.load(MemoryOrderRelaxed); }
    uint32_t misses() const noexcept { return misses_.load(MemoryOrderRelaxed); }
    bool     failed() const noexcept { return failed_.load(MemoryOrderSeqCst); }
    bool     registered() const noexcept { return used_; }

    /// Сколько миллисекунд прошло с последней отметки (для отчётов).
    uint32_t age_ms() const noexcept;

    static constexpr Size static_bytes() noexcept { return sizeof(Participant); }

private:
    friend class Service;

    const char* name_            = nullptr;
    uint32_t    timeout_ms_      = 0u;
    uint32_t    min_interval_ms_ = 0u;

    /// Время последней отметки. Атомарно: пишет задача (или прерывание),
    /// читает прерывание тика — обычная переменная нарушала бы правило
    /// проекта (см. types.hpp) и зависела бы от разрядности шины.
    atomic_t<uint32_t> last_ms_{0u};
    atomic_t<uint32_t> checks_{0u};
    atomic_t<uint32_t> misses_{0u};

    /// Отказ («я больше не работаю»): пишет задача, читает тик.
    atomic_bool failed_{false};

    bool used_ = false;
};

/// Снимок статистики сервиса.
struct Stats {
    uint32_t participants   = 0u;   ///< зарегистрировано участников
    uint32_t checks         = 0u;   ///< сколько раз проверяли сроки
    uint32_t kicks          = 0u;   ///< сколько раз покормили железо
    uint32_t misses         = 0u;   ///< сколько раз принято решение о просрочке
    uint32_t early_checks   = 0u;   ///< отметок раньше min_interval_ms
    uint32_t warnings       = 0u;   ///< предупреждений о просрочке
    uint32_t stalls         = 0u;   ///< разрешённых длительных операций
    uint32_t refused_stalls = 0u;   ///< отказов в длительной операции
    uint32_t deep_sleeps    = 0u;   ///< сколько раз система спала под сторожем
    uint32_t slept_ms       = 0u;   ///< суммарно проспано под сторожем, мс
    uint32_t rearms         = 0u;   ///< сколько раз снимали защёлку просрочки
    uint32_t max_late_ms    = 0u;   ///< максимальное опоздание отметки
    bool     hardware       = false;///< аппаратный таймер запущен
};

/// Посмертная запись: что известно о причине ПРЕДЫДУЩЕЙ перезагрузки.
struct Fault {
    bool     valid        = false;  ///< есть достоверная информация
    uint32_t cause        = 0u;     ///< биты ResetCause
    uint32_t participant  = kNoParticipant;
    uint32_t late_ms      = 0u;     ///< насколько участник опоздал
    uint32_t boot_count   = 0u;     ///< номер текущей загрузки
};

/// Настройка сервиса.
struct Config {
    Mode     mode = Mode::Reset;

    /// Период проверки сроков И кормления железа, мс. Он же — задержка
    /// обнаружения просрочки, поэтому не стоит делать его большим.
    uint32_t check_period_ms = 100u;

    /// Запас к максимальному сроку участников: аппаратный таймаут =
    /// max(сроки) + запас. 0 — рассчитать автоматически
    /// (2 × check_period + запас на пробуждение из сна).
    uint32_t hw_margin_ms = 0u;

    /// Сколько времени нужно сервису, чтобы проснуться и заметить просрочку
    /// (плюс гранулярность источника пробуждения платформы). Вычитается из
    /// того, сколько система имеет права спать. 0 — рассчитать автоматически.
    uint32_t sleep_safety_ms = 0u;

    /// Предел одной длительной операции (allow_stall). 0 — запрещены.
    uint32_t max_stall_ms = 0u;

    /// Первый резервный регистр для посмертной отметки (занимает 4 подряд).
    uint8_t note_base = 0u;

    /// Предупреждение ДО сброса (только Mode::Warning). Вызывается из
    /// прерывания тика: колбэк обязан быть коротким — без блокировок и сна.
    Callback_h on_warning = nullptr;
    void*      warning_arg = nullptr;
};

/// Сервис сторожевого таймера.
class Service {
public:
    static Service& instance() noexcept {
        static Service s;
        return s;
    }

    Service(const Service&)            = delete;
    Service& operator=(const Service&) = delete;

    /// Хук системного тика: OsTimer::RegisterSystemHook(&Service::tick_hook, nullptr).
    static void tick_hook(void*) noexcept { instance().on_tick(); }

    /// Источник ограничения сна для power::Supervisor (регистрируется в init).
    static uint32_t sleep_limit_hook(void* ctx) noexcept;
    static void     slept_hook(void* ctx, uint32_t slept_ms) noexcept;

    /// Инициализация: рассчитывает аппаратный таймаут, читает причину
    /// последнего сброса и поднимает счётчик загрузок. Железо НЕ запускает —
    /// это делает start() (иначе медленная инициализация приложения сама
    /// приводила бы к сбросу по сторожевому таймеру).
    ///
    /// Возврат: Ok | AlreadyExists | InvalidArg (mode/период) |
    /// NotSupported (железо не может обеспечить нужный таймаут) |
    /// NoMemory (супервизор энергосбережения не принял ограничение сна:
    /// продолжать нельзя — сон был бы длиннее срока сторожа).
    Status init(const Config& cfg) noexcept;

    bool initialized() const noexcept { return inited_; }

    /// Запустить аппаратный таймер. Вызывать, когда система готова работать
    /// (после регистрации участников и задач). Идемпотентно.
    Status start() noexcept;

    /// Остановить аппаратный таймер: Ok | NotSupported (IWDG не умеет) |
    /// Unavailable (не запущен).
    Status stop() noexcept;

    Mode mode() const noexcept { return cfg_.mode; }

    /// Аппаратный таймаут, который реально обеспечило железо.
    uint32_t hardware_timeout_ms() const noexcept { return hw_timeout_ms_; }

    /// Сколько миллисекунд система может НЕ просыпаться, чтобы её не сбросил
    /// аппаратный таймер (kNoDeadline — ограничения нет).
    uint32_t sleep_limit_ms() noexcept;

    /// Разрешить одну длительную операцию на ms (стирание Flash, калибровка).
    /// Пока окно открыто, сервис продолжает кормить железо, не считая
    /// просрочек. Окно ОГРАНИЧЕНО Config::max_stall_ms и истекает само,
    /// поэтому забытая пауза не превращается в «сторож выключен навсегда».
    /// Возврат: Ok | NotSupported (окно больше предела или ms == 0).
    Status allow_stall(uint32_t ms) noexcept;

    /// Сколько миллисекунд осталось от текущего окна (0 — окна нет).
    uint32_t stall_left_ms() noexcept;

    Stats stats() noexcept;
    Fault last_fault() noexcept;
    Status clear_fault() noexcept;

    /// Снять защёлку просрочки и начать слежение заново.
    ///
    /// Нужно приложению, которое САМО обработало нарушение (например,
    /// перезапустило отвалившуюся подсистему) и сознательно продолжает работу
    /// без перезагрузки всего устройства. Снимается и защёлка просрочки, и
    /// признак отказа участников (Participant::fail): сроки начинаются заново,
    /// аппаратный таймер сразу кормится.
    ///
    /// ОСТОРОЖНО: это единственный способ отменить уже принятое решение о
    /// сбросе, поэтому вызовы считаются (Stats::rearms) и должны быть видны в
    /// отчёте: «за смену подмигнули сторожу N раз» — это диагноз, а не норма.
    ///
    /// Возврат: Ok | Unavailable (сервис не идёт) | AlreadyExists (снимать
    /// нечего).
    Status rearm() noexcept;

    /// Причина последнего сброса (прочитана один раз при init).
    uint32_t reset_cause() const noexcept { return reset_cause_; }

    static constexpr Size static_bytes() noexcept { return sizeof(Service); }

private:
    Service() noexcept = default;

    void on_tick() noexcept;
    /// Система спала slept_ms: пересчитать сроки и покормить таймер.
    void on_slept(uint32_t slept_ms) noexcept;
    /// Регистрация участника (вызывает Participant::init).
    Status register_participant(Participant& p) noexcept;
    /// Учесть отметку раньше min_interval_ms (вызывает Participant::check_in).
    void   count_early() noexcept;
    void   note_fault(uint32_t participant, uint32_t late_ms) noexcept;
    void   kick(uint32_t now) noexcept;
    /// Индексы — ОТНОСИТЕЛЬНЫЕ (kNoteIdx*): Config::note_base прибавляется
    /// здесь, чтобы вызывающий код не повторял это в каждом обращении.
    bool     note_write(uint16_t index, uint16_t value) noexcept;
    uint16_t note_read(uint16_t index) noexcept;
    static bool time_reached(uint32_t now, uint32_t deadline) noexcept {
        return static_cast<int32_t>(now - deadline) >= 0;
    }

    friend class Participant;

    Config   cfg_{};
    bool     inited_   = false;   ///< init() выполнен
    bool     running_  = false;   ///< start() выполнен: сроки считаются
    bool     hardware_ = false;   ///< аппаратный таймер запущен

    uint32_t hw_timeout_ms_   = 0u;
    uint32_t kick_period_ms_  = 0u;
    uint32_t sleep_safety_ms_ = 0u;

    Participant* slots_[MCFRAME_WATCHDOG_SLOTS]{};
    uint32_t     participants_ = 0u;

    uint32_t last_kick_ms_   = 0u;
    uint32_t stall_until_ms_ = 0u;   ///< 0 — окна длительной операции нет

    bool     fault_noted_ = false;   ///< посмертная отметка уже сделана
    bool     warned_      = false;   ///< решение о просрочке принято (защёлка)

    uint32_t checks_         = 0u;
    uint32_t kicks_          = 0u;
    uint32_t misses_         = 0u;
    uint32_t early_checks_   = 0u;
    uint32_t warnings_       = 0u;
    uint32_t stalls_         = 0u;
    uint32_t refused_stalls_ = 0u;
    uint32_t deep_sleeps_    = 0u;
    uint32_t slept_ms_       = 0u;
    uint32_t rearms_         = 0u;
    uint32_t max_late_ms_    = 0u;

    uint32_t reset_cause_ = 0u;
    uint32_t boot_count_  = 0u;
};

// ─────────────────────────── Раскладка посмертной отметки ───────────────────
//
// Занимает kNoteCount подряд идущих резервных регистров начиная с
// Config::note_base. Индексы (kNoteIdx*) — СМЕЩЕНИЯ внутри раскладки; значение
// «наши ли это регистры» — отдельная константа kNoteMagic (их легко перепутать,
// поэтому имена и разведены).
constexpr uint16_t kNoteIdxMagic = 0u;   ///< признак «регистры наши»
constexpr uint16_t kNoteIdxBoot  = 1u;   ///< номер текущей загрузки
constexpr uint16_t kNoteIdxFault = 2u;   ///< индекс участника + 1 (0 — не было)
constexpr uint16_t kNoteIdxLate  = 3u;   ///< опоздание, мс (обрезано до 65535)
constexpr uint16_t kNoteIdxFBoot = 4u;   ///< в какой загрузке была просрочка
constexpr uint16_t kNoteCount    = 5u;

/// 'W','D' — признак, что резервные регистры принадлежат сторожу.
constexpr uint16_t kNoteMagic = 0x5744u;

} // namespace embed::watchdog
