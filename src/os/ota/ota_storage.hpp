// os/ota/ota_storage.hpp
#pragma once
// ---------------------------------------------------------------------------
// ГДЕ ХРАНИТСЯ НОВАЯ ПРОШИВКА.
//
// Модуль OTA не знает, что такое «внутренняя Flash», «SPI-флешка» или «буфер
// в ОЗУ»: он работает с абстракцией Storage и двумя смещениями — где лежат
// МЕТАДАННЫЕ и где лежат СЛОТЫ. Поэтому одна и та же логика приёма, проверки
// и активации работает и на STM32F103 с двумя слотами в 128 КБ Flash, и на
// плате с внешней микросхемой, и на стенде с буфером в памяти.
//
// ─────────────────────────── МОДЕЛЬ ХРАНИЛИЩА ───────────────────────────────
//
//   offset 0                meta_offset          slot_offset[0]  slot_offset[1]
//   │                      │                    │               │
//   │   (что угодно —      │  2 страницы мета-  │  образ A      │  образ B
//   │    например код или  │  данных: основная  │  (слот)       │  (слот)
//   │    ничего)           │  и резервная копия │               │
//
// ПОЧЕМУ МЕТАДАННЫЕ — ОТДЕЛЬНЫЕ СТРАНИЦЫ, А НЕ ЗАГОЛОВОК ОБРАЗА
// Запись состояния (сколько байт принято и какая CRC на этой длине) делается
// НЕСКОЛЬКО РАЗ за передачу — это то, что позволяет продолжить обрывок после
// потери питания. На NOR-flash «перезаписать» байт нельзя: сначала стирается
// страница целиком. Если бы состояние лежало в начале образа, каждое
// сохранение стирало бы уже принятые данные. Поэтому:
//   * метаданные — две страницы, которые стираются по очереди (2 копии
//     дают возможность писать поверх СТАРОЙ копии, не теряя новую);
//   * образ — пишется строго вперёд, по одному разу на байт до следующего
//     стирания слота.
//
// ПОЧЕМУ ДВЕ КОПИИ, А НЕ ОДНА
// Стирание + запись копии не атомарны: сбой питания между ними оставил бы
// «полустёртую» запись, и о состоянии не узнал бы никто. Две копии с
// счётчиком seq решают это: пока пишется младшая копия, старшая остаётся
// целой; при чтении берётся копия с БОЛЬШИМ seq и целой CRC32.
// ---------------------------------------------------------------------------

#include "ota_protocol.hpp"

namespace embed::ota {

// ─────────────────────────────── Интерфейс ─────────────────────────────────
//
// Требования к реализации (они же — то, на что опирается сервис):
//   1. write() принимает любое смещение и длину, но носитель, который физически
//      не умеет программировать одиночный байт (NOR-flash: half-word у STM32F1),
//      ВПРАВЕ ОТКАЗАТЬ (InvalidArg/HardwareError) вместо того, чтобы
//      договариваться «наполовину». Тогда выравнивать обязан вызывающий, и
//      сервис OTA это делает: он пишет только порции, кратные
//      write_granularity(), а последнюю добивает значением 0xFF в свободной
//      части слота. Так ни один байт не программируется дважды (для F1 это
//      запрещено: изменение содержимого требует стирания страницы).
//   2. Стирание — обязательное и явное: запись в нестёртое место на
//      NOR-flash физически невозможна. Сервис стирает слот ДО первой записи.
//   3. Отказ не «съедается»: если erase()/write() вернул ошибку, сервис
//      обязан прекратить приём и не помечать слот готовым (иначе устройство
//      загрузит полузаписанный образ).
//   4. read() честно возвращает то, что лежит в носителе. Именно на нём
//      строится проверка «записалось ли» — доверять только коду возврата
//      write() нельзя: у NOR-flash бывают «полузаписанные» ячейки.

class Storage {
public:
    virtual ~Storage() = default;

    Storage(const Storage&)            = delete;
    Storage& operator=(const Storage&) = delete;
    Storage(Storage&&)                 = delete;
    Storage& operator=(Storage&&)      = delete;

    /// Подготовка носителя (unlock, включение питания внешней микросхемы).
    /// Может физически ничего не делать — возврат Ok обязателен.
    virtual Status begin() noexcept { return Status::Ok; }

    /// Освобождение носителя (lock). Вызывается сервисом в конце операции.
    virtual void end() noexcept {}

    /// Ёмкость носителя в байтах (адресное пространство смещений).
    virtual uint32_t capacity() const noexcept = 0;

    /// Грануляр стирания: стирать можно только блоки, кратные этой величине
    /// (и по адресу, кратному ей). 0 — стирание не нужно (ОЗУ).
    virtual uint32_t page_size() const noexcept = 0;

    /// Грануляр программирования: минимальная порция, которую носитель готов
    /// принять (2 — half-word у STM32F1). 0/1 — запись побайтная.
    /// Сервис использует это число только для диагностики и расчёта
    /// минимального шага checkpoint; выравнивает драйвер.
    virtual uint32_t write_granularity() const noexcept { return 1u; }

    /// Стирание. offset кратен page_size(), len — тоже.
    virtual Status erase(uint32_t offset, uint32_t len) noexcept = 0;

    /// Запись без стирания: место обязано быть стёртым заранее.
    virtual Status write(uint32_t offset, const void* data, uint32_t len) noexcept = 0;

    /// Чтение.
    virtual Status read(uint32_t offset, void* data, uint32_t len) noexcept = 0;

    /// Носитель энергозависимый (ОЗУ): после сброса содержимое исчезнет,
    /// поэтому «продолжить после потери питания» на нём невозможно, а
    /// проверка прочитанного ничего не доказывает.
    virtual bool is_volatile() const noexcept { return false; }

    /// Имя для логов/отчётов.
    virtual const char* name() const noexcept = 0;

    /// Заполнить участок значением «стёрто» — нужно только для ОЗУ-моделей,
    /// чтобы запись была похожа на запись в стёртую Flash.
    virtual Status erase_fill(uint32_t offset, uint32_t len) noexcept {
        return erase(offset, len);
    }

protected:
    /// Объявленный копирующий конструктор подавил бы неявный по умолчанию,
    /// поэтому он объявлен явно: носители — статические объекты.
    Storage() noexcept = default;
};

// ──────────────────────────── Носитель в памяти ───────────────────────────
//
// ОЗУ, внешняя RAM, участок, отображённый в адресное пространство, или буфер
// теста. Штатное применение — стенд и «принять образ в память, чтобы потом
// записать его во Flash из загрузчика».

class MemoryStorage : public Storage {
public:
    MemoryStorage() noexcept = default;

    /// Привязать буфер. Буфер принадлежит приложению (статический массив,
    /// внешняя RAM, куча) — модуль память не выделяет.
    void attach(uint8_t* buffer, uint32_t len, bool volatile_memory = true) noexcept {
        buf_ = buffer;
        len_ = (buffer == nullptr) ? 0u : len;
        volatile_ = volatile_memory;
        if (buf_ != nullptr) {
            for (uint32_t i = 0u; i < len_; ++i) buf_[i] = 0xFFu;
        }
    }

    uint32_t capacity() const noexcept override { return len_; }
    uint32_t page_size() const noexcept override { return 1u; }
    uint32_t write_granularity() const noexcept override { return 1u; }
    bool     is_volatile() const noexcept override { return volatile_; }
    const char* name() const noexcept override { return "memory"; }

    Status erase(uint32_t offset, uint32_t len) noexcept override {
        return erase_fill(offset, len);
    }

    Status erase_fill(uint32_t offset, uint32_t len) noexcept override {
        if (!range_ok(offset, len)) return Status::InvalidArg;
        for (uint32_t i = 0u; i < len; ++i) buf_[offset + i] = 0xFFu;
        return Status::Ok;
    }

    Status write(uint32_t offset, const void* data, uint32_t len) noexcept override {
        if (!range_ok(offset, len) || (data == nullptr && len != 0u)) return Status::InvalidArg;
        const auto* src = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0u; i < len; ++i) {
            // Как во Flash: единичные биты можно только сбрасывать.
            buf_[offset + i] &= src[i];
        }
        return Status::Ok;
    }

    Status read(uint32_t offset, void* data, uint32_t len) noexcept override {
        if (!range_ok(offset, len) || (data == nullptr && len != 0u)) return Status::InvalidArg;
        auto* dst = static_cast<uint8_t*>(data);
        for (uint32_t i = 0u; i < len; ++i) dst[i] = buf_[offset + i];
        return Status::Ok;
    }

    uint8_t* buffer() const noexcept { return buf_; }

private:
    bool range_ok(uint32_t offset, uint32_t len) const noexcept {
        if (buf_ == nullptr) return false;
        if (offset > len_) return false;
        return (len_ - offset) >= len;
    }

    uint8_t* buf_ = nullptr;
    uint32_t len_ = 0u;
    bool     volatile_ = true;
};

// ──────────────────────── Носитель: внешняя ROM платформы ──────────────────
//
// Тонкая обёртка над контрактом platform::ops()::ext_rom_* (SPI-flash, SD,
// FRAM — что именно, решает платформа). Ядро не знает ни команд, ни шины:
// их знает платформенный код в src/platforms/<цель>.

class ExtRomStorage : public Storage {
public:
    explicit ExtRomStorage(uint32_t page_size = 4096u) noexcept : page_(page_size) {}

    void set_page_size(uint32_t page_size) noexcept { page_ = page_size; }

    Status begin() noexcept override {
        if (!available()) return Status::NotSupported;
        return Status::Ok;
    }

    uint32_t capacity() const noexcept override {
        if (!platform::ops().has_ext_rom()) return 0u;
        const Size cap = platform::ops().ext_rom_capacity();
        return (cap > 0xFFFFFFFFu) ? 0xFFFFFFFFu : static_cast<uint32_t>(cap);
    }

    uint32_t page_size() const noexcept override { return page_; }
    uint32_t write_granularity() const noexcept override { return 1u; }
    const char* name() const noexcept override { return "ext_rom"; }

    Status erase(uint32_t offset, uint32_t len) noexcept override {
        if (!available()) return Status::NotSupported;
        if (page_ == 0u || (offset % page_) != 0u || (len % page_) != 0u) {
            return Status::InvalidArg;
        }
        return platform::ops().ext_rom_erase(offset, len) ? Status::Ok : Status::HardwareError;
    }

    Status write(uint32_t offset, const void* data, uint32_t len) noexcept override {
        if (!available()) return Status::NotSupported;
        if (data == nullptr && len != 0u) return Status::InvalidArg;
        return platform::ops().ext_rom_write(offset, static_cast<const uint8_t*>(data), len)
                 ? Status::Ok : Status::HardwareError;
    }

    Status read(uint32_t offset, void* data, uint32_t len) noexcept override {
        if (!available()) return Status::NotSupported;
        if (data == nullptr && len != 0u) return Status::InvalidArg;
        return platform::ops().ext_rom_read(offset, static_cast<uint8_t*>(data), len)
                 ? Status::Ok : Status::HardwareError;
    }

private:
    static bool available() noexcept {
        return platform::ops().has_ext_rom() &&
               platform::ops().ext_rom_read  != nullptr &&
               platform::ops().ext_rom_write != nullptr &&
               platform::ops().ext_rom_erase != nullptr;
    }

    uint32_t page_ = 4096u;
};

// ───────────────────── Носитель: встроенная Flash платформы ────────────────
//
// Драйвер встроенной Flash живёт в платформе (unlock/erase/program — регистры,
// свои для каждого семейства). Здесь — только приведение его к Storage и
// ОКНО: сервис OTA работает не со всей Flash, а с заданным участком, чтобы
// случайная ошибка в смещении не стёрла код работающей прошивки.
//
// Смещения в этом носителе — АБСОЛЮТНЫЕ адреса Flash минус её базовый адрес
// (обычно 0x08000000 у STM32), то есть «адрес в пространстве Flash».

class InternalFlashStorage : public Storage {
public:
    InternalFlashStorage() noexcept = default;

    /// Окно внутри Flash. window_offset отсчитывается от base() платформы.
    InternalFlashStorage(uint32_t window_offset, uint32_t window_len) noexcept
        : window_offset_(window_offset), window_len_(window_len) {}

    void set_window(uint32_t window_offset, uint32_t window_len) noexcept {
        window_offset_ = window_offset;
        window_len_    = window_len;
    }

    static bool available() noexcept {
        const platform::FlashOps* f = platform::ops().flash;
        return f != nullptr && f->erase != nullptr && f->program != nullptr &&
               f->read != nullptr && f->unlock != nullptr;
    }

    Status begin() noexcept override {
        if (!available()) return Status::NotSupported;
        // Флаг выставляется ПО РЕЗУЛЬТАТУ, а не до вызова: иначе при неудачном
        // unlock() объект считал бы себя разблокированным и «взводил защиту»
        // в end(), ничего не взведя.
        const bool ok = platform::ops().flash->unlock();
        locked_ = !ok;
        return ok ? Status::Ok : Status::HardwareError;
    }

    void end() noexcept override {
        if (available() && !locked_ && platform::ops().flash->lock != nullptr) {
            platform::ops().flash->lock();
            locked_ = true;
        }
    }

    uint32_t capacity() const noexcept override {
        if (!available()) return 0u;
        const uint32_t full = platform::ops().flash->capacity();
        if (window_offset_ >= full) return 0u;      // окно за пределами Flash
        const uint32_t left = full - window_offset_;
        return (window_len_ != 0u && window_len_ < left) ? window_len_ : left;
    }

    uint32_t page_size() const noexcept override {
        return available() ? platform::ops().flash->page_size() : 0u;
    }

    uint32_t write_granularity() const noexcept override {
        if (!available()) return 1u;
        const uint32_t g = platform::ops().flash->write_granularity();
        return (g == 0u) ? 1u : g;
    }

    const char* name() const noexcept override { return "internal_flash"; }

    Status erase(uint32_t offset, uint32_t len) noexcept override {
        const uint32_t page = page_size();
        if (!available() || page == 0u) return Status::NotSupported;
        if ((offset % page) != 0u || (len % page) != 0u) return Status::InvalidArg;
        if ((offset + len) < offset || (offset + len) > capacity()) return Status::InvalidArg;
        return platform::ops().flash->erase(platform::ops().flash->base() + offset, len)
                 ? Status::Ok : Status::HardwareError;
    }

    Status write(uint32_t offset, const void* data, uint32_t len) noexcept override {
        if (!available()) return Status::NotSupported;
        if (data == nullptr && len != 0u) return Status::InvalidArg;
        if ((offset + len) < offset || (offset + len) > capacity()) return Status::InvalidArg;
        return platform::ops().flash->program(platform::ops().flash->base() + offset,
                                              data, len)
                 ? Status::Ok : Status::HardwareError;
    }

    Status read(uint32_t offset, void* data, uint32_t len) noexcept override {
        if (!available()) return Status::NotSupported;
        if (data == nullptr && len != 0u) return Status::InvalidArg;
        if ((offset + len) < offset || (offset + len) > capacity()) return Status::InvalidArg;
        return platform::ops().flash->read(platform::ops().flash->base() + offset, data, len)
                 ? Status::Ok : Status::HardwareError;
    }

private:
    uint32_t window_offset_ = 0u;
    uint32_t window_len_    = 0u;
    bool     locked_        = true;
};

// ─────────────────────── Запись состояния (метаданные) ─────────────────────
//
// Что именно хранится про слот. Запись переживает сброс питания — на ней
// держится всё: и продолжение передачи, и решение «что грузить», и попытки
// загрузки (защита от «кирпича» из-за прошивки, которая не стартует).

/// Признак, что запись наша: 'M''C''F''O'.
inline constexpr uint32_t kRecordMagic  = 0x4F46434Du;
inline constexpr uint16_t kRecordFormat = 1u;

/// Флаги проверки в записи (что именно подтверждено).
inline constexpr uint32_t kRecFlagCrcOk      = 1u << 0;   ///< CRC32 образа сошлась
inline constexpr uint32_t kRecFlagHashOk     = 1u << 1;   ///< SHA-256 сошёлся
inline constexpr uint32_t kRecFlagSignOk     = 1u << 2;   ///< подпись проверена
inline constexpr uint32_t kRecFlagReadbackOk = 1u << 3;   ///< прочитанное из носителя сошлось
inline constexpr uint32_t kRecFlagTrial      = 1u << 4;   ///< идёт пробная загрузка

/// Слот «нет» (например: активного слота не знаем).
inline constexpr uint8_t kSlotNone = 0xFFu;

/// Сколько страниц занимает запись ОДНОГО слота: рабочая копия + резервная.
inline constexpr uint32_t kMetaPagesPerSlot = 2u;

/// Сколько страниц метаданных нужно хранилищу всего: по две на каждый слот.
/// Разделение обязательно — состояния работающей и принимаемой прошивок
/// описываются ОДНОВРЕМЕННО, и одна общая пара страниц затирала бы одно
/// другим.
inline constexpr uint32_t kMetaPages = kMetaPagesPerSlot * 2u;

struct SlotRecord {
    uint8_t  slot          = 0u;
    uint8_t  state         = static_cast<uint8_t>(proto::State::Idle);
    uint8_t  hash_kind     = static_cast<uint8_t>(proto::HashKind::None);
    uint8_t  reserved0     = 0u;
    uint32_t seq           = 0u;    ///< счётчик записей: больше — новее
    uint32_t image_size    = 0u;    ///< ожидаемый размер образа
    uint32_t image_crc32   = 0u;    ///< ожидаемая CRC32 (из START/VERIFY)
    uint32_t stored_bytes  = 0u;    ///< непрерывно записано байт
    uint32_t crc_at_stored = 0u;    ///< CRC32 образа на длине stored_bytes
    uint32_t fw_version    = 0u;
    uint32_t boot_attempts = 0u;    ///< сколько раз пытались загрузить
    uint32_t flags         = 0u;    ///< kRecFlag*
    uint8_t  sha256[32]{};          ///< ожидаемый SHA-256 (если hash_kind != None)

    bool crc_ok() const noexcept { return (flags & kRecFlagCrcOk) != 0u; }
    bool verified() const noexcept {
        return proto::state_is_loadable(static_cast<proto::State>(state));
    }
};

/// Размер записи, записанной в носитель (явная раскладка, LE).
inline constexpr uint32_t kSlotRecordSize =
    4u      // magic
    + 2u    // format
    + 1u + 1u   // state, slot
    + 2u    // reserved1
    + 4u    // seq
    + 4u    // image_size
    + 4u    // image_crc32
    + 4u    // stored_bytes
    + 4u    // crc_at_stored
    + 4u    // fw_version
    + 4u    // boot_attempts
    + 4u    // flags
    + 32u   // sha256
    + 1u + 1u   // hash_kind, reserved2
    + 4u;   // record_crc32
static_assert(kSlotRecordSize == 80u, "SlotRecord: раскладка");

inline void encode_record(uint8_t* out, const SlotRecord& r) noexcept {
    proto::put_u32(out + 0u, kRecordMagic);
    proto::put_u16(out + 4u, kRecordFormat);
    out[6] = r.state;
    out[7] = r.slot;
    proto::put_u16(out + 8u, 0u);
    proto::put_u32(out + 10u, r.seq);
    proto::put_u32(out + 14u, r.image_size);
    proto::put_u32(out + 18u, r.image_crc32);
    proto::put_u32(out + 22u, r.stored_bytes);
    proto::put_u32(out + 26u, r.crc_at_stored);
    proto::put_u32(out + 30u, r.fw_version);
    proto::put_u32(out + 34u, r.boot_attempts);
    proto::put_u32(out + 38u, r.flags);
    for (uint32_t i = 0u; i < 32u; ++i) out[42u + i] = r.sha256[i];
    out[74] = r.hash_kind;
    out[75] = r.reserved0;
    // CRC32 — по всем полям выше; сам CRC в расчёт не входит.
    proto::put_u32(out + 76u, proto::image_crc(out, 76u));
}

/// Разобрать запись. false — запись не наша, испорчена или чужого формата.
inline bool decode_record(const uint8_t* in, SlotRecord& r) noexcept {
    if (proto::get_u32(in + 0u) != kRecordMagic) return false;
    if (proto::get_u16(in + 4u) != kRecordFormat) return false;
    if (proto::get_u32(in + 76u) != proto::image_crc(in, 76u)) return false;

    r.state         = in[6];
    r.slot          = in[7];
    r.seq           = proto::get_u32(in + 10u);
    r.image_size    = proto::get_u32(in + 14u);
    r.image_crc32   = proto::get_u32(in + 18u);
    r.stored_bytes  = proto::get_u32(in + 22u);
    r.crc_at_stored = proto::get_u32(in + 26u);
    r.fw_version    = proto::get_u32(in + 30u);
    r.boot_attempts = proto::get_u32(in + 34u);
    r.flags         = proto::get_u32(in + 38u);
    for (uint32_t i = 0u; i < 32u; ++i) r.sha256[i] = in[42u + i];
    r.hash_kind     = in[74];
    r.reserved0     = in[75];
    return true;
}

/// Хранилище записей: две копии в соседних страницах.
///
/// Копии нужны ровно для одного: писать поверх СТАРОЙ, не теряя новую.
/// Поэтому store() всегда стирает ту страницу, у которой seq меньше, а load()
/// берёт страницу с большим seq и целой CRC32.
class MetaStore {
public:
    MetaStore() noexcept = default;

    Status init(Storage* storage, uint32_t meta_offset) noexcept {
        if (storage == nullptr) return Status::InvalidArg;
        const uint32_t page = storage->page_size();
        if (page == 0u) return Status::NotSupported;   // метаданным нужны страницы
        if (meta_offset % page != 0u) return Status::InvalidArg;
        if ((meta_offset + page * kMetaPagesPerSlot) > storage->capacity()) {
            return Status::InvalidArg;
        }

        storage_ = storage;
        offset_  = meta_offset;
        page_    = page;
        refresh();
        return Status::Ok;
    }

    /// Перечитать обе копии из носителя.
    ///
    /// ПОЧЕМУ ЧИТАТЬ ПРИ КАЖДОМ ОБРАЩЕНИИ, А НЕ ОДИН РАЗ. Метки пишет не один
    /// владелец: их меняет сервис OTA (приём, активация) и загрузчик (счётчик
    /// попыток). Каждый из них держит свой объект хранилища, и «закэшированное»
    /// состояние одного мгновенно становится ложью для другого — а цена лжи
    /// здесь максимальная: запись поверх НОВОЙ копии (потеря активации) или
    /// «продолжение с неверного смещения» при загрузке. Чтение 80 байт из
    /// Flash дешевле любой из этих ошибок.

    bool initialized() const noexcept { return storage_ != nullptr; }

    /// Страница под запись (та, чья копия старше).
    uint32_t next_page_index() const noexcept {
        if (!has_a_) return 0u;
        if (!has_b_) return 1u;
        return (rec_a_.seq <= rec_b_.seq) ? 0u : 1u;
    }

    /// Прочитать более новую валидную запись для слота (с перечитыванием).
    /// Возврат: Ok | NotFound (обе копии пусты/испорчены/относятся к другому слоту).
    Status load(uint8_t slot, SlotRecord& out) noexcept {
        if (storage_ == nullptr) return Status::Unavailable;
        refresh();

        bool found = false;
        SlotRecord best{};
        if (has_a_ && rec_a_.slot == slot) { best = rec_a_; found = true; }
        if (has_b_ && rec_b_.slot == slot) {
            if (!found || rec_b_.seq > best.seq) { best = rec_b_; found = true; }
        }
        if (!found) return Status::NotFound;
        out = best;
        return Status::Ok;
    }

    /// Записать запись: seq увеличивается автоматически (иначе копии нельзя
    /// было бы сравнивать), пишется поверх старшей по номеру страницы.
    Status store(SlotRecord& rec) noexcept {
        if (storage_ == nullptr) return Status::Unavailable;
        // Перед записью — перечитать: иначе seq и выбор «какую копию затирать»
        // считались бы по устаревшему состоянию.
        refresh();

        rec.seq = next_seq();
        const uint32_t index = next_page_index();
        const Status st = write_copy(index, rec);
        if (st != Status::Ok) return st;

        if (index == 0u) { rec_a_ = rec; has_a_ = true; }
        else             { rec_b_ = rec; has_b_ = true; }
        return Status::Ok;
    }

    /// Стереть обе копии (полный сброс метаданных).
    Status clear() noexcept {
        if (storage_ == nullptr) return Status::Unavailable;
        const Status st = storage_->erase(offset_, page_ * kMetaPagesPerSlot);
        if (st != Status::Ok) return st;
        has_a_ = has_b_ = false;
        rec_a_ = SlotRecord{};
        rec_b_ = SlotRecord{};
        seq_   = 0u;
        return Status::Ok;
    }

    uint32_t meta_offset() const noexcept { return offset_; }
    uint32_t page_size() const noexcept { return page_; }
    uint32_t last_seq() const noexcept { return seq_; }

private:
    void refresh() noexcept {
        SlotRecord a{}, b{};
        has_a_ = read_copy(0u, a);
        has_b_ = read_copy(1u, b);
        rec_a_ = a;
        rec_b_ = b;
    }

    uint32_t next_seq() noexcept {
        uint32_t m = seq_;
        if (has_a_ && rec_a_.seq > m) m = rec_a_.seq;
        if (has_b_ && rec_b_.seq > m) m = rec_b_.seq;
        seq_ = m + 1u;
        return seq_;
    }

    bool read_copy(uint32_t index, SlotRecord& out) noexcept {
        uint8_t buf[kSlotRecordSize]{};
        if (storage_->read(offset_ + index * page_, buf, kSlotRecordSize) != Status::Ok) {
            return false;
        }
        return decode_record(buf, out);
    }

    Status write_copy(uint32_t index, const SlotRecord& rec) noexcept {
        const uint32_t addr = offset_ + index * page_;
        Status st = storage_->erase(addr, page_);
        if (st != Status::Ok) return st;

        uint8_t buf[kSlotRecordSize]{};
        encode_record(buf, rec);
        st = storage_->write(addr, buf, kSlotRecordSize);
        if (st != Status::Ok) return st;

        // Обратная проверка: записалось ли. Именно она отличает «write вернул
        // Ok» от «в носителе лежит то, что мы записали».
        uint8_t back[kSlotRecordSize]{};
        st = storage_->read(addr, back, kSlotRecordSize);
        if (st != Status::Ok) return st;
        for (uint32_t i = 0u; i < kSlotRecordSize; ++i) {
            if (back[i] != buf[i]) return Status::Corruption;
        }
        return Status::Ok;
    }

    Storage* storage_ = nullptr;
    uint32_t offset_ = 0u;
    uint32_t page_   = 0u;

    bool       has_a_ = false;
    bool       has_b_ = false;
    SlotRecord rec_a_{};
    SlotRecord rec_b_{};
    uint32_t   seq_ = 0u;
};

// ───────────────────────── Проверка раскладки ──────────────────────────────
//
// Сервис обязан отвергнуть конфигурацию, при которой «слот перекрывает
// метаданные» или «образ не влезает в слот». Такая ошибка в настройках
// проявилась бы уничтожением уже принятых данных посреди передачи.

struct LayoutCheck {
    bool     ok         = false;
    Status   status     = Status::InvalidArg;
    uint32_t image_limit = 0u;   ///< максимальный размер образа при этой раскладке
};

inline LayoutCheck check_layout(const Storage& storage,
                               uint32_t meta_offset,
                               const uint32_t slot_offset[2],
                               const uint32_t slot_capacity[2]) noexcept {
    LayoutCheck out{};
    const uint32_t page = storage.page_size();
    const uint32_t cap  = storage.capacity();

    if (page == 0u || cap == 0u) { out.status = Status::NotSupported; return out; }
    if ((meta_offset % page) != 0u) return out;

    const uint32_t meta_len = page * kMetaPages;   // обе копии обоих слотов
    if (meta_offset + meta_len > cap) return out;

    for (uint32_t s = 0u; s < 2u; ++s) {
        if (slot_capacity[s] == 0u) continue;
        if ((slot_offset[s] % page) != 0u) return out;
        if (slot_offset[s] > cap) return out;
        // Ёмкость слота обязана быть кратной странице: иначе последняя
        // страница слота окажется «половинчатой», и стирание хвоста получит
        // длину, не кратную странице, — то есть отказ носителя посреди
        // передачи, когда слот уже частично стёрт.
        if ((slot_capacity[s] % page) != 0u) return out;
        // Вычитание, а не сложение: сумма смещения и ёмкости при «щедрых»
        // числах в настройке переполнила бы uint32 и проверка прошла бы,
        // пропустив слот за пределами носителя.
        if (slot_capacity[s] > (cap - slot_offset[s])) return out;
        // Перекрытие с метаданными.
        if (slot_offset[s] < meta_offset + meta_len &&
            meta_offset < slot_offset[s] + slot_capacity[s]) {
            return out;
        }
        // Перекрытие слотов друг с другом.
        const uint32_t other = (s == 0u) ? 1u : 0u;
        if (slot_capacity[other] != 0u &&
            slot_offset[s] < slot_offset[other] + slot_capacity[other] &&
            slot_offset[other] < slot_offset[s] + slot_capacity[s]) {
            return out;
        }
        if (out.image_limit == 0u || slot_capacity[s] < out.image_limit) {
            out.image_limit = slot_capacity[s];
        }
    }
    if (out.image_limit == 0u) return out;

    out.ok = true;
    out.status = Status::Ok;
    return out;
}


// ───────────────── Раскладка отметки об обновлении (резервные регистры) ─────
//
// Резервные регистры (у STM32 — BKP) переживают сброс, поэтому именно туда
// пишется «шла пробная загрузка». Это нужно, чтобы после сброса посреди
// обновления было видно не только «что лежит в слоте», но и «сколько раз
// пытались загрузить» — иначе устройство, которое валится на старте новой
// прошивки, выглядело бы как исправное.
constexpr uint16_t kNoteIdxMagic    = 0u;   ///< признак «регистры наши»
constexpr uint16_t kNoteIdxState    = 1u;   ///< состояние слота
constexpr uint16_t kNoteIdxAttempts = 2u;   ///< попыток загрузки
constexpr uint16_t kNoteIdxSlot     = 3u;   ///< слот
constexpr uint16_t kNoteCount       = 4u;   ///< сколько регистров нужно

/// 'O','T' — признак, что отметка принадлежит OTA.
constexpr uint16_t kNoteMagic = 0x4F54u;

// ─────────── Политика загрузки: ОДНО место на приложение и загрузчик ─────────
//
// Пробные загрузки, активация, подтверждение, откат и «слот негоден» — это
// правила, которые обязаны совпадать у сервиса OTA (он принимает образ и
// оставляет метки) и у загрузчика (он по этим меткам решает, кого грузить).
// Разошедшись, они дают самый неприятный класс дефектов: устройство грузит не
// тот слот или не считает попытки, и выглядит это как «железо глючит».
//
// Поэтому правила живут здесь — маленькими функциями над записью слота, без
// ввода-вывода, — а и сервис, и загрузчик вызывают их. Ввод-вывод (чтение и
// запись записей) — SlotStore ниже: он тоже общий, но загрузчику из него нужен
// минимум, поэтому сервис OTA и загрузчик не тянут код друг друга.

/// Слот, который следует грузить (по записям двух слотов).
///
/// Приоритет: явно активированный слот важнее всего (это просьба хоста),
/// затем подтверждённый (Running — он уже работал), затем проверенный.
/// Слот, из которого работаем СЕЙЧАС (`current_slot`), при равных состояниях
/// предпочтительнее: его счётчик попыток относится к нему, а не к соседу.
inline int boot_rank(const SlotRecord& rec) noexcept {
    switch (static_cast<proto::State>(rec.state)) {
        case proto::State::Activated: return 3;
        case proto::State::Running:   return 2;
        case proto::State::Verified:  return 1;
        default:                      return 0;   // грузить нечего
    }
}

/// Годен ли слот к загрузке по своей записи (без проверки образа).
inline bool record_loadable(const SlotRecord& rec) noexcept {
    return proto::state_is_loadable(static_cast<proto::State>(rec.state));
}

/// Отметить слот активированным: «грузить при следующей загрузке».
inline void record_mark_activated(SlotRecord& rec) noexcept {
    rec.state         = static_cast<uint8_t>(proto::State::Activated);
    rec.boot_attempts = 0u;
    rec.flags        |= kRecFlagTrial;
}

/// Снять активацию, оставив образ годным (его можно активировать снова).
inline void record_mark_verified(SlotRecord& rec) noexcept {
    rec.state         = static_cast<uint8_t>(proto::State::Verified);
    rec.boot_attempts = 0u;
    rec.flags        &= ~kRecFlagTrial;
}

/// Подтверждение работы: прошивка запустилась и живёт.
inline void record_mark_running(SlotRecord& rec) noexcept {
    rec.state         = static_cast<uint8_t>(proto::State::Running);
    rec.boot_attempts = 0u;
    rec.flags        &= ~kRecFlagTrial;
}

/// Слот негоден (провал проверки, порча образа, отказ стирания): грузить его
/// нельзя, и метка обязана это говорить.
inline void record_mark_failed(SlotRecord& rec, uint16_t detail = 0u) noexcept {
    rec.state         = static_cast<uint8_t>(proto::State::Failed);
    rec.boot_attempts = 0u;
    rec.flags         = 0u;
    rec.reserved0     = static_cast<uint8_t>(detail & 0xFFu);
}

/// Учесть попытку пробной загрузки.
///
/// Возврат: true — слот исчерпал разрешённые попытки, и приложение (или
/// загрузчик) обязано вернуться к прежнему слоту. Это и есть защита от
/// «кирпича»: прошивка, падающая на старте, не может перезагружаться вечно.
inline bool record_take_trial(SlotRecord& rec, uint32_t max_attempts) noexcept {
    const uint32_t attempts = rec.boot_attempts + 1u;
    rec.boot_attempts = attempts;
    rec.flags |= kRecFlagTrial;
    return (max_attempts != 0u) && (attempts > max_attempts);
}

/// Хранилище записей состояния: ДВА слота, у каждого своя пара страниц.
///
/// Один объект на все нужды: сервис OTA читает и пишет метки приёма, загрузчик —
/// только читает и правит счётчики загрузки. Логика адресации страниц (две на
/// слот) живёт здесь, а не повторяется в каждом вызывающем.
class SlotStore {
public:
    SlotStore() noexcept = default;

    /// Привязать хранилище. meta_offset — начало области метаданных
    /// (первая из четырёх страниц: по две на слот).
    Status init(Storage* storage, uint32_t meta_offset) noexcept {
        if (storage == nullptr) return Status::InvalidArg;
        const uint32_t page = storage->page_size();
        if (page == 0u) return Status::NotSupported;
        if ((meta_offset % page) != 0u) return Status::InvalidArg;
        if ((meta_offset + page * kMetaPages) > storage->capacity()) {
            return Status::InvalidArg;
        }

        storage_ = storage;
        meta_offset_ = meta_offset;
        per_slot_ = page * kMetaPagesPerSlot;

        for (uint8_t s = 0u; s < 2u; ++s) {
            const Status st = store_[s].init(storage, meta_offset + s * per_slot_);
            if (st != Status::Ok) return st;
        }
        return Status::Ok;
    }

    bool initialized() const noexcept { return storage_ != nullptr; }

    /// Прочитать запись слота: Ok | NotFound (записи нет).
    Status load(uint8_t slot, SlotRecord& out) noexcept {
        if (storage_ == nullptr) return Status::Unavailable;
        if (slot > 1u) return Status::InvalidArg;
        return store_[slot].load(slot, out);
    }

    /// Записать запись слота (счётчик seq ведётся хранилищем).
    Status store(uint8_t slot, const SlotRecord& rec) noexcept {
        if (storage_ == nullptr) return Status::Unavailable;
        if (slot > 1u) return Status::InvalidArg;
        SlotRecord copy = rec;
        copy.slot = slot;
        return store_[slot].store(copy);
    }

    /// Пометить слот негодным (запись создаётся, если её не было).
    Status invalidate(uint8_t slot, uint16_t detail = 0u) noexcept {
        if (storage_ == nullptr) return Status::Unavailable;
        if (slot > 1u) return Status::InvalidArg;

        SlotRecord rec{};
        if (load(slot, rec) != Status::Ok) {
            rec = SlotRecord{};
            rec.slot = slot;
        }
        record_mark_failed(rec, detail);
        return store(slot, rec);
    }

    /// Слот, который следует грузить, и его запись.
    /// Layout::kNoSlot — грузить нечего.
    uint8_t pick(uint8_t current_slot, SlotRecord& out) noexcept {
        uint8_t best = 0xFFu;
        int best_rank = 0;
        for (uint8_t s = 0u; s < 2u; ++s) {
            SlotRecord rec{};
            if (load(s, rec) != Status::Ok) continue;
            if (!record_loadable(rec)) continue;
            int rank = boot_rank(rec);
            if (s == current_slot) rank += 1;
            if (rank > best_rank) { best_rank = rank; best = s; out = rec; }
        }
        return best;
    }

private:
    Storage* storage_ = nullptr;
    uint32_t meta_offset_ = 0u;
    uint32_t per_slot_ = 0u;
    MetaStore store_[2]{};
};

} // namespace embed::ota
