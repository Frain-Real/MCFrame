// os/ota/ota_protocol.hpp
#pragma once
// ---------------------------------------------------------------------------
// ПРОТОКОЛ ПРИЁМА ПРОШИВКИ (MCFrame OTA Link Protocol, MOLP v1).
//
// Здесь описана ПРОВОДНАЯ часть OTA: как выглядит кадр, какие операции
// существуют, что каждая сторона обязана сделать и как ведёт себя приёмник,
// когда кадр испорчен, потерян, пришёл не в том порядке или питание пропало
// посреди передачи. Логика сервиса (хранилище, сессия, активация) — в ota.hpp.
//
// ЗАЧЕМ СВОЙ ПРОТОКОЛ, А НЕ «ЗАЛИТЬ БИНАРНИК ЧЕРЕЗ XMODEM»
//   * XMODEM-подобные протоколы передают ПОТОК и ничего не знают про образ:
//     они не отвечают на вопросы «сколько байт уже записано», «какая версия
//     лежит в слоте», «это тот же образ или другой». При обрыве связи
//     передачу приходится начинать с нуля;
//   * здесь единица обмена — АДРЕСУЕМЫЙ блок (offset + данные), а ответ всегда
//     несёт next_offset: сколько байт принято НЕПРЕРЫВНО и записано. Поэтому
//     повтор кадра идемпотентен, а возобновление после потери питания или
//     сбоя связи — не «особый случай», а обычный режим;
//   * целостность и аутентичность разделены: CRC32 ловит порчу при передаче,
//     SHA-256 — порчу при хранении, подпись — подмену образа. Образ, не
//     прошедший проверку, НИКОГДА не помечается готовым к загрузке.
//
// ─────────────────────────────── ФОРМАТ КАДРА ───────────────────────────────
//
// Все многобайтные поля — little-endian. Размер кадра = 13 + LEN.
//
//   смещение  размер  поле   назначение
//   --------  ------  -----  -------------------------------------------------
//      0        1     SOF0   0x4F ('O')
//      1        1     SOF1   0x54 ('T')
//      2        1     VER    версия протокола (kVersion = 1)
//      3        1     OP     операция (Op)
//      4        1     FLAGS  флаги кадра (kFlag*)
//      5        1     SEQ    номер кадра в направлении (0..255, циклически)
//      6..7     2     LEN    длина полезной нагрузки, байт (0..max_payload)
//      8        1     HCRC   CRC-8/SMBUS по байтам 2..7
//      9..       LEN   PAYLOAD
//      9+LEN     4     CRC32  CRC-32/ISO-HDLC по байтам 2..(8+LEN)
//
// ПОЧЕМУ ДВА CRC И ЗАЧЕМ HCRC ОТДЕЛЬНО
// HCRC закрывает только заголовок и проверяется ДО того, как приёмник поверит
// полю LEN. Без него испорченная длина заставила бы приёмник ждать
// несуществующие байты (или проглотить начало следующего кадра) — то есть
// терять синхронизацию из-за одной ошибки в линии. CRC32 закрывает весь кадр
// и является окончательным приговором: не сошёлся — кадр не обрабатывается.
//
// ПОЧЕМУ CRC-32/ISO-HDLC (zlib), А НЕ АППАРАТНЫЙ CRC STM32
// Аппаратный блок CRC у STM32F1 считает CRC-32/MPEG-2 (пресет
// crypto::Crc32Stm32). Выбран zlib-вариант, потому что ту же CRC32 умеют
// zlib, Python (zlib.crc32), crc32 из coreutils и любой хост-инструмент:
// сборщику образа не нужна наша таблица, чтобы посчитать ожидаемое значение.
// Цена — программный расчёт (crypto::Crc32Stm32 существует, если позже
// понадобится ускорение, но тогда его обязан считать и хост).
//
// ────────────────────────────── ОБМЕН СООБЩЕНИЯМИ ─────────────────────────────
//
// Полудуплекс, строго «запрос — ответ», окно = 1: хост не отправляет
// следующий кадр, пока не получил ответ на предыдущий (либо не истёк
// таймаут). Это осознанный выбор для МКУ: не нужны буферы под несколько
// кадров в полёте, а потеря ответа стоит одного повтора, а не переполнения.
//
//   хост                                 устройство
//   ────────────────────────────────────────────────────────────────────────
//   INFO         ──────────────────────►  ACK(Info)              возможности
//   START        ──────────────────────►  ACK(next_offset=0)     сессия открыта
//   DATA(0)      ──────────────────────►  ACK(next_offset=n)     записано n байт
//   DATA(n)      ──────────────────────►  ACK(next_offset=2n)
//   ...                                   (DATA с тем же offset — повтор:
//                                          ACK с текущим next_offset)
//   END(size, crc, sha) ───────────────►  ACK(Verified)          образ проверен
//   SIGN(kind, sig)     ───────────────►  ACK(Verified)          подпись принята
//   ACTIVATE(slot)      ───────────────►  ACK(Activated)         грузим при сбросе
//   CONFIRM             ───────────────►  ACK(Running)           новая прошивка
//                                                                подтвердила себя
//
// ОТВЕТЫ: ACK (Op::Ack, code = Code::Ok) и NAK (Op::Nak, code != Ok) устроены
// одинаково (payload = Response), чтобы у хоста был ОДИН разбор ответа.
// SEQ ответа — свой счётчик устройства, а не эхо: хост сопоставляет ответ с
// запросом по порядку (окно = 1), а SEQ отвечает за целостность самого ответа.
//
// ─────────────────────── ПРАВИЛА ПРИЁМА (обязательства приёмника) ─────────────
//
//   1. КАДР, НЕ ПРОШЕДШИЙ CRC, НЕ ОБРАБАТЫВАЕТСЯ. Если при этом удалось
//      разобрать SEQ — отвечаем NAK(BadFrame, seq), иначе молчим: отвечать
//      на мусор значит удваивать трафик и мешать хосту найти синхронизацию.
//      ВАЖНО ДЛЯ ХОСТА: такой NAK приходит ВНЕ ОЧЕРЕДИ (он не ответ на
//      текущий запрос), и его следует игнорировать, если хост не ждёт ответа
//      или уже получил свой. Частота таких ответов ограничена приёмником
//      (Config::bad_frame_nak_period_ms), поэтому «залить» ими канал нельзя.
//   2. СИНХРОНИЗАЦИЯ ВОССТАНАВЛИВАЕТСЯ СКАНИРОВАНИЕМ SOF. Приёмник не
//      «съезжает» навсегда после одной ошибки: начало кадра сдвигается на
//      байт, и те же байты разбираются заново. Поэтому кадр, начинающийся
//      внутри чужой полезной нагрузки, тоже будет найден.
//   3. ПОВТОР DATA ИДЕМПОТЕНТЕН. offset < next_offset — это ретрансмиссия:
//      данные НЕ перезаписываются, отвечаем ACK с текущим next_offset.
//   4. РАЗРЫВ ОБНАРУЖИВАЕТСЯ ПО OFFSET. offset > next_offset означает
//      пропуск: NAK(BadOffset) с next_offset, и хост продолжает с него.
//      Так потеря кадра не превращается в дыру в прошивке.
//   5. ЗАПИСЬ ТОЛЬКО ВПЕРЁД. Образ пишется строго последовательно (это
//      требование к NOR-flash: записать мимо стирания нельзя), поэтому
//      «принять блок 5, потом блок 3» нельзя — и не нужно.
//   6. НЕПРИНЯТОЕ НЕ АКТИВИРУЕТСЯ. Слот считается готовым только после
//      END и проверки CRC32 (и хеша/подписи, если они заявлены), и только
//      тогда он может быть активирован.
//   7. ОБРЫВ СВЯЗИ — НЕ ОШИБКА, А ПАУЗА. Сессия после session_timeout
//      закрывается, но запись состояния (stored_bytes + CRC на этой длине)
//      остаётся в метаданных: следующий START с флагом Resume проверит
//      прочитанный из хранилища префикс и продолжит с того, что реально
//      лежит, а не с того, что помнит хост.
// ---------------------------------------------------------------------------

#include "../common.hpp"
#include "../crypto/crc.hpp"

#include <cstddef>
#include <cstdint>

namespace embed::ota::proto {

// ─────────────────────────────── Константы кадра ────────────────────────────

inline constexpr uint8_t kSof0 = 0x4Fu;   ///< 'O'
inline constexpr uint8_t kSof1 = 0x54u;   ///< 'T'
inline constexpr uint8_t kVersion = 1u;

inline constexpr Size kHeaderInFrame = 9u;    ///< SOF..HCRC
inline constexpr Size kCrcInFrame    = 4u;    ///< CRC32 в конце кадра
inline constexpr Size kFrameOverhead = kHeaderInFrame + kCrcInFrame;   // 13
inline constexpr Size kMinFrame      = kFrameOverhead;

/// Предел полезной нагрузки по умолчанию. Переопределяется сборкой
/// (-DMCFRAME_OTA_MAX_PAYLOAD=256) — это же число хост узнаёт из INFO.
///
/// Почему по умолчанию 128: буфер приёмника живёт в статике сервиса
/// (kFrameOverhead + max_payload), а на STM32F103 20 КБ RAM на всё. 128 байт
/// payload дают 141-байтный буфер и приличную скорость на 115200 бод (канал
/// занят кадром ~12 мс, накладные расходы 13/141 ≈ 9 %).
#ifndef MCFRAME_OTA_MAX_PAYLOAD
#define MCFRAME_OTA_MAX_PAYLOAD 128u
#endif

/// Предел payload для кадров команд (START/INFO/...) — он же общий предел.
inline constexpr uint16_t kMaxPayload = static_cast<uint16_t>(MCFRAME_OTA_MAX_PAYLOAD);

/// Предел подписи (фрагментируется по kFlagMore).
/// 80 байт: ECDSA P-256 (64) и Ed25519 (64) влезают целиком, HMAC-SHA256 (32)
/// тем более. RSA-2048 требует 256 — поднимайте MCFRAME_OTA_SIGNATURE_MAX.
#ifndef MCFRAME_OTA_SIGNATURE_MAX
#define MCFRAME_OTA_SIGNATURE_MAX 80u
#endif
inline constexpr uint16_t kMaxSignature = static_cast<uint16_t>(MCFRAME_OTA_SIGNATURE_MAX);

/// Наибольшая длина payload, которую приёмник согласен «проглотить», НЕ
/// сохраняя её (см. фазу Skip декодера).
///
/// Поле LEN испорчено? Тогда кадр «на 30 КБ» проглотил бы вместе с собой и все
/// следующие кадры — приёмник замолчал бы надолго. Поэтому кадр, который
/// БОЛЬШЕ этого предела, считается не «большим кадром», а испорченным
/// заголовком: начало кадра сдвигается на байт, и настоящие кадры находятся.
/// Предел заведомо больше любого честного блока данных (max_payload), поэтому
/// «хост не послушался INFO и прислал блок побольше» по-прежнему получает
/// внятный отказ TooLarge.
#ifndef MCFRAME_OTA_MAX_SKIP
#define MCFRAME_OTA_MAX_SKIP 1024u
#endif
inline constexpr uint16_t kMaxSkipLen = static_cast<uint16_t>(MCFRAME_OTA_MAX_SKIP);

static_assert(kMaxPayload >= 64u, "OTA: payload меньше 64 байт ломает START/END");

// ───────────────────────────────── Операции ────────────────────────────────

/// Операция кадра. 0x00..0x7F — запросы хоста, 0x80.. — ответы устройства.
enum class Op : uint8_t {
    // ── запросы хоста ──
    Info    = 0x01u,   ///< возможности устройства и раскладка слотов
    Start   = 0x02u,   ///< открыть сессию (размер, CRC, версия, слот, resume)
    Data    = 0x03u,   ///< блок образа: offset(4) + данные
    End     = 0x04u,   ///< конец образа: проверить целостность
    Sign    = 0x05u,   ///< подпись образа (может фрагментироваться)
    Abort   = 0x06u,   ///< отменить сессию
    Erase   = 0x07u,   ///< стереть слот целиком
    Verify  = 0x08u,   ///< проверить УЖЕ ЛЕЖАЩИЙ в слоте образ
    Activate= 0x09u,   ///< пометить готовый слот как «грузить при сбросе»
    Confirm = 0x0Au,   ///< подтвердить работу текущей прошивки (защита от отката)
    State   = 0x0Bu,   ///< запросить состояние сессии/слота
    Read    = 0x0Cu,   ///< прочитать байты из слота (диагностика/сверка)

    // ── ответы устройства ──
    Ack     = 0x80u,   ///< принято (payload = Response, code = Ok)
    Nak     = 0x81u,   ///< отказ (payload = Response, code != Ok)
};

/// Текстовое имя операции — для логов и отчётов.
inline const char* op_text(Op op) noexcept {
    switch (op) {
        case Op::Info:     return "INFO";
        case Op::Start:    return "START";
        case Op::Data:     return "DATA";
        case Op::End:      return "END";
        case Op::Sign:     return "SIGN";
        case Op::Abort:    return "ABORT";
        case Op::Erase:    return "ERASE";
        case Op::Verify:   return "VERIFY";
        case Op::Activate: return "ACTIVATE";
        case Op::Confirm:  return "CONFIRM";
        case Op::State:    return "STATE";
        case Op::Read:     return "READ";
        case Op::Ack:      return "ACK";
        case Op::Nak:      return "NAK";
    }
    return "?";
}

// ─────────────────────────────── Флаги кадра ───────────────────────────────

/// Хост просит ответ на кадр, который обычно ответа не требует.
inline constexpr uint8_t kFlagAckReq = 1u << 0;
/// В payload есть продолжение (фрагментация длинных команд, например подписей).
inline constexpr uint8_t kFlagMore   = 1u << 1;
/// Кадр — повтор после таймаута (диагностика на стороне устройства).
inline constexpr uint8_t kFlagRetry  = 1u << 2;

// ──────────────────────────────── Коды ответов ─────────────────────────────

/// Код результата в ответе устройства.
///
/// Отдельный тип вместо Status, потому что это ЧАСТЬ ПРОТОКОЛА: значения
/// уходят в провод, и по ним работает хост-инструмент, а не только наш код.
enum class Code : uint8_t {
    Ok          = 0u,    ///< принято
    BadFrame    = 1u,    ///< кадр испорчен: CRC/формат/версия
    BadState    = 2u,    ///< операция недопустима в текущем состоянии
    BadOffset   = 3u,    ///< смещение вне образа или не равно next_offset
    NoSpace     = 4u,    ///< образ больше слота
    Storage     = 5u,    ///< отказ носителя (стирание/запись/чтение)
    Verify      = 6u,    ///< CRC/хеш/подпись не сошлись
    Busy        = 7u,    ///< занято: идёт другая сессия или операция
    Unsupported = 8u,    ///< операция/флаг/алгоритм не поддержаны
    Auth        = 9u,    ///< подпись не проверена (нет ключа, отказ проверки)
    TooLarge    = 10u,   ///< payload/образ больше допустимого
    Timeout     = 11u,   ///< истёк срок кадра или сессии
    Denied      = 12u,   ///< запрещено политикой (откат версии, чужой слот)
    BadRequest  = 13u,   ///< некорректная полезная нагрузка команды
};

inline const char* code_text(Code code) noexcept {
    switch (code) {
        case Code::Ok:          return "OK";
        case Code::BadFrame:    return "BAD_FRAME";
        case Code::BadState:    return "BAD_STATE";
        case Code::BadOffset:   return "BAD_OFFSET";
        case Code::NoSpace:     return "NO_SPACE";
        case Code::Storage:     return "STORAGE";
        case Code::Verify:      return "VERIFY";
        case Code::Busy:        return "BUSY";
        case Code::Unsupported: return "UNSUPPORTED";
        case Code::Auth:        return "AUTH";
        case Code::TooLarge:    return "TOO_LARGE";
        case Code::Timeout:     return "TIMEOUT";
        case Code::Denied:      return "DENIED";
        case Code::BadRequest:  return "BAD_REQUEST";
    }
    return "?";
}

// ─────────────────────────────── Состояния ─────────────────────────────────

/// Состояние сессии/слота. Уходит в ответах (Response::state) и в метаданные
/// хранилища (SlotRecord::state) — значения обязаны совпадать, поэтому тип
/// один на оба места.
enum class State : uint8_t {
    Idle      = 0u,   ///< сессии нет, слот пуст
    Started   = 1u,   ///< сессия открыта, байты ещё не писались
    Receiving = 2u,   ///< идёт приём (часть образа уже записана)
    Complete  = 3u,   ///< все байты записаны, целостность НЕ подтверждена
    Verified  = 4u,   ///< CRC (и хеш/подпись) сошлись: образ годен
    Activated = 5u,   ///< помечен «грузить при следующей перезагрузке»
    Running   = 6u,   ///< запущен и подтверждён приложением
    Failed    = 7u,   ///< сессия провалена: ошибка записи или проверки
};

inline const char* state_text(State st) noexcept {
    switch (st) {
        case State::Idle:      return "IDLE";
        case State::Started:   return "STARTED";
        case State::Receiving: return "RECEIVING";
        case State::Complete:  return "COMPLETE";
        case State::Verified:  return "VERIFIED";
        case State::Activated: return "ACTIVATED";
        case State::Running:   return "RUNNING";
        case State::Failed:    return "FAILED";
    }
    return "?";
}

/// Готов ли образ к загрузке (проверен и/или активирован).
inline constexpr bool state_is_loadable(State st) noexcept {
    return st == State::Verified || st == State::Activated || st == State::Running;
}

/// Принимает ли слот данные в этом состоянии (сессия жива).
inline constexpr bool state_accepts_data(State st) noexcept {
    return st == State::Started || st == State::Receiving;
}

// ─────────────────────────── Виды транспорта ───────────────────────────────

/// По какому интерфейсу устройство принимает прошивку. Значение уходит в
/// INFO, поэтому это перечислитель протокола, а не внутренний тип.
enum class Iface : uint8_t {
    None     = 0u,   ///< интерфейса нет: образ уже лежит в хранилище
    Memory   = 1u,   ///< буфер в памяти (стенд, тесты, «залито из ОЗУ»)
    Uart     = 2u,   ///< UART/USART (в том числе USB-UART, RS-485)
    I2c      = 3u,   ///< I2C в режиме ведомого
    Spi      = 4u,   ///< SPI в режиме ведомого
    Usb      = 5u,   ///< USB CDC
    Custom   = 6u,   ///< свой транспорт (приложение сообщает вид)
};

inline const char* iface_text(Iface f) noexcept {
    switch (f) {
        case Iface::None:   return "none";
        case Iface::Memory: return "memory";
        case Iface::Uart:   return "uart";
        case Iface::I2c:    return "i2c";
        case Iface::Spi:    return "spi";
        case Iface::Usb:    return "usb";
        case Iface::Custom: return "custom";
    }
    return "?";
}

// ────────────────────────── Возможности в INFO ─────────────────────────────

inline constexpr uint8_t kCapSha256    = 1u << 0;   ///< умеет хешировать образ
inline constexpr uint8_t kCapSign      = 1u << 1;   ///< умеет проверять подпись
inline constexpr uint8_t kCapResume    = 1u << 2;   ///< умеет продолжать передачу
inline constexpr uint8_t kCapDualSlot  = 1u << 3;   ///< два слота (A/B)
inline constexpr uint8_t kCapReadback  = 1u << 4;   ///< сверяет прочитанное из носителя
inline constexpr uint8_t kCapVerify    = 1u << 5;   ///< умеет проверять готовый слот

/// Вид хеша образа в START/END/VERIFY.
enum class HashKind : uint8_t {
    None   = 0u,   ///< только CRC32 (быстро, ловит порчу, не ловит подмену)
    Sha256 = 1u,   ///< CRC32 + SHA-256
};

// ────────────────────── Вспомогательное: запись чисел LE ───────────────────

inline void put_u16(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void put_u32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline uint16_t get_u16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t get_u32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// ─────────────────────────── CRC образа и заголовка ────────────────────────

/// CRC образа: сырое состояние -> update -> finish (см. crypto::Crc).
using ImageCrc = crypto::Crc32Zlib;

inline uint32_t image_crc_begin() noexcept { return ImageCrc::init(); }
inline uint32_t image_crc_update(uint32_t crc, const void* data, Size len) noexcept {
    return ImageCrc::update(static_cast<uint32_t>(crc), data, len);
}
inline uint32_t image_crc_finish(uint32_t crc) noexcept {
    return ImageCrc::finish(static_cast<uint32_t>(crc));
}
inline uint32_t image_crc(const void* data, Size len) noexcept {
    return ImageCrc::compute(data, len);
}

/// «Сырое» состояние CRC32, соответствующее такому финальному значению.
///
/// Нужно, чтобы ПРОДОЛЖИТЬ расчёт с середины образа: запись состояния хранит
/// CRC префикса в окончательном виде (его можно сверить хост-инструментом), а
/// инкрементальный расчёт продолжается с состояния ДО финального XOR.
/// Обратимость опирается на xorout = 0xFFFFFFFF (стандарт zlib) — при смене
/// пресета эту функцию придётся менять вместе с ним.
inline uint32_t image_crc_resume_state(uint32_t finished) noexcept {
    return finished ^ 0xFFFFFFFFu;
}

/// CRC заголовка (HCRC): считается по байтам 2..7 кадра.
inline uint8_t header_crc(const uint8_t* frame) noexcept {
    return crypto::Crc8SMBus::compute(frame + 2, 6u);
}

// ─────────────────────────── Полезная нагрузка команд ──────────────────────
//
// Раскладка задана явно функциями encode/decode, а НЕ приведением struct к
// байтам: упаковка в памяти (padding, alignment, порядок байт) — свойство
// компилятора, а не протокола. На хосте и на МКУ структуры совпали бы, но
// ровно до смены компилятора или добавления поля.

/// INFO (запрос): payload пуст, но хост МОЖЕТ прислать свою версию протокола
/// одним байтом — устройство отвергнет несовместимую явно, а не «странно
/// себя поведёт».
inline constexpr Size kInfoReqSize = 1u;

/// INFO (ответ): возможности и раскладка. 32 байта.
struct Info {
    uint16_t proto_version = kVersion;
    uint8_t  iface         = static_cast<uint8_t>(Iface::None);
    uint8_t  caps          = 0u;
    uint16_t max_payload   = kMaxPayload;
    uint16_t page_size     = 0u;
    uint32_t fw_version    = 0u;
    uint32_t slot_offset[2]{};
    uint32_t slot_capacity[2]{};
    uint32_t image_limit   = 0u;   ///< предел размера образа = min(slot_capacity)
};

inline constexpr Size kInfoSize = 32u;
static_assert(kInfoSize == 2u + 1u + 1u + 2u + 2u + 4u + 4u * 4u + 4u, "INFO: раскладка");

inline void encode_info(uint8_t* out, const Info& in) noexcept {
    put_u16(out + 0u, in.proto_version);
    out[2] = in.iface;
    out[3] = in.caps;
    put_u16(out + 4u, in.max_payload);
    put_u16(out + 6u, in.page_size);
    put_u32(out + 8u, in.fw_version);
    put_u32(out + 12u, in.slot_offset[0]);
    put_u32(out + 16u, in.slot_offset[1]);
    put_u32(out + 20u, in.slot_capacity[0]);
    put_u32(out + 24u, in.slot_capacity[1]);
    put_u32(out + 28u, in.image_limit);
}

inline bool decode_info(const uint8_t* in, Size len, Info& out) noexcept {
    if (len < kInfoSize) return false;
    out.proto_version  = get_u16(in + 0u);
    out.iface          = in[2];
    out.caps           = in[3];
    out.max_payload    = get_u16(in + 4u);
    out.page_size      = get_u16(in + 6u);
    out.fw_version     = get_u32(in + 8u);
    out.slot_offset[0] = get_u32(in + 12u);
    out.slot_offset[1] = get_u32(in + 16u);
    out.slot_capacity[0] = get_u32(in + 20u);
    out.slot_capacity[1] = get_u32(in + 24u);
    out.image_limit    = get_u32(in + 28u);
    return true;
}

// ─────────────────────────────── START ─────────────────────────────────────

/// Флаги START (payload).
inline constexpr uint8_t kStartResume    = 1u << 0;   ///< продолжить прерванную сессию
inline constexpr uint8_t kStartOverwrite = 1u << 1;   ///< перезаписать слот, даже если он занят
/// Зарезервирован: «только открыть сессию, без записи». НЕ реализован —
/// приёмник отвергает START с этим флагом (Unsupported), а не открывает
/// обычную сессию: «проверить без записи» и «стереть слот и писать» — разные
/// вещи, и подменять одно другим нельзя.
inline constexpr uint8_t kStartVerifyOnly= 1u << 2;

/// Слот «выбери сам» (например тот, что не запущен).
inline constexpr uint16_t kSlotAuto = 0xFFFFu;

/// START: 54 байта.
struct Start {
    uint32_t image_size    = 0u;
    uint32_t image_crc32   = 0u;
    uint32_t fw_version    = 0u;
    uint16_t slot          = kSlotAuto;
    uint16_t chunk_max     = kMaxPayload;   ///< предел DATA, который просит хост
    uint8_t  hash_kind     = static_cast<uint8_t>(HashKind::None);
    uint8_t  flags         = 0u;
    uint32_t resume_offset = 0u;
    uint8_t  sha256[32]{};
};

inline constexpr Size kStartSize = 4u + 4u + 4u + 2u + 2u + 1u + 1u + 4u + 32u;   // 54
static_assert(kStartSize <= kMaxPayload, "START не влезает в кадр");

inline void encode_start(uint8_t* out, const Start& in) noexcept {
    put_u32(out + 0u, in.image_size);
    put_u32(out + 4u, in.image_crc32);
    put_u32(out + 8u, in.fw_version);
    put_u16(out + 12u, in.slot);
    put_u16(out + 14u, in.chunk_max);
    out[16] = in.hash_kind;
    out[17] = in.flags;
    put_u32(out + 18u, in.resume_offset);
    for (Size i = 0u; i < 32u; ++i) out[22u + i] = in.sha256[i];
}

inline bool decode_start(const uint8_t* in, Size len, Start& out) noexcept {
    if (len < kStartSize) return false;
    out.image_size    = get_u32(in + 0u);
    out.image_crc32   = get_u32(in + 4u);
    out.fw_version    = get_u32(in + 8u);
    out.slot          = get_u16(in + 12u);
    out.chunk_max     = get_u16(in + 14u);
    out.hash_kind     = in[16];
    out.flags         = in[17];
    out.resume_offset = get_u32(in + 18u);
    for (Size i = 0u; i < 32u; ++i) out.sha256[i] = in[22u + i];
    return true;
}

// ──────────────────────────────── DATA ─────────────────────────────────────

/// DATA: 4 байта заголовка блока (offset) + данные.
///
/// ВАЖНО ПРО ПРЕДЕЛЫ (здесь легко ошибиться на 4 байта)
/// INFO.max_payload и Response.max_payload — предел ПОЛЕЗНОЙ НАГРУЗКИ КАДРА,
/// а не блока данных. Полезная нагрузка DATA состоит из 4 байт смещения и
/// самого блока, поэтому предел БЛОКА = max_payload - 4 (при max_payload = 128
/// это 124 байта). Блок больше предела устройство отвергает с кодом TooLarge
/// и сообщает в detail фактический предел блока.
inline constexpr Size kDataHeaderSize = 4u;

/// Предел блока данных для такого предела кадра.
inline constexpr Size data_block_limit(uint16_t max_payload) noexcept {
    return (max_payload > kDataHeaderSize) ? (max_payload - kDataHeaderSize) : 0u;
}

// ──────────────────────────────── END ──────────────────────────────────────

/// END: 40 байт. Хеш присутствует, только если в START заявлен Sha256;
/// иначе поле игнорируется приёмником (и это видно по START-кадру хоста).
struct End {
    uint32_t image_size  = 0u;
    uint32_t image_crc32 = 0u;
    uint8_t  sha256[32]{};
};

inline constexpr Size kEndSize = 4u + 4u + 32u;   // 40
static_assert(kEndSize <= kMaxPayload, "END не влезает в кадр");

inline void encode_end(uint8_t* out, const End& in) noexcept {
    put_u32(out + 0u, in.image_size);
    put_u32(out + 4u, in.image_crc32);
    for (Size i = 0u; i < 32u; ++i) out[8u + i] = in.sha256[i];
}

inline bool decode_end(const uint8_t* in, Size len, End& out) noexcept {
    if (len < kEndSize) return false;
    out.image_size  = get_u32(in + 0u);
    out.image_crc32 = get_u32(in + 4u);
    for (Size i = 0u; i < 32u; ++i) out.sha256[i] = in[8u + i];
    return true;
}

// ──────────────────────────────── SIGN ─────────────────────────────────────

/// SIGN: 6 байт заголовка (общая длина подписи, смещение фрагмента, вид) +
/// байты подписи.
///
/// Длинная подпись (RSA-2048 = 256 Б) не влезает в один кадр при
/// max_payload = 128, поэтому фрагментируется: kFlagMore у всех фрагментов,
/// кроме последнего. Фрагменты обязаны идти подряд и без пропусков — ответ
/// несёт next_offset, равный числу принятых байт подписи.
struct Sign {
    uint16_t total_len = 0u;
    uint16_t offset    = 0u;
    uint8_t  kind      = 0u;   ///< алгоритм подписи (нумерация приложения)
    uint8_t  reserved  = 0u;
};

inline constexpr Size kSignHeaderSize = 6u;

inline void encode_sign_header(uint8_t* out, const Sign& in) noexcept {
    put_u16(out + 0u, in.total_len);
    put_u16(out + 2u, in.offset);
    out[4] = in.kind;
    out[5] = in.reserved;
}

inline bool decode_sign_header(const uint8_t* in, Size len, Sign& out) noexcept {
    if (len < kSignHeaderSize) return false;
    out.total_len = get_u16(in + 0u);
    out.offset    = get_u16(in + 2u);
    out.kind      = in[4];
    out.reserved  = in[5];
    return true;
}

// ──────────────────────────── ERASE / READ / ABORT ─────────────────────────

/// ERASE: какой слот стереть.
inline constexpr Size kEraseSize = 2u;
inline void encode_erase(uint8_t* out, uint16_t slot) noexcept { put_u16(out, slot); }
inline bool decode_erase(const uint8_t* in, Size len, uint16_t& slot) noexcept {
    if (len < kEraseSize) return false;
    slot = get_u16(in);
    return true;
}

/// READ: что прочитать из слота. Ответ — Response, за которым идут данные.
struct Read {
    uint32_t offset = 0u;
    uint16_t len    = 0u;
};
inline constexpr Size kReadSize = 6u;
inline void encode_read(uint8_t* out, const Read& in) noexcept {
    put_u32(out + 0u, in.offset);
    put_u16(out + 4u, in.len);
}
inline bool decode_read(const uint8_t* in, Size len, Read& out) noexcept {
    if (len < kReadSize) return false;
    out.offset = get_u32(in + 0u);
    out.len    = get_u16(in + 4u);
    return true;
}

/// ABORT: причина (0 — «просто отмена») и необязательный текст.
inline constexpr Size kAbortSize = 1u;

// ─────────────────────────────── VERIFY ────────────────────────────────────

/// VERIFY: проверка образа, УЖЕ лежащего в слоте (например его записал другой
/// канал — программатор, SPI-хост, второй загрузчик). 44 байта.
struct Verify {
    uint32_t image_size  = 0u;
    uint32_t image_crc32 = 0u;
    uint16_t slot        = kSlotAuto;
    uint8_t  hash_kind   = static_cast<uint8_t>(HashKind::None);
    uint8_t  flags       = 0u;
    uint8_t  sha256[32]{};
};

inline constexpr Size kVerifySize = 4u + 4u + 2u + 1u + 1u + 32u;   // 44
static_assert(kVerifySize <= kMaxPayload, "VERIFY не влезает в кадр");

inline void encode_verify(uint8_t* out, const Verify& in) noexcept {
    put_u32(out + 0u, in.image_size);
    put_u32(out + 4u, in.image_crc32);
    put_u16(out + 8u, in.slot);
    out[10] = in.hash_kind;
    out[11] = in.flags;
    for (Size i = 0u; i < 32u; ++i) out[12u + i] = in.sha256[i];
}

inline bool decode_verify(const uint8_t* in, Size len, Verify& out) noexcept {
    if (len < kVerifySize) return false;
    out.image_size  = get_u32(in + 0u);
    out.image_crc32 = get_u32(in + 4u);
    out.slot        = get_u16(in + 8u);
    out.hash_kind   = in[10];
    out.flags       = in[11];
    for (Size i = 0u; i < 32u; ++i) out.sha256[i] = in[12u + i];
    return true;
}

// ─────────────────────────────── ОТВЕТ ─────────────────────────────────────
//
// Один формат на ACK и NAK: у хоста ровно один разбор ответа, а «что именно
// пошло не так» читается из code. Поля, не относящиеся к операции, нулевые.

struct Response {
    uint8_t  code          = static_cast<uint8_t>(Code::Ok);
    uint8_t  state         = static_cast<uint8_t>(State::Idle);
    uint16_t slot          = 0u;
    uint16_t max_payload   = kMaxPayload;  ///< какой предел DATA примет устройство
    uint16_t detail        = 0u;           ///< уточнение кода (для логов/хоста)
    uint32_t next_offset   = 0u;           ///< непрерывно принято и ЗАПИСАНО байт
    uint32_t image_crc32   = 0u;           ///< CRC образа, посчитанная устройством
    uint32_t fw_version    = 0u;           ///< версия образа в слоте
    uint32_t slot_offset   = 0u;           ///< где слот лежит в хранилище
    uint32_t slot_capacity = 0u;           ///< ёмкость слота
};

inline constexpr Size kResponseSize = 1u + 1u + 2u + 2u + 2u + 4u + 4u + 4u + 4u + 4u;  // 28
static_assert(kResponseSize <= kMaxPayload, "Response не влезает в кадр");

inline void encode_response(uint8_t* out, const Response& in) noexcept {
    out[0] = in.code;
    out[1] = in.state;
    put_u16(out + 2u, in.slot);
    put_u16(out + 4u, in.max_payload);
    put_u16(out + 6u, in.detail);
    put_u32(out + 8u, in.next_offset);
    put_u32(out + 12u, in.image_crc32);
    put_u32(out + 16u, in.fw_version);
    put_u32(out + 20u, in.slot_offset);
    put_u32(out + 24u, in.slot_capacity);
}

inline bool decode_response(const uint8_t* in, Size len, Response& out) noexcept {
    if (len < kResponseSize) return false;
    out.code          = in[0];
    out.state         = in[1];
    out.slot          = get_u16(in + 2u);
    out.max_payload   = get_u16(in + 4u);
    out.detail        = get_u16(in + 6u);
    out.next_offset   = get_u32(in + 8u);
    out.image_crc32   = get_u32(in + 12u);
    out.fw_version    = get_u32(in + 16u);
    out.slot_offset   = get_u32(in + 20u);
    out.slot_capacity = get_u32(in + 24u);
    return true;
}

// ──────────────────────────────── Кадр ─────────────────────────────────────

/// Разобранный кадр. payload указывает ВНУТРЬ буфера декодера — он живёт до
/// следующего вызова feed()/resume(), поэтому копировать его «на потом» нельзя.
struct Frame {
    uint8_t        version   = 0u;
    Op             op        = Op::Info;
    uint8_t        flags     = 0u;
    uint8_t        seq       = 0u;
    uint16_t       len       = 0u;
    const uint8_t* payload   = nullptr;
    /// Кадр целый (CRC сошёлся), но payload больше буфера приёмника: данные не
    /// сохранены, отвечать надо TooLarge. Такой кадр всё равно «съеден»
    /// целиком, поэтому синхронизация не теряется.
    bool           too_large = false;

    bool has(uint8_t flag) const noexcept { return (flags & flag) != 0u; }
};

/// Размер кадра с такой полезной нагрузкой.
inline constexpr Size frame_size(Size payload_len) noexcept {
    return kFrameOverhead + payload_len;
}

/// Сколько байт кадра покрывает CRC32: от VER (смещение 2) до последнего
/// байта payload включительно.
///
/// Отдельная функция, а не «8 + LEN» на месте: граница посчитана по
/// смещениям 2..(8+LEN) ВКЛЮЧИТЕЛЬНО, то есть LEN + 7 байт. Ошибка здесь на
/// единицу даёт кадры, которые не принимает никто, включая сам отправитель.
inline constexpr Size crc_cover_len(Size payload_len) noexcept {
    return (kHeaderInFrame - 2u) + payload_len;
}

/// Сколько байт нужно декодеру под кадр с таким пределом payload.
inline constexpr Size decoder_buffer_size(uint16_t max_payload) noexcept {
    return kFrameOverhead + max_payload;
}

/// Зарезервировать кадр: записать SOF/заголовок/HCRC и вернуть полный размер
/// (или 0, если не влез). Полезная нагрузка пишется вызывающим ПОСЛЕ этого —
/// ровно так строятся ответы, которым данные приходят из носителя: их кладут
/// сразу в буфер кадра, чтобы не держать вторую копию на стеке (стек задачи в
/// прошивке измеряется десятками слов, и лишние 100 байт на кадр — заметная
/// потеря).
inline Size frame_reserve(uint8_t* out, Size cap, Op op, uint8_t flags, uint8_t seq,
                          Size payload_len) noexcept {
    if (out == nullptr) return 0u;
    if (payload_len > kMaxPayload) return 0u;

    const Size total = frame_size(payload_len);
    if (total > cap) return 0u;

    out[0] = kSof0;
    out[1] = kSof1;
    out[2] = kVersion;
    out[3] = static_cast<uint8_t>(op);
    out[4] = flags;
    out[5] = seq;
    put_u16(out + 6u, static_cast<uint16_t>(payload_len));
    out[8] = header_crc(out);
    return total;
}

/// Завершить кадр: посчитать CRC32 по байтам 2..(8+LEN) и записать его.
inline void frame_seal(uint8_t* out, Size payload_len) noexcept {
    if (out == nullptr) return;
    const uint32_t crc = image_crc(out + 2u, crc_cover_len(payload_len));
    put_u32(out + kHeaderInFrame + payload_len, crc);
}

/// Собрать кадр. Возвращает длину кадра в байтах либо 0, если кадр не влез в
/// cap или длина payload превышает kMaxPayload.
///
/// Функция одна на обе стороны: устройство шлёт ею ответы, хост-отправитель —
/// запросы. Это гарантирует, что «наш» отправитель и приёмник не разойдутся.
inline Size encode(uint8_t* out, Size cap, Op op, uint8_t flags, uint8_t seq,
                   const void* payload, Size payload_len) noexcept {
    const Size total = frame_reserve(out, cap, op, flags, seq, payload_len);
    if (total == 0u) return 0u;

    if (payload_len > 0u && payload != nullptr) {
        const auto* src = static_cast<const uint8_t*>(payload);
        for (Size i = 0u; i < payload_len; ++i) out[kHeaderInFrame + i] = src[i];
    }
    frame_seal(out, payload_len);
    return total;
}

// ─────────────────────────────── Декодер ───────────────────────────────────
//
// Потоковый автомат: байты приходят кусками любого размера (UART отдаёт то,
// что успел принять DMA, тест — целый кадр), а на выходе получаются целые
// кадры. Память фиксирована: один буфер под максимальный кадр.
//
// КАК УСТРОЕНА СИНХРОНИЗАЦИЯ (главное свойство приёмника)
// Буфер хранит байты потока, а курсор интерпретирует их как кандидата в кадр,
// начиная с позиции start_. Если кандидат разрушен (не сошёлся HCRC/CRC32),
// начало кандидата сдвигается на ОДИН байт вперёд, и те же самые байты
// буфера интерпретируются заново. Поэтому:
//   * кадр, начинающийся внутри чужой полезной нагрузки, будет найден;
//   * одна ошибка в линии стоит одного потерянного кадра, а не всего сеанса
//     связи (приёмник, который «ждёт остаток» испорченного LEN, теряет связь
//     до перезапуска).
//
// ПОЧЕМУ КАДР БОЛЬШЕ БУФЕРА НЕ ЛОМАЕТ ПРИЁМ
// Хост узнаёт предел payload из INFO, но приёмник обязан быть устойчив и к
// испорченному/чужому потоку. Кадр с len > max_payload читается «на проходе»:
// байты не сохраняются, CRC32 считается потоково, и по нему кадр либо
// признаётся целым (тогда отвечаем TooLarge, зная op/seq), либо отбрасывается
// как испорченный. Буфер при этом не переполняется ни при какой длине.

class Decoder {
public:
    enum class Fault : uint8_t {
        None       = 0u,
        HeaderCrc  = 1u,   ///< не сошёлся HCRC (заголовок испорчен)
        FrameCrc   = 2u,   ///< не сошёлся CRC32 (кадр испорчен)
        BadVersion = 3u,   ///< другая версия протокола
        TooLarge   = 4u,   ///< payload больше буфера приёмника (проглочен)
        Noise      = 5u,   ///< поток мусора: сброс буфера, чтобы не тратить CPU
        BadLength  = 6u,   ///< длина неправдоподобна: считаем заголовок битым
    };

    /// Сколько раз за один проход разрешено «переиграть» кандидата.
    ///
    /// Нужно для оценки худшего времени: повторный разбор без буферизации
    /// ограничен размером буфера, но сплошной мусор заставлял бы перебирать
    /// все начала подряд. После предела буфер сбрасывается — кадр, который
    /// в нём начинался, всё равно был испорчен.
    static constexpr uint32_t kMaxRestarts = 32u;

    Decoder() noexcept = default;
    Decoder(const Decoder&)            = delete;
    Decoder& operator=(const Decoder&) = delete;

    /// Задать буфер под кадр и предел полезной нагрузки.
    ///
    /// Буфер обязан вмещать decoder_buffer_size(max_payload) байт; если он
    /// меньше, предел payload урезается до того, что реально влезает, — лучше
    /// честно уменьшить окно DATA (хост узнает новый предел из ответа), чем
    /// принимать кадры, которые некуда положить.
    void init(uint8_t* buffer, Size capacity, uint16_t max_payload) noexcept {
        buf_ = buffer;
        cap_ = capacity;
        fault_count_ = 0u;
        skipped_ = 0u;

        if (decoder_buffer_size(max_payload) > cap_) {
            max_payload_ = (cap_ < kMinFrame)
                         ? 0u
                         : static_cast<uint16_t>(cap_ - kFrameOverhead);
        } else {
            max_payload_ = max_payload;
        }
        reset();
    }

    void reset() noexcept {
        phase_ = Phase::Sof0;
        start_ = 0u;
        pos_   = 0u;
        have_  = 0u;
        drop_  = 0u;
        len_   = 0u;
        crc_   = image_crc_begin();
        fault_ = Fault::None;
        skipped_ = 0u;
        last_too_large_ = false;
        skip_index_ = 0u;
        too_large_crc_ = 0u;
        crc_bytes_ = 0u;
        crc_index_ = 0u;
    }

    /// Предел payload, который приёмник реально может сохранить.
    uint16_t max_payload() const noexcept { return max_payload_; }

    /// Последняя ошибка разбора (диагностика).
    Fault last_fault() const noexcept { return fault_; }
    /// Сколько раз разбор закончился ошибкой (кадр отброшен). Счётчик, а не
    /// флаг: сервису нужно знать, СКОЛЬКО кадров потеряно, даже если ошибка
    /// одна и та же.
    uint32_t fault_count() const noexcept { return fault_count_; }
    /// Сколько байт отброшено при восстановлении синхронизации.
    uint32_t skipped_bytes() const noexcept { return skipped_; }
    /// Предыдущий кадр был целым, но не влез (на него нужно ответить TooLarge).
    bool last_too_large() const noexcept { return last_too_large_; }

    /// Результат разбора очередного куска потока.
    struct Step {
        Size consumed = 0u;    ///< сколько байт из переданных обработано
        bool ready    = false; ///< кадр собран (или распознан как слишком большой)
    };

    /// Разобрать кусок потока. За один вызов выдаётся не более ОДНОГО кадра:
    /// вызывающий повторяет с остатком (data + consumed), пока не кончится
    /// вход, — так два кадра в одном буфере не теряются.
    ///
    /// ВАЖНО: frame.payload указывает в буфер декодера и живёт до следующего
    /// вызова feed()/resume(). Скопировать «на потом» его нельзя.
    Step feed(const uint8_t* data, Size len, Frame& out) noexcept {
        Step step{};
        if (buf_ == nullptr || max_payload_ == 0u || data == nullptr) return step;

        drop_emitted();

        while (step.consumed < len) {
            if (have_ >= cap_) make_room();
            if (have_ >= cap_) {
                // Места нет даже после сдвига. Штатно недостижимо: длина кадра
                // проверяется до чтения payload, поэтому кандидат всегда
                // помещается в буфер. Но зависнуть приёмник не должен.
                reset();
                fault_ = Fault::Noise;
                ++fault_count_;
                step.consumed = len;      // остаток потока пропускаем
                return step;
            }

            buf_[have_++] = data[step.consumed++];
            if (run(out)) {
                step.ready = true;
                return step;
            }
        }
        return step;
    }

    /// Доразобрать то, что уже лежит в буфере, без нового ввода.
    /// Возврат true — кадр готов.
    ///
    /// Нужно приёмнику, когда несколько кадров пришли одним куском: после
    /// выдачи первого остаток буфера разбирается этим вызовом.
    bool resume(Frame& out) noexcept {
        if (buf_ == nullptr || max_payload_ == 0u) return false;
        drop_emitted();
        return run(out);
    }

private:
    enum class Phase : uint8_t { Sof0, Sof1, Header, Payload, Crc, Skip };

    /// Интерпретировать буфер с курсора. Возврат true — кадр готов.
    bool run(Frame& out) noexcept {
        uint32_t restarts = 0u;

        for (;;) {
            switch (phase_) {
                case Phase::Sof0:
                    if (pos_ >= have_) return false;
                    if (buf_[pos_] == kSof0) {
                        start_ = pos_;
                        phase_ = Phase::Sof1;
                        crc_   = image_crc_begin();
                    } else {
                        ++skipped_;
                    }
                    ++pos_;
                    break;

                case Phase::Sof1:
                    if (pos_ >= have_) return false;
                    if (buf_[pos_] == kSof1) {
                        phase_ = Phase::Header;
                        crc_   = image_crc_begin();
                        ++pos_;
                    } else if (++restarts > kMaxRestarts) {
                        return give_up();
                    } else {
                        // Кандидат разрушен: байты всё ещё в буфере, поэтому
                        // пробуем начать кадр со СЛЕДУЮЩЕГО байта.
                        rewind_candidate();
                    }
                    break;

                case Phase::Header: {
                    // 9 байт заголовка от start_. CRC32 кадра покрывает
                    // VER..последний байт payload, то есть ВСЁ, начиная со
                    // смещения 2: и поля заголовка, и HCRC, и полезную
                    // нагрузку. (HCRC — отдельная, более слабая проверка
                    // заголовка ДО доверия к полю LEN, она не отменяет
                    // общего CRC32.)
                    while (pos_ < have_ && (pos_ - start_) < kHeaderInFrame) {
                        const Size idx = pos_ - start_;
                        if (idx >= 2u) {
                            crc_ = image_crc_update(crc_, buf_ + pos_, 1u);
                        }
                        ++pos_;
                    }
                    if ((pos_ - start_) < kHeaderInFrame) return false;

                    if (buf_[start_ + 8u] != header_crc(buf_ + start_)) {
                        fault_ = Fault::HeaderCrc;
                        ++fault_count_;
                        if (++restarts > kMaxRestarts) return give_up();
                        rewind_candidate();
                        break;
                    }
                    if (buf_[start_ + 2u] != kVersion) {
                        fault_ = Fault::BadVersion;
                        ++fault_count_;
                        if (++restarts > kMaxRestarts) return give_up();
                        rewind_candidate();
                        break;
                    }

                    // Поля заголовка запоминаем СРАЗУ: фаза Skip сбрасывает
                    // start_, и перечитывать их из буфера позже значило бы
                    // брать чужие байты.
                    hdr_ver_   = buf_[start_ + 2u];
                    hdr_op_    = static_cast<Op>(buf_[start_ + 3u]);
                    hdr_flags_ = buf_[start_ + 4u];
                    hdr_seq_   = buf_[start_ + 5u];

                    len_ = get_u16(buf_ + start_ + 6u);
                    if (len_ > max_payload_) {
                        const uint16_t limit = (max_payload_ > kMaxSkipLen)
                                             ? max_payload_ : kMaxSkipLen;
                        if (len_ > limit) {
                            // Длина неправдоподобна: это не «большой кадр», а
                            // испорченный заголовок. Проглотить 30 КБ значило
                            // бы потерять и все следующие кадры.
                            fault_ = Fault::BadLength;
                            ++fault_count_;
                            if (++restarts > kMaxRestarts) return give_up();
                            rewind_candidate();
                            break;
                        }
                        // Кадр целый по формату, но не влезает: читаем «на
                        // проходе» и ответим TooLarge, зная op и seq.
                        phase_ = Phase::Skip;
                        skip_index_ = 0u;
                        too_large_crc_ = 0u;
                        break;
                    }
                    phase_ = Phase::Payload;
                    break;
                }

                case Phase::Payload: {
                    const Size frame_end = start_ + kHeaderInFrame + len_;
                    while (pos_ < frame_end && pos_ < have_) {
                        crc_ = image_crc_update(crc_, buf_ + pos_, 1u);
                        ++pos_;
                    }
                    if (pos_ < frame_end) return false;
                    phase_ = Phase::Crc;
                    crc_bytes_ = 0u;
                    crc_index_ = 0u;
                    break;
                }

                case Phase::Crc: {
                    while (pos_ < have_ && crc_index_ < kCrcInFrame) {
                        crc_bytes_ |= static_cast<uint32_t>(buf_[pos_])
                                      << (8u * crc_index_);
                        ++crc_index_;
                        ++pos_;
                    }
                    if (crc_index_ < kCrcInFrame) return false;

                    if (crc_bytes_ != image_crc_finish(crc_)) {
                        fault_ = Fault::FrameCrc;
                        ++fault_count_;
                        if (++restarts > kMaxRestarts) return give_up();
                        rewind_candidate();
                        break;
                    }
                    fault_ = Fault::None;
                    last_too_large_ = false;
                    fill_frame(out, start_);
                    // Кадр отдаём сейчас, а байты освобождаем перед следующим
                    // разбором: payload указывает внутрь буфера.
                    drop_  = pos_;
                    phase_ = Phase::Sof0;
                    start_ = pos_;
                    return true;
                }

                case Phase::Skip: {
                    // payload + CRC32 идут мимо буфера: CRC считается потоково.
                    const Size total = static_cast<Size>(len_) + kCrcInFrame;
                    while (pos_ < have_ && skip_index_ < total) {
                        if (skip_index_ < len_) {
                            crc_ = image_crc_update(crc_, buf_ + pos_, 1u);
                        } else {
                            too_large_crc_ |= static_cast<uint32_t>(buf_[pos_])
                                              << (8u * (skip_index_ - len_));
                        }
                        ++skip_index_;
                        ++pos_;
                    }
                    if (skip_index_ < total) {
                        // Байты кадра больше не нужны — освобождаем буфер.
                        have_ = 0u;
                        pos_  = 0u;
                        start_ = 0u;
                        return false;
                    }
                    if (too_large_crc_ != image_crc_finish(crc_)) {
                        fault_ = Fault::FrameCrc;
                        ++fault_count_;
                        if (++restarts > kMaxRestarts) return give_up();
                        rewind_candidate();
                        break;
                    }
                    // Кадр целый, но не влез: сообщаем op/seq, payload пуст.
                    fault_ = Fault::TooLarge;
                    last_too_large_ = true;
                    out = Frame{};
                    out.version = hdr_ver_;
                    out.op      = hdr_op_;
                    out.flags   = hdr_flags_;
                    out.seq     = hdr_seq_;
                    out.len     = len_;
                    out.payload = nullptr;
                    out.too_large = true;
                    drop_  = pos_;
                    phase_ = Phase::Sof0;
                    start_ = pos_;
                    return true;
                }
            }

            if (pos_ < have_) continue;   // в буфере есть что разбирать
            return false;
        }
    }

    /// Начать кандидата со следующего байта буфера.
    ///
    /// Прогресс гарантирован: start_ строго растёт, а ветка Sof0 всегда
    /// сдвигает курсор, поэтому бесконечного цикла не возникнет.
    void rewind_candidate() noexcept {
        start_ = (start_ + 1u <= have_) ? (start_ + 1u) : have_;
        pos_   = start_;
        phase_ = Phase::Sof0;
        crc_   = image_crc_begin();
    }

    /// Сплошной мусор: буфер сбрасывается, приём продолжается с чистого листа.
    bool give_up() noexcept {
        fault_ = Fault::Noise;
        ++fault_count_;
        have_ = 0u;
        pos_  = 0u;
        start_ = 0u;
        phase_ = Phase::Sof0;
        return false;
    }

    /// Освободить буфер после отданного кадра.
    ///
    /// Отдельным шагом, а не сразу: payload кадра указывает в буфер, и
    /// вызывающий должен успеть его обработать.
    void drop_emitted() noexcept {
        if (drop_ == 0u) return;
        const Size rest = have_ - drop_;
        for (Size i = 0u; i < rest; ++i) buf_[i] = buf_[drop_ + i];
        have_ = rest;
        pos_  = 0u;
        start_ = 0u;
        drop_ = 0u;
    }

    /// Место под следующий байт: выбросить израсходованный префикс.
    void make_room() noexcept {
        if (start_ == 0u) return;          // сдвигать нечего
        const Size rest = have_ - start_;
        for (Size i = 0u; i < rest; ++i) buf_[i] = buf_[start_ + i];
        have_ = rest;
        pos_  = (pos_ > start_) ? (pos_ - start_) : 0u;
        start_ = 0u;
        drop_  = 0u;
    }

    void fill_frame(Frame& out, Size start) noexcept {
        out = Frame{};
        out.version = hdr_ver_;
        out.op      = hdr_op_;
        out.flags   = hdr_flags_;
        out.seq     = hdr_seq_;
        out.len     = len_;
        out.payload = buf_ + start + kHeaderInFrame;
        out.too_large = false;
    }

    uint8_t* buf_ = nullptr;
    Size     cap_ = 0u;
    uint16_t max_payload_ = 0u;

    Phase    phase_ = Phase::Sof0;
    Size     start_ = 0u;   ///< начало текущего кандидата
    Size     pos_   = 0u;   ///< курсор разбора
    Size     have_  = 0u;   ///< сколько байт лежит в буфере
    Size     drop_  = 0u;   ///< сколько байт освободить после отданного кадра
    uint16_t len_   = 0u;   ///< длина payload текущего кандидата
    uint32_t crc_   = 0u;   ///< «сырое» состояние CRC32 текущего кандидата

    /// Поля заголовка текущего кандидата (см. комментарий в фазе Header).
    uint8_t  hdr_ver_   = 0u;
    Op       hdr_op_    = Op::Info;
    uint8_t  hdr_flags_ = 0u;
    uint8_t  hdr_seq_   = 0u;

    Size     skip_index_ = 0u;   ///< сколько байт большого кадра уже съедено
    uint32_t too_large_crc_ = 0u;
    uint32_t crc_bytes_ = 0u;
    uint8_t  crc_index_ = 0u;

    Fault    fault_ = Fault::None;
    uint32_t fault_count_ = 0u;
    uint32_t skipped_ = 0u;
    bool     last_too_large_ = false;
};

} // namespace embed::ota::proto
