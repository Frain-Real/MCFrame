// tests/test_ota.cpp
// ---------------------------------------------------------------------------
// OTA: протокол приёма, запись в носитель, проверка, активация и продолжение
// передачи после потери питания.
//
// ЧТО ЗДЕСЬ ГЛАВНОЕ
// Проверяется не «функция вернула Ok», а ПОВЕДЕНИЕ ПРИЁМНИКА в тех состояниях,
// из-за которых OTA обычно и ломается:
//   * кадр испорчен, потерян, пришёл дважды или не в том порядке;
//   * питание пропало посреди передачи — продолжать можно только с того, что
//     РЕАЛЬНО лежит в носителе (а не с того, что помнит хост);
//   * запись «прошла», но в носителе не то (проверка чтением);
//   * образ целый, но не тот (CRC), или подписан не тем ключом;
//   * слот проверен, но не активирован; активирован, но не подтверждён.
//
// РОЛИ
//   * хост-отправитель (Host) — реализация ДРУГОЙ стороны протокола: собирает
//     кадры тем же кодеком (proto::encode), разбирает ответы тем же декодером
//     и повторяет запросы. Так проверяется, что спецификация описывает обе
//     стороны, а не только приёмник;
//   * устройство — ota::Service поверх mock-платформы: носитель — модель
//     встроенной Flash (страница 1 КБ, как у STM32F103), интерфейс — либо
//     транспорт в памяти, либо mock-UART с его шиной.
// ---------------------------------------------------------------------------
#include "os/ota/ota.hpp"
#include "os/ota/ota_transport.hpp"

#include "os/config/time_config.hpp"
#include "os/crypto/sha2.hpp"
#include "os/power/power.hpp"
#include "os/watchdog/watchdog.hpp"

#include "flash_mock.hpp"     // модель Flash/внешней памяти mock-платформы
#include "platform_power_mock.hpp"
#include "platform_watchdog_mock.hpp"
#include "uart_mock.hpp"      // UART как «провод» между хостом и устройством

#include <cstdio>
#include <cstring>

using namespace embed;
using namespace embed::ota;

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

// ────────────────────────────── Раскладка Flash ─────────────────────────────
//
// Как на STM32F103RB: 128 КБ, страница 1 КБ. Метаданные — 4 страницы
// (по две копии на слот), дальше два слота по 28 КБ.
constexpr uint32_t kPage        = 1024u;
constexpr uint32_t kMetaOffset  = 0u;
constexpr uint32_t kSlotAOffset = 4u * kPage;                  // 4096
constexpr uint32_t kSlotASize   = 28u * kPage;                 // 28672
constexpr uint32_t kSlotBOffset = kSlotAOffset + kSlotASize;   // 32768
constexpr uint32_t kSlotBSize   = 28u * kPage;

// ─────────────────────────────── Тестовый образ ─────────────────────────────

constexpr uint32_t kImageMax = 16u * 1024u;
uint8_t g_image[kImageMax]{};

/// Псевдослучайный, но ВОСПРОИЗВОДИМЫЙ образ: тест не зависит от внешнего
/// файла, но проверяет CRC32/SHA-256 на «неудобных» данных.
void make_image(uint32_t size, uint8_t seed) {
    uint32_t x = 0x12345678u ^ (static_cast<uint32_t>(seed) << 24);
    for (uint32_t i = 0u; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        g_image[i] = static_cast<uint8_t>(x >> 24);
    }
}

uint32_t image_crc(uint32_t size) { return proto::image_crc(g_image, size); }

void image_sha(uint32_t size, uint8_t out[32]) {
    crypto::Sha2<crypto::Sha2Variant::Sha256> h;
    h.update(g_image, size);
    h.finalize(out);
}

// ───────────────────────────────── «Провод» ────────────────────────────────
//
// Хост и устройство соединены «проводом»: интерфейс может быть любым, и от
// него не должен зависеть ни один байт протокола. Проверяются два:
//   * MemoryWire — транспорт в памяти (быстрый и детерминированный);
//   * UartWire   — mock-UART с шиной: проверяется путь через драйвер
//     интерфейса и то, что кадр не «разваливается» на куски.

struct Wire {
    virtual ~Wire() = default;
    /// Хост -> устройство.
    virtual void send(const uint8_t* data, Size len) = 0;
    /// Ответы устройства -> хост (0, если пока нечего).
    virtual Size recv(uint8_t* buf, Size cap) = 0;
    /// Забыть всё, что устройство уже отправило (изоляция сцен).
    virtual void flush() = 0;
    /// Полный сброс провода: пусты и вход, и выход. Нужен между сценами:
    /// непрочитанные байты прошлой сцены сдвинули бы разбор следующей.
    virtual void reset() = 0;
    /// Интерфейс, который видит сервис OTA.
    virtual Transport& transport() = 0;
};

struct MemoryWire : Wire {
    MemoryTransport tr{};

    void send(const uint8_t* data, Size len) override { (void)tr.feed(data, len); }
    Size recv(uint8_t* buf, Size cap) override { return tr.take(buf, cap); }
    void flush() override {
        uint8_t tmp[MemoryTransport::kBuffer];
        while (tr.pending_out() > 0u) (void)tr.take(tmp, sizeof tmp);
    }
    void reset() override { tr.reset(); }
    Transport& transport() override { return tr; }
};

/// UART как провод: запросы кладутся в приёмник mock-UART, ответы читаются из
/// его передатчика. Ничего «протокольного» здесь нет — только байты.
struct UartWire : Wire {
    using Driver = mock::UartMockPolling;

    UartWire() {
        Driver::Config cfg{};
        cfg.baudRate = 115200u;
        (void)Driver::instance().configure(cfg);
        (void)Driver::instance().open();
        mock::g_uart_bus.reset();
    }

    void send(const uint8_t* data, Size len) override {
        mock::UartBus& bus = mock::g_uart_bus;
        // Приёмник mock-UART — линейный буфер без сдвига, поэтому освобождаем
        // его, когда всё прочитано: иначе долгая передача «упрётся» в 256 байт
        // и хвост кадров молча потеряется.
        if (bus.rx_read == bus.rx_len) {
            bus.rx_len = 0u;
            bus.rx_read = 0u;
        }
        bus.feed(data, len);
    }

    Size recv(uint8_t* buf, Size cap) override {
        mock::UartBus& bus = mock::g_uart_bus;
        const Size have = bus.tx_len - tx_read_;
        const Size n = (have < cap) ? have : cap;
        for (Size i = 0u; i < n; ++i) buf[i] = bus.tx[tx_read_ + i];
        tx_read_ += n;
        if (tx_read_ == bus.tx_len) {        // всё прочитано — освобождаем шину
            bus.tx_len = 0u;
            tx_read_ = 0u;
        }
        return n;
    }

    void flush() override {
        mock::g_uart_bus.tx_len = 0u;
        tx_read_ = 0u;
    }
    void reset() override {
        mock::g_uart_bus.reset();
        tx_read_ = 0u;
    }
    Transport& transport() override { return transport_; }

    StreamTransport<Driver> transport_{Driver::instance(), proto::Iface::Uart, 0u, "uart"};
    Size tx_read_ = 0u;
};

/// Что делает «устройство», пока хост ждёт ответ: прогоняет приём.
/// Через указатель, а не напрямую, — чтобы тест мог подменить поведение.
using TickFn = void (*)(void*);
TickFn g_tick = nullptr;

// «Провода» живут столько же, сколько тест: сервис OTA хранит на интерфейс
// указатель, поэтому объект интерфейса обязан переживать сервис (иначе
// shutdown() обратился бы к уничтоженному объекту — это и поймал ASan).
MemoryWire g_wire;
UartWire   g_uart_wire;

void device_tick(void* /*arg*/) { (void)Service::instance().poll(0u); }

// ─────────────────────── Хост-отправитель (другая сторона) ─────────────────

class Host {
public:
    explicit Host(Wire& wire) : wire_(&wire) {
        dec_.init(rx_, sizeof rx_, proto::kMaxPayload);
    }

    bool send(proto::Op op, uint8_t flags, const void* payload, Size len) {
        uint8_t buf[proto::kFrameOverhead + proto::kMaxPayload];
        const Size total = proto::encode(buf, sizeof buf, op, flags, seq_++, payload, len);
        if (total == 0u) return false;
        wire_->send(buf, total);
        return true;
    }

    bool request(proto::Op op, const void* payload, Size len, uint8_t flags = 0u) {
        if (!send(op, flags, payload, len)) return false;
        return wait_response();
    }

    /// Отправить кадр с ИСПОРЧЕННЫМ байтом: так выглядит ошибка в линии.
    bool send_corrupt(proto::Op op, const void* payload, Size len) {
        uint8_t buf[proto::kFrameOverhead + proto::kMaxPayload];
        const Size total = proto::encode(buf, sizeof buf, op, 0u, seq_++, payload, len);
        if (total == 0u) return false;
        buf[total - 5u] = static_cast<uint8_t>(buf[total - 5u] ^ 0xFFu);
        wire_->send(buf, total);
        return true;
    }

    /// Отправить кадр с полезной нагрузкой БОЛЬШЕ kMaxPayload.
    ///
    /// Штатный кодек такой кадр собрать отказывается, поэтому собираем вручную:
    /// приёмник обязан быть устойчив и к чужому/сломанному отправителю, а не
    /// только к своему.
    bool send_oversized(proto::Op op, const void* payload, Size len) {
        static uint8_t buf[1024];
        if (proto::frame_size(len) > sizeof buf) return false;
        if (len > 0xFFFFu) return false;

        buf[0] = proto::kSof0;
        buf[1] = proto::kSof1;
        buf[2] = proto::kVersion;
        buf[3] = static_cast<uint8_t>(op);
        buf[4] = 0u;
        buf[5] = seq_++;
        proto::put_u16(buf + 6u, static_cast<uint16_t>(len));
        buf[8] = proto::header_crc(buf);
        const auto* src = static_cast<const uint8_t*>(payload);
        for (Size i = 0u; i < len; ++i) buf[proto::kHeaderInFrame + i] = src[i];
        proto::put_u32(buf + proto::kHeaderInFrame + len,
                       proto::image_crc(buf + 2u, proto::crc_cover_len(len)));
        wire_->send(buf, proto::frame_size(len));
        return true;
    }

    /// Ждать ответ на СВОЙ запрос, прогоняя приём устройства.
    ///
    /// Посторонние NAK BAD_FRAME пропускаются: приёмник отвечает ими на
    /// испорченный кадр, то есть ВНЕ ОЧЕРЕДИ, и такой ответ не имеет
    /// отношения к текущему запросу (см. правила приёма в ota_protocol.hpp).
    /// Считать их «ответом» — значит терять настоящий ответ и повторять
    /// запрос на ровном месте.
    bool wait_response(uint32_t max_ticks = 400u) {
        resp_ = proto::Response{};
        for (uint32_t i = 0u; i < max_ticks; ++i) {
            if (drain()) {
                if (last_frame_.op == proto::Op::Nak &&
                    resp_.code == static_cast<uint8_t>(proto::Code::BadFrame)) {
                    ++stray_bad_frames_;
                    continue;
                }
                return true;
            }
            if (g_tick != nullptr) g_tick(nullptr);
        }
        return false;
    }

    /// Сколько посторонних NAK BAD_FRAME пришло (диагностика тестов).
    uint32_t stray_bad_frames() const { return stray_bad_frames_; }

    const proto::Response& response() const { return resp_; }
    const proto::Frame& last_frame() const { return last_frame_; }
    bool last_is_ack() const { return last_frame_.op == proto::Op::Ack; }
    bool last_is_nak() const { return last_frame_.op == proto::Op::Nak; }
    proto::Op last_op() const { return last_frame_.op; }
    proto::Info last_info() const { return last_info_; }
    const uint8_t* last_data() const { return last_data_; }
    Size last_data_len() const { return last_data_len_; }

    // ── Операции протокола ──

    bool info() {
        const uint8_t ver = proto::kVersion;
        if (!request(proto::Op::Info, &ver, 1u)) return false;
        if (last_frame_.op != proto::Op::Ack) return false;
        return proto::decode_info(last_frame_.payload, last_frame_.len, last_info_);
    }

    bool start(const proto::Start& s) {
        uint8_t buf[proto::kStartSize];
        proto::encode_start(buf, s);
        return request(proto::Op::Start, buf, proto::kStartSize);
    }

    bool data(uint32_t offset, const uint8_t* p, Size len) {
        uint8_t buf[proto::kDataHeaderSize + proto::kMaxPayload];
        proto::put_u32(buf, offset);
        for (Size i = 0u; i < len; ++i) buf[proto::kDataHeaderSize + i] = p[i];
        return request(proto::Op::Data, buf, proto::kDataHeaderSize + len);
    }

    bool end(uint32_t size, uint32_t crc, const uint8_t* sha256) {
        proto::End e{};
        e.image_size = size;
        e.image_crc32 = crc;
        if (sha256 != nullptr) std::memcpy(e.sha256, sha256, 32u);
        uint8_t buf[proto::kEndSize];
        proto::encode_end(buf, e);
        return request(proto::Op::End, buf, proto::kEndSize);
    }

    bool sign(uint16_t total_len, uint16_t offset, uint8_t kind,
              const uint8_t* sig, Size len, bool more) {
        uint8_t buf[proto::kSignHeaderSize + proto::kMaxPayload];
        proto::Sign h{};
        h.total_len = total_len;
        h.offset = offset;
        h.kind = kind;
        proto::encode_sign_header(buf, h);
        for (Size i = 0u; i < len; ++i) buf[proto::kSignHeaderSize + i] = sig[i];
        return request(proto::Op::Sign, buf, proto::kSignHeaderSize + len,
                       more ? proto::kFlagMore : 0u);
    }

    bool activate(uint16_t slot) {
        uint8_t buf[proto::kEraseSize];
        proto::encode_erase(buf, slot);
        return request(proto::Op::Activate, buf, proto::kEraseSize);
    }

    bool erase(uint16_t slot) {
        uint8_t buf[proto::kEraseSize];
        proto::encode_erase(buf, slot);
        return request(proto::Op::Erase, buf, proto::kEraseSize);
    }

    bool confirm() { return request(proto::Op::Confirm, nullptr, 0u); }
    bool state()   { return request(proto::Op::State, nullptr, 0u); }
    bool abort(uint8_t reason) { return request(proto::Op::Abort, &reason, 1u); }

    bool verify(const proto::Verify& v) {
        uint8_t buf[proto::kVerifySize];
        proto::encode_verify(buf, v);
        return request(proto::Op::Verify, buf, proto::kVerifySize);
    }

    bool read(uint32_t offset, uint16_t len) {
        proto::Read r{};
        r.offset = offset;
        r.len = len;
        uint8_t buf[proto::kReadSize];
        proto::encode_read(buf, r);
        return request(proto::Op::Read, buf, proto::kReadSize);
    }

    /// Передать образ блоками. Возвращает смещение, до которого устройство
    /// ПОДТВЕРДИЛО приём (не то, что хост «отправил»).
    uint32_t send_image(uint32_t size, uint32_t chunk, uint32_t stop_after = 0u,
                        uint32_t from = 0u) {
        // Хост обязан уважать предел, который устройство объявило в INFO:
        // полезная нагрузка кадра DATA — это БЛОК ПЛЮС 4 байта смещения,
        // поэтому предел блока = предел кадра - 4. Блок больше предела — уже
        // не «передача», а нарушение протокола.
        const uint32_t max_block = static_cast<uint32_t>(proto::data_block_limit(proto::kMaxPayload));
        if (chunk == 0u || chunk > max_block) chunk = max_block;
        uint32_t off = from;
        while (off < size) {
            Size n = chunk;
            if (n > (size - off)) n = size - off;
            if (!data(off, g_image + off, n)) return off;
            if (last_frame_.op != proto::Op::Ack) return off;
            const uint32_t next = resp_.next_offset;
            if (next <= off && stop_after == 0u) return off;   // устройство стоит
            off = next;
            if (stop_after != 0u && off >= stop_after) break;
        }
        return off;
    }

private:
    /// Разобрать всё, что пришло от устройства.
    bool drain() {
        for (;;) {
            proto::Frame f{};
            if (dec_.resume(f)) return keep(f);

            uint8_t buf[proto::kMaxPayload + proto::kFrameOverhead + 8u];
            const Size got = wire_->recv(buf, sizeof buf);
            if (got == 0u) return false;

            Size pos = 0u;
            while (pos < got) {
                const proto::Decoder::Step st = dec_.feed(buf + pos, got - pos, f);
                pos += st.consumed;
                if (st.ready) return keep(f);
                if (st.consumed == 0u) break;
            }
        }
    }

    bool keep(const proto::Frame& f) {
        last_frame_ = f;
        last_data_ = nullptr;
        last_data_len_ = 0u;
        resp_ = proto::Response{};
        if (f.op == proto::Op::Ack || f.op == proto::Op::Nak) {
            (void)proto::decode_response(f.payload, f.len, resp_);
        }
        if (f.op == proto::Op::Ack && f.len > proto::kResponseSize) {
            last_data_ = f.payload + proto::kResponseSize;
            last_data_len_ = static_cast<Size>(f.len) - proto::kResponseSize;
        }
        return true;
    }

    Wire* wire_ = nullptr;
    uint8_t rx_[proto::kMaxPayload + proto::kFrameOverhead]{};
    proto::Decoder dec_{};
    proto::Frame last_frame_{};
    proto::Response resp_{};
    proto::Info last_info_{};
    uint8_t seq_ = 0u;
    const uint8_t* last_data_ = nullptr;
    Size last_data_len_ = 0u;
    uint32_t stray_bad_frames_ = 0u;
};

// ───────────────────────────── Настройка приёма ────────────────────────────

InternalFlashStorage g_storage;

Config make_config(Wire* wire, uint8_t active_slot = 0u, uint32_t fw_version = 1u) {
    Config cfg{};
    cfg.storage          = &g_storage;
    cfg.transport        = (wire != nullptr) ? &wire->transport() : nullptr;
    cfg.meta_offset      = kMetaOffset;
    cfg.slot_offset[0]   = kSlotAOffset;
    cfg.slot_capacity[0] = kSlotASize;
    cfg.slot_offset[1]   = kSlotBOffset;
    cfg.slot_capacity[1] = kSlotBSize;
    cfg.active_slot      = active_slot;
    cfg.fw_version       = fw_version;
    cfg.checkpoint_bytes = 256u;         // в тесте — чаще, чем на железе
    cfg.session_timeout_ms = 0u;         // таймаут сессии проверяется отдельно
    cfg.erase_stall_ms   = 500u;
    cfg.verify_readback  = true;
    cfg.verify_resume_prefix = true;
    cfg.keep_awake       = false;        // энергосбережение здесь не участвует
    cfg.use_watchdog_stall = false;
    return cfg;
}

/// Насколько смещение продолжения может отстать от отправленного: последние
/// блоки могли не попасть в контрольную точку (она сохраняется не после
/// каждого блока, иначе носитель изнашивался бы втрое быстрее).
namespace cfg { constexpr uint32_t kResumeSlack = 512u; }

Service& S() { return Service::instance(); }

/// Снимок счётчиков: статистика сервиса НАКОПИТЕЛЬНАЯ (она не сбрасывается
/// между сессиями — иначе «сколько всего обновлений пережило устройство»
/// пришлось бы считать снаружи). Поэтому сцены сравнивают приращения, а не
/// абсолютные значения: иначе проверка зависела бы от порядка сцен.
struct StatsBase {
    Stats base{};
    StatsBase() : base(S().stats()) {}
    uint32_t frames()        const { return S().stats().frames        - base.frames; }
    uint32_t bad_frames()    const { return S().stats().bad_frames    - base.bad_frames; }
    uint32_t resync_bytes()  const { return S().stats().resync_bytes  - base.resync_bytes; }
    uint32_t sessions()      const { return S().stats().sessions      - base.sessions; }
    uint32_t resumed()       const { return S().stats().resumed       - base.resumed; }
    uint32_t resume_rejected() const { return S().stats().resume_rejected - base.resume_rejected; }
    uint32_t expired()       const { return S().stats().expired       - base.expired; }
    uint32_t aborts()        const { return S().stats().aborts        - base.aborts; }
    uint32_t duplicates()    const { return S().stats().duplicates    - base.duplicates; }
    uint32_t gaps()          const { return S().stats().gaps          - base.gaps; }
    uint32_t bytes_written() const { return S().stats().bytes_written - base.bytes_written; }
    uint32_t page_erases()   const { return S().stats().page_erases   - base.page_erases; }
    uint32_t verify_ok()     const { return S().stats().verify_ok     - base.verify_ok; }
    uint32_t verify_fail()   const { return S().stats().verify_fail   - base.verify_fail; }
    uint32_t readback_fail() const { return S().stats().readback_fail - base.readback_fail; }
    uint32_t signature_ok()  const { return S().stats().signature_ok  - base.signature_ok; }
    uint32_t signature_fail()const { return S().stats().signature_fail- base.signature_fail; }
};
mock_platform::FlashModel& F() { return mock_platform::flash_model(); }

/// Полный сброс между сценами: «железо» чистое, провод пуст, RAM сервиса
/// забыта. Именно в таком порядке: сначала сервис перестаёт пользоваться
/// интерфейсом (shutdown), потом чистится «железо» и провод.
void reset_world(Wire& wire) {
    S().shutdown();
    wire.reset();
    F().reset();
}

/// «Перезагрузка устройства»: RAM забыта, носитель — нет.
void power_cycle() { S().shutdown(); }

// ─────────────────────────── 1. INFO и раскладка ───────────────────────────

void test_info_and_layout() {
    (void)std::puts("INFO и проверка раскладки:");

    MemoryWire& wire = g_wire;
    reset_world(wire);

    // «Куда писать» проверяется до всего остального: перекрытие слотов и
    // метаданных обнаружилось бы только посреди передачи — то есть тогда,
    // когда слот уже стёрт.
    Config bad = make_config(&wire);
    bad.slot_offset[1] = bad.slot_offset[0];       // слоты перекрываются
    check("перекрытие слотов -> InvalidArg", S().init(bad) == Status::InvalidArg);

    bad = make_config(&wire);
    bad.meta_offset = 7u;                          // не кратно странице
    check("метаданные не по странице -> InvalidArg", S().init(bad) == Status::InvalidArg);

    bad = make_config(&wire);
    bad.slot_capacity[0] = 0u;
    bad.slot_capacity[1] = 0u;
    check("нет ни одного слота -> InvalidArg", S().init(bad) == Status::InvalidArg);

    bad = make_config(&wire);
    bad.slot_offset[0] = kMetaOffset + kPage;      // слот залез на метаданные
    check("слот перекрывает метаданные -> InvalidArg", S().init(bad) == Status::InvalidArg);

    check("корректная раскладка -> Ok", S().init(make_config(&wire)) == Status::Ok);

    Host host(wire);
    check("INFO принят", host.info());
    check_eq<uint32_t>("INFO: версия протокола",
                       host.last_info().proto_version, proto::kVersion);
    check_eq<uint32_t>("INFO: интерфейс = memory",
                       host.last_info().iface, static_cast<uint8_t>(proto::Iface::Memory));
    check("INFO: два слота заявлены", (host.last_info().caps & proto::kCapDualSlot) != 0u);
    check_eq<uint32_t>("INFO: предел образа = ёмкость слота",
                       host.last_info().image_limit, kSlotASize);
    check_eq<uint32_t>("INFO: смещение слота A",
                       host.last_info().slot_offset[0], kSlotAOffset);
}

// ─────────────────────── 2. Полный цикл: приём и проверка ──────────────────

void test_happy_path() {
    (void)std::puts("полный цикл приёма (транспорт в памяти + внутренняя Flash):");
    const StatsBase st0;

    MemoryWire& wire = g_wire;
    reset_world(wire);
    check("init", S().init(make_config(&wire)) == Status::Ok);

    constexpr uint32_t kSize = 4096u;
    make_image(kSize, 1u);
    const uint32_t crc = image_crc(kSize);

    Host host(wire);
    check("INFO", host.info());

    proto::Start s{};
    s.image_size  = kSize;
    s.image_crc32 = crc;
    s.fw_version  = 2u;
    s.slot        = proto::kSlotAuto;
    s.chunk_max   = 128u;
    check("START", host.start(s));
    check("START -> ACK", host.last_is_ack());
    check_eq<uint32_t>("START: next_offset = 0", host.response().next_offset, 0u);
    check_eq<uint32_t>("START: выбран НЕ активный слот (B)", host.response().slot, 1u);

    const uint32_t sent = host.send_image(kSize, 100u);
    check_eq<uint32_t>("передано всё", sent, kSize);
    check_eq<uint32_t>("последний ответ: next_offset = размер",
                       host.response().next_offset, kSize);

    check("END", host.end(kSize, crc, nullptr));
    check("END -> ACK", host.last_is_ack());
    check_eq<uint32_t>("состояние VERIFIED",
                       host.response().state, static_cast<uint8_t>(proto::State::Verified));

    // Что реально лежит в носителе: сверяем БАЙТЫ, а не статусы.
    uint8_t back[64]{};
    check("чтение слота B из носителя",
          g_storage.read(kSlotBOffset, back, sizeof back) == Status::Ok);
    check("байты образа совпали", std::memcmp(back, g_image, sizeof back) == 0);

    SlotRecord rec{};
    check("запись состояния читается", S().slot_record(1u, rec) == Status::Ok);
    check_eq<uint32_t>("состояние в записи",
                       rec.state, static_cast<uint8_t>(proto::State::Verified));
    check("CRC подтверждён флагом", rec.crc_ok());
    check("проверка чтением выполнена", (rec.flags & kRecFlagReadbackOk) != 0u);

    check("стирались страницы", st0.page_erases() > 0u);
    check_eq<uint32_t>("проверка образа зачтена", st0.verify_ok(), 1u);
    check_eq<uint32_t>("сессия одна", st0.sessions(), 1u);
}

// ──────────────────── 3. Повторы, разрывы и мусор в линии ──────────────────

void test_repeats_and_gaps() {
    (void)std::puts("повтор, разрыв, мусор и переросший кадр:");
    const StatsBase st0;

    MemoryWire& wire = g_wire;
    reset_world(wire);
    check("init", S().init(make_config(&wire)) == Status::Ok);

    constexpr uint32_t kSize = 1024u;
    make_image(kSize, 2u);

    Host host(wire);
    (void)host.info();

    proto::Start s{};
    s.image_size = kSize;
    s.image_crc32 = image_crc(kSize);
    s.fw_version = 2u;
    check("START", host.start(s));

    check("DATA[0..99]", host.data(0u, g_image, 100u));
    check_eq<uint32_t>("next_offset = 100", host.response().next_offset, 100u);

    // ПОВТОР того же блока (ответ потерялся): данные не пишутся дважды.
    const uint32_t written_before = S().stats().bytes_written;

    check("повтор DATA[0..99]", host.data(0u, g_image, 100u));
    check("повтор -> ACK", host.last_is_ack());
    check_eq<uint32_t>("next_offset не изменился", host.response().next_offset, 100u);
    check_eq<uint32_t>("повторно не писали", S().stats().bytes_written, written_before);
    check_eq<uint32_t>("повтор учтён", st0.duplicates(), 1u);

    // РАЗРЫВ: блок 300, хотя ждём 100.
    check("DATA[300..399] (разрыв)", host.data(300u, g_image + 300u, 100u));
    check("разрыв -> NAK", host.last_is_nak());
    check_eq<uint32_t>("код BAD_OFFSET",
                       host.response().code, static_cast<uint8_t>(proto::Code::BadOffset));
    check_eq<uint32_t>("в ответе next_offset = 100", host.response().next_offset, 100u);
    check("разрыв учтён", st0.gaps() >= 1u);

    // МУСОР перед кадром: приёмник обязан найти кадр, а не «съехать».
    {
        const uint8_t junk[] = {0x00u, 0x4Fu, 0x00u, 0xFFu, 0x4Fu, 0x54u, 0x00u};
        wire.send(junk, sizeof junk);
    }
    check("DATA[100..199] после мусора", host.data(100u, g_image + 100u, 100u));
    check("кадр найден после мусора", host.last_is_ack());
    check_eq<uint32_t>("next_offset = 200", host.response().next_offset, 200u);
    check("учтены отброшенные байты", st0.resync_bytes() > 0u);

    // ИСПОРЧЕННЫЙ КАДР: портим байт до CRC.
    {
        uint8_t buf[proto::kDataHeaderSize + 32u];
        proto::put_u32(buf, 200u);
        for (Size i = 0u; i < 32u; ++i) buf[proto::kDataHeaderSize + i] = g_image[200u + i];
        const uint32_t bad_before = S().stats().bad_frames;
        const uint32_t stray_before = host.stray_bad_frames();
        check("отправлен испорченный кадр", host.send_corrupt(proto::Op::Data, buf, sizeof buf));
        (void)host.wait_response(50u);
        check("испорченный кадр учтён", S().stats().bad_frames > bad_before);
        check("отвечено NAK BAD_FRAME (вне очереди)",
              host.stray_bad_frames() > stray_before);
    }

    // После испорченного кадра приём продолжается штатно.
    check("DATA[200..299] после порчи", host.data(200u, g_image + 200u, 100u));
    check_eq<uint32_t>("next_offset = 300", host.response().next_offset, 300u);

    // ПЕРЕРОСШИЙ кадр: устройство обязано ответить пределом, который примет.
    {
        uint8_t buf[proto::kDataHeaderSize + proto::kMaxPayload + 16u];
        proto::put_u32(buf, 300u);
        for (Size i = 0u; i < proto::kMaxPayload + 16u; ++i) {
            buf[proto::kDataHeaderSize + i] = g_image[(300u + i) % kSize];
        }
        check("отправлен переросший кадр",
              host.send_oversized(proto::Op::Data, buf, sizeof buf));
        (void)host.wait_response(400u);
        check("переросший кадр -> NAK TOO_LARGE",
              host.last_is_nak() &&
              host.response().code == static_cast<uint8_t>(proto::Code::TooLarge));
        check_eq<uint32_t>("в ответе предел блока",
                           host.response().detail,
                           static_cast<uint32_t>(proto::data_block_limit(proto::kMaxPayload)));
    }
    check("поток не разъехался: следующий блок принят",
          host.data(300u, g_image + 300u, 100u) && host.last_is_ack());
    check_eq<uint32_t>("next_offset = 400", host.response().next_offset, 400u);
}

// ───────────────── 4. Неверная CRC и проверка чтением ──────────────────────

void test_verify_failures() {
    (void)std::puts("неверный образ и «запись прошла, а данных нет»:");
    const StatsBase st0;

    MemoryWire& wire = g_wire;
    reset_world(wire);
    (void)S().init(make_config(&wire));

    constexpr uint32_t kSize = 2048u;
    make_image(kSize, 3u);

    Host host(wire);
    (void)host.info();

    // ── Образ с неверной CRC ──
    proto::Start s{};
    s.image_size = kSize;
    s.image_crc32 = image_crc(kSize) ^ 0xDEADBEEFu;   // хост «ошибся»
    s.fw_version = 2u;
    check("START (неверная CRC образа)", host.start(s));
    (void)host.send_image(kSize, 124u);
    check("END", host.end(kSize, image_crc(kSize), nullptr));
    check("END -> NAK VERIFY",
          host.last_is_nak() &&
          host.response().code == static_cast<uint8_t>(proto::Code::Verify));
    check_eq<uint32_t>("состояние (Failed)",
                       host.response().state, static_cast<uint8_t>(proto::State::Failed));
    check_eq<uint32_t>("провалов проверки", st0.verify_fail(), 1u);

    SlotRecord rec{};
    check("запись слота читается",
          S().slot_record(static_cast<uint8_t>(host.response().slot), rec) == Status::Ok);
    check("слот НЕ помечен готовым", !rec.verified());

    // ── «Запись прошла, но в носителе не то» ──
    //
    // Модель носителя принимает write(), но ничего не меняет — ровно так ведёт
    // себя изнашиваемая Flash. Обнаружить это можно только ЧТЕНИЕМ, поэтому
    // тест проверяет, что сервис читает записанное.
    reset_world(wire);
    check("init", S().init(make_config(&wire)) == Status::Ok);
    // Подмена включается ТОЛЬКО для области образа: метаданные пишутся
    // честно, поэтому отказ виден именно на проверке чтением образа.
    F().write_drops_ = true;
    F().write_drops_from = kSlotBOffset;

    Host host2(wire);
    (void)host2.info();
    proto::Start s2{};
    s2.image_size = kSize;
    s2.image_crc32 = image_crc(kSize);
    s2.fw_version = 2u;
    check("START (запись в никуда)", host2.start(s2));
    (void)host2.send_image(kSize, 124u);
    check("END", host2.end(kSize, image_crc(kSize), nullptr));
    check("END -> NAK STORAGE (проверка чтением)",
          host2.last_is_nak() &&
          host2.response().code == static_cast<uint8_t>(proto::Code::Storage));
    check_eq<uint32_t>("провал проверки чтением учтён", st0.readback_fail(), 1u);
}

// ──────────────────────── 5. Продолжение после сбоя ────────────────────────

void test_resume_after_power_loss() {
    (void)std::puts("продолжение передачи после потери питания:");
    const StatsBase st0;

    MemoryWire& wire = g_wire;
    reset_world(wire);
    check("init", S().init(make_config(&wire)) == Status::Ok);

    constexpr uint32_t kSize = 4096u;
    make_image(kSize, 4u);
    const uint32_t crc = image_crc(kSize);

    Host host(wire);
    (void)host.info();

    proto::Start s{};
    s.image_size = kSize;
    s.image_crc32 = crc;
    s.fw_version = 2u;
    check("START", host.start(s));

    // Передаём часть и «выключаем питание»: RAM сервиса забыта, носитель — нет.
    const uint32_t sent = host.send_image(kSize, 124u, /*stop_after*/ 1024u);
    check("передано больше 1 КБ", sent >= 1024u);
    power_cycle();
    check("init после сбоя", S().init(make_config(&wire)) == Status::Ok);

    Host host2(wire);
    (void)host2.info();
    proto::Start r = s;
    r.flags = proto::kStartResume;
    r.resume_offset = sent;                      // хост «помнит» всё, что отправил
    check("START + Resume", host2.start(r));
    check("Resume -> ACK", host2.last_is_ack());
    // Смещение продолжения выбирает УСТРОЙСТВО по тому, что реально лежит:
    // оно не больше отправленного (часть последних блоков могла не попасть в
    // контрольную точку) и не «съедает» больше одного интервала.
    const uint32_t resume_from = host2.response().next_offset;
    check("продолжение — не дальше отправленного", resume_from <= sent);
    check("продолжение — рядом с отправленным",
          resume_from + cfg::kResumeSlack >= sent);
    check_eq<uint32_t>("продолжение учтено", st0.resumed(), 1u);

    const uint32_t more = host2.send_image(kSize, 124u, 0u, resume_from);
    check_eq<uint32_t>("передано до конца", more, kSize);
    check("END", host2.end(kSize, crc, nullptr));
    check("END -> ACK после продолжения", host2.last_is_ack());
    check_eq<uint32_t>("состояние VERIFIED",
                       host2.response().state, static_cast<uint8_t>(proto::State::Verified));

    // Контроль: образ в носителе — тот самый, а не «склеенный из двух».
    uint8_t back[128]{};
    check("чтение начала образа",
          g_storage.read(kSlotBOffset, back, sizeof back) == Status::Ok);
    check("начало совпало", std::memcmp(back, g_image, sizeof back) == 0);
    check("чтение конца образа",
          g_storage.read(kSlotBOffset + kSize - 128u, back, sizeof back) == Status::Ok);
    check("конец совпал", std::memcmp(back, g_image + kSize - 128u, sizeof back) == 0);

    // ── Испорченный префикс: продолжать нельзя ──
    reset_world(wire);
    check("init", S().init(make_config(&wire)) == Status::Ok);
    Host host3(wire);
    (void)host3.info();
    check("START заново", host3.start(s));
    (void)host3.send_image(kSize, 124u, 2048u);
    power_cycle();
    F().poke(kSlotBOffset + 100u, static_cast<uint8_t>(g_image[100] ^ 0x5Au));  // порча

    check("init", S().init(make_config(&wire)) == Status::Ok);
    Host host4(wire);
    (void)host4.info();
    check("START + Resume с испорченным префиксом", host4.start(r));
    check_eq<uint32_t>("продолжать нельзя: просим с начала",
                       host4.response().next_offset, 0u);
    check("отказ продолжения учтён", st0.resume_rejected() >= 1u);
}

// ───────────────────── 6. SHA-256, подпись, откат версии ───────────────────

// «Подпись» для теста: функция от сообщения и ключа. Смысл не в
// криптостойкости, а в том, что проверку выполняет ПРИЛОЖЕНИЕ, а модуль лишь
// передаёт ему ровно то, что подписано.
constexpr uint8_t kTestKey = 0xA5u;

bool test_verifier(void* /*ctx*/, uint8_t /*kind*/, const uint8_t* message,
                   Size len, const uint8_t* signature, Size sig_len) noexcept {
    if (sig_len != 32u) return false;
    crypto::Sha2<crypto::Sha2Variant::Sha256> h;
    h.update(&kTestKey, 1u);
    h.update(message, len);
    uint8_t digest[32]{};
    h.finalize(digest);
    for (Size i = 0u; i < 32u; ++i) {
        if (digest[i] != signature[i]) return false;
    }
    return true;
}

void make_signature(uint32_t size, uint32_t crc, uint32_t fw_version,
                    const uint8_t sha[32], uint8_t out[32]) {
    uint8_t message[52];
    message[0] = 'M'; message[1] = 'C'; message[2] = 'F'; message[3] = 'R';
    message[4] = 'A'; message[5] = 'M'; message[6] = 'E'; message[7] = 'O';
    proto::put_u32(message + 8u, size);
    proto::put_u32(message + 12u, crc);
    proto::put_u32(message + 16u, fw_version);
    for (Size i = 0u; i < 32u; ++i) message[20u + i] = sha[i];

    crypto::Sha2<crypto::Sha2Variant::Sha256> h;
    h.update(&kTestKey, 1u);
    h.update(message, sizeof message);
    h.finalize(out);
}

void test_hash_signature_policy() {
    (void)std::puts("SHA-256, подпись и политика версии:");
    const StatsBase st0;

    MemoryWire& wire = g_wire;
    reset_world(wire);

    constexpr uint32_t kSize = 2048u;
    make_image(kSize, 5u);
    const uint32_t crc = image_crc(kSize);
    uint8_t sha[32]{};
    image_sha(kSize, sha);

    Config cfg = make_config(&wire);
    cfg.fw_version = 10u;                        // работающая версия
    cfg.require_signature = true;
    cfg.verify_signature = &test_verifier;
    check("init", S().init(cfg) == Status::Ok);

    Host host(wire);
    check("INFO", host.info());
    check("INFO: подпись заявлена", (host.last_info().caps & proto::kCapSign) != 0u);

    // ── Откат версии запрещён ──
    proto::Start old{};
    old.image_size = kSize;
    old.image_crc32 = crc;
    old.fw_version = 5u;                         // старее работающей
    check("START старой версии", host.start(old));
    check("откат -> NAK DENIED",
          host.last_is_nak() &&
          host.response().code == static_cast<uint8_t>(proto::Code::Denied));

    // ── CRC + SHA-256 + подпись ──
    proto::Start s{};
    s.image_size = kSize;
    s.image_crc32 = crc;
    s.fw_version = 11u;
    s.hash_kind = static_cast<uint8_t>(proto::HashKind::Sha256);
    std::memcpy(s.sha256, sha, 32u);
    check("START с SHA-256", host.start(s));
    check("START -> ACK", host.last_is_ack());
    (void)host.send_image(kSize, 124u);

    check("END", host.end(kSize, crc, sha));
    check("END -> ACK, но нужна подпись", host.last_is_ack());
    check_eq<uint32_t>("состояние COMPLETE (ещё не годен)",
                       host.response().state, static_cast<uint8_t>(proto::State::Complete));

    uint8_t sig[32]{};
    make_signature(kSize, crc, 11u, sha, sig);

    // Неверная подпись: образ целый, но чужой.
    uint8_t bad_sig[32];
    std::memcpy(bad_sig, sig, 32u);
    bad_sig[0] = static_cast<uint8_t>(bad_sig[0] ^ 0xFFu);
    check("SIGN с неверной подписью", host.sign(32u, 0u, 1u, bad_sig, 32u, false));
    check("подпись не принята -> NAK AUTH",
          host.last_is_nak() &&
          host.response().code == static_cast<uint8_t>(proto::Code::Auth));
    check_eq<uint32_t>("провал подписи учтён", st0.signature_fail(), 1u);

    // ── Та же передача, но с верной подписью ──
    reset_world(wire);
    check("init заново", S().init(cfg) == Status::Ok);
    Host host2(wire);
    (void)host2.info();
    check("START", host2.start(s));
    (void)host2.send_image(kSize, 124u);
    check("END", host2.end(kSize, crc, sha));
    check("SIGN верной подписью", host2.sign(32u, 0u, 1u, sig, 32u, false));
    check("SIGN -> ACK", host2.last_is_ack());
    check_eq<uint32_t>("состояние VERIFIED",
                       host2.response().state, static_cast<uint8_t>(proto::State::Verified));
    check_eq<uint32_t>("подпись подтверждена", st0.signature_ok(), 1u);

    SlotRecord rec{};
    (void)S().slot_record(static_cast<uint8_t>(host2.response().slot), rec);
    check("флаг подписи в записи", (rec.flags & kRecFlagSignOk) != 0u);
    check("целостность в записи не потеряна подписью", rec.crc_ok());

    // ── Подпись фрагментами (длинные подписи вроде RSA-2048) ──
    reset_world(wire);
    (void)S().init(cfg);
    Host host5(wire);
    (void)host5.info();
    (void)host5.start(s);
    (void)host5.send_image(kSize, 124u);
    (void)host5.end(kSize, crc, sha);
    check("SIGN фрагмент 1/2", host5.sign(32u, 0u, 1u, sig, 16u, true));
    check("после фрагмента нужен ещё", host5.last_is_ack() &&
                                       host5.response().next_offset == 16u);
    check("SIGN фрагмент 2/2", host5.sign(32u, 16u, 1u, sig + 16u, 16u, false));
    check("подпись из фрагментов принята",
          host5.last_is_ack() &&
          host5.response().state == static_cast<uint8_t>(proto::State::Verified));

    // ── Неверный SHA-256 ──
    reset_world(wire);
    check("init", S().init(cfg) == Status::Ok);
    Host host3(wire);
    (void)host3.info();
    proto::Start s3 = s;
    std::memset(s3.sha256, 0u, 32u);             // хост заявил другой хеш
    (void)host3.start(s3);
    (void)host3.send_image(kSize, 124u);
    uint8_t wrong[32]{};
    (void)host3.end(kSize, crc, wrong);
    check("неверный SHA-256 -> NAK VERIFY",
          host3.last_is_nak() &&
          host3.response().code == static_cast<uint8_t>(proto::Code::Verify));
}

// ──────────────────── 7. Активация, подтверждение, откат ───────────────────

void test_activation_and_rollback() {
    (void)std::puts("активация, подтверждение загрузки и откат:");

    MemoryWire& wire = g_wire;
    reset_world(wire);
    check("init", S().init(make_config(&wire, /*active_slot*/ 0u)) == Status::Ok);

    constexpr uint32_t kSize = 2048u;
    make_image(kSize, 6u);
    const uint32_t crc = image_crc(kSize);

    Host host(wire);
    (void)host.info();

    // ── Приём в слот B ──
    proto::Start s{};
    s.image_size = kSize;
    s.image_crc32 = crc;
    s.fw_version = 2u;
    s.slot = 1u;
    check("START в слот 1", host.start(s));
    (void)host.send_image(kSize, 124u);
    check("END", host.end(kSize, crc, nullptr));
    check("образ проверен",
          host.response().state == static_cast<uint8_t>(proto::State::Verified));

    // Активировать непроверенный слот нельзя — проверяем на пустом слоте A.
    check("ACTIVATE слота A (пуст)", host.activate(0u));
    check("активация пустого слота -> NAK",
          host.last_is_nak() &&
          host.response().code == static_cast<uint8_t>(proto::Code::BadState));

    check("ACTIVATE слота B", host.activate(1u));
    check("ACTIVATE -> ACK", host.last_is_ack());
    check_eq<uint32_t>("состояние ACTIVATED",
                       host.response().state, static_cast<uint8_t>(proto::State::Activated));

    // ── «Перезагрузка»: устройство теперь грузит слот 1 ──
    power_cycle();
    Config cfg = make_config(&wire, /*active_slot*/ 1u);
    cfg.max_boot_attempts = 2u;
    check("init (активен слот 1)", S().init(cfg) == Status::Ok);

    BootInfo bi{};
    check("boot_check", S().boot_check(bi) == Status::Ok);
    check("загрузка пробная", bi.trial);
    check_eq<uint32_t>("попытка №1", bi.boot_attempts, 1u);
    check_eq<uint32_t>("версия из записи", bi.fw_version, 2u);

    // «Прошивка не подтвердила себя»: сброс за сбросом.
    for (uint32_t i = 0u; i < 3u; ++i) {
        power_cycle();
        (void)S().init(cfg);
        (void)S().boot_check(bi);
    }
    check("попытки исчерпаны -> нужен откат", bi.rollback_needed);

    // Откатываться некуда: слот A пуст. Модуль обязан снять активацию, а не
    // делать вид, что «вернулся».
    check("rollback без годного слота", S().rollback() == Status::NotFound);

    // ── Кладём годный образ и в слот A и откатываемся на него ──
    power_cycle();
    check("init", S().init(make_config(&wire, /*active_slot*/ 1u)) == Status::Ok);
    F().preload(kSlotAOffset, g_image, kSize);        // «залито программатором»
    check("verify_stored слота A",
          S().verify_stored(0u, kSize, crc, nullptr) == Status::Ok);
    check("rollback на слот A", S().rollback() == Status::Ok);
    SlotRecord rec{};
    (void)S().slot_record(0u, rec);
    check_eq<uint32_t>("слот A активирован",
                       rec.state, static_cast<uint8_t>(proto::State::Activated));

    // ── Подтверждение работы: счётчик попыток сбрасывается ──
    power_cycle();
    check("init (снова слот 1)", S().init(make_config(&wire, /*active_slot*/ 1u)) == Status::Ok);
    (void)S().boot_check(bi);
    check("подтверждение запущенной прошивки", S().confirm_running() == Status::Ok);
    (void)S().slot_record(1u, rec);
    check_eq<uint32_t>("состояние RUNNING",
                       rec.state, static_cast<uint8_t>(proto::State::Running));
    check_eq<uint32_t>("попытки сброшены", rec.boot_attempts, 0u);

    // Отметка о попытке пережила сброс (резервные регистры mock-платформы).
    mock_platform::WatchdogModel& wm = mock_platform::watchdog_model();
    check_eq<uint32_t>("отметка OTA в резервных регистрах",
                       wm.notes[kNoteIdxMagic + 5u], kNoteMagic);
}

// ─────────────────────── 8. Политика: что запрещено ────────────────────────

void test_policy() {
    (void)std::puts("запреты: активный слот и готовая копия:");

    MemoryWire& wire = g_wire;
    reset_world(wire);
    check("init", S().init(make_config(&wire, /*active_slot*/ 0u)) == Status::Ok);

    constexpr uint32_t kSize = 1024u;
    make_image(kSize, 7u);
    const uint32_t crc = image_crc(kSize);

    Host host(wire);
    (void)host.info();

    // Стирание АКТИВНОГО слота — это стирание самого себя: без загрузчика
    // такое обязано быть запрещено.
    check("ERASE активного слота", host.erase(0u));
    check("ERASE активного слота -> NAK DENIED",
          host.last_is_nak() &&
          host.response().code == static_cast<uint8_t>(proto::Code::Denied));
    check("ERASE неактивного слота", host.erase(1u));
    check("ERASE неактивного слота -> ACK", host.last_is_ack());

    // Готовая (проверенная) копия в слоте — это ЗАПАС на случай отката.
    // Затирать её молча нельзя: нужно явное согласие kStartOverwrite.
    F().preload(kSlotBOffset, g_image, kSize);
    check("годный образ в слоте 1",
          S().verify_stored(1u, kSize, crc, nullptr) == Status::Ok);

    make_image(kSize, 8u);                       // другой образ
    proto::Start s{};
    s.image_size = kSize;
    s.image_crc32 = image_crc(kSize);
    s.fw_version = 2u;
    s.slot = 1u;
    check("START другого образа в занятый слот", host.start(s));
    check("занятый слот -> NAK DENIED",
          host.last_is_nak() &&
          host.response().code == static_cast<uint8_t>(proto::Code::Denied));

    s.flags = proto::kStartOverwrite;
    check("START с kStartOverwrite", host.start(s));
    check("явное согласие -> ACK", host.last_is_ack());
}

// ────────────────────── 9. Режим «интерфейса нет» ──────────────────────────

void test_no_transport() {
    (void)std::puts("без интерфейса (образ залит другим каналом):");

    MemoryWire& wire = g_wire;
    reset_world(wire);
    Config cfg = make_config(nullptr);
    check("init без транспорта", S().init(cfg) == Status::Ok);
    check_eq<uint32_t>("интерфейс = none",
                       static_cast<uint32_t>(S().iface()),
                       static_cast<uint32_t>(proto::Iface::None));
    check("poll без интерфейса -> NotSupported", S().poll(0u) == Status::NotSupported);

    constexpr uint32_t kSize = 1024u;
    make_image(kSize, 9u);
    const uint32_t crc = image_crc(kSize);
    uint8_t sha[32]{};
    image_sha(kSize, sha);

    // «Программатор» положил образ в слот 1 напрямую.
    F().preload(kSlotBOffset, g_image, kSize);

    check("verify_stored: CRC не сходится -> Corruption",
          S().verify_stored(1u, kSize, crc ^ 1u, nullptr) == Status::Corruption);
    check("verify_stored: всё сходится",
          S().verify_stored(1u, kSize, crc, sha) == Status::Ok);

    SlotRecord rec{};
    (void)S().slot_record(1u, rec);
    check("слот признан годным", rec.verified());
    check_eq<uint32_t>("размер записан", rec.image_size, kSize);

    check("активация", S().activate(1u) == Status::Ok);
    check("активация слота без записи -> NotFound", S().activate(0u) == Status::NotFound);
}

// ─────────────────────── 10. Приём через UART (провод) ─────────────────────

void test_uart_transport() {
    (void)std::puts("приём по UART (mock-UART как провод):");

    UartWire& wire = g_uart_wire;
    reset_world(wire);
    check("init с UART-транспортом", S().init(make_config(&wire)) == Status::Ok);
    check_eq<uint32_t>("интерфейс = uart",
                       static_cast<uint32_t>(S().iface()),
                       static_cast<uint32_t>(proto::Iface::Uart));

    constexpr uint32_t kSize = 1024u;
    make_image(kSize, 10u);
    const uint32_t crc = image_crc(kSize);

    Host host(wire);
    check("INFO по UART", host.info());
    check_eq<uint32_t>("INFO: интерфейс uart",
                       host.last_info().iface, static_cast<uint8_t>(proto::Iface::Uart));

    proto::Start s{};
    s.image_size = kSize;
    s.image_crc32 = crc;
    s.fw_version = 2u;
    check("START по UART", host.start(s));
    check("START -> ACK", host.last_is_ack());

    const uint32_t sent = host.send_image(kSize, 64u);
    check_eq<uint32_t>("передано по UART", sent, kSize);
    check("END по UART", host.end(kSize, crc, nullptr));
    check("END -> ACK", host.last_is_ack());
    check_eq<uint32_t>("состояние VERIFIED",
                       host.response().state, static_cast<uint8_t>(proto::State::Verified));

    uint8_t back[32]{};
    check("образ в носителе совпал",
          g_storage.read(kSlotBOffset, back, sizeof back) == Status::Ok &&
          std::memcmp(back, g_image, sizeof back) == 0);
}

// ─────────────── 11. Другой носитель: внешняя память платформы ─────────────
//
// Тот же сервис, тот же протокол, но Storage другой: проверяется, что логика
// приёма не привязана к встроенной Flash. Модель внешней памяти mock-платформы
// (64 КБ, стирание страницами по 4 КБ) отвечает через контракт ext_rom_*.

void test_ext_rom_storage() {
    (void)std::puts("внешняя память (ExtRomStorage, страница 4 КБ):");
    const StatsBase st0;

    MemoryWire& wire = g_wire;
    reset_world(wire);

    static ExtRomStorage ext(4096u);              // страница — как у SPI-flash
    Config cfg = make_config(&wire);
    cfg.storage          = &ext;
    cfg.meta_offset      = 0u;                    // 8 страниц метаданных = 32 КБ? нет:
                                                  // 4 страницы по 4 КБ = 16 КБ
    cfg.slot_offset[0]   = 16u * 1024u;
    cfg.slot_capacity[0] = 16u * 1024u;
    cfg.slot_offset[1]   = 32u * 1024u;
    cfg.slot_capacity[1] = 16u * 1024u;
    check("init с внешней памятью", S().init(cfg) == Status::Ok);
    check_eq<uint32_t>("носитель в отчёте",
                       static_cast<uint32_t>(S().config().storage->page_size()), 4096u);

    constexpr uint32_t kSize = 2048u;
    make_image(kSize, 13u);
    const uint32_t crc = image_crc(kSize);

    Host host(wire);
    check("INFO", host.info());
    check_eq<uint32_t>("предел образа = ёмкость слота",
                       host.last_info().image_limit, 16u * 1024u);

    proto::Start s{};
    s.image_size = kSize;
    s.image_crc32 = crc;
    s.fw_version = 2u;
    check("START", host.start(s));
    (void)host.send_image(kSize, 124u);
    check("END", host.end(kSize, crc, nullptr));
    check("END -> ACK", host.last_is_ack());
    check_eq<uint32_t>("состояние VERIFIED",
                       host.response().state, static_cast<uint8_t>(proto::State::Verified));

    // Стирание шло страницами по 4 КБ: одна страница на 2 КБ образа.
    check_eq<uint32_t>("стёрта ровно одна страница внешней памяти",
                       st0.page_erases(), 1u);

    uint8_t back[32]{};
    check("образ во внешней памяти совпал",
          ext.read(cfg.slot_offset[1], back, sizeof back) == Status::Ok &&
          std::memcmp(back, g_image, sizeof back) == 0);
}

// ─────────────────────── 12. Прочие операции протокола ─────────────────────

void test_misc_ops() {
    (void)std::puts("STATE, READ, ABORT и неизвестная операция:");
    const StatsBase st0;

    MemoryWire& wire = g_wire;
    reset_world(wire);
    (void)S().init(make_config(&wire));

    constexpr uint32_t kSize = 512u;
    make_image(kSize, 11u);
    const uint32_t crc = image_crc(kSize);

    Host host(wire);
    (void)host.info();

    proto::Start s{};
    s.image_size = kSize;
    s.image_crc32 = crc;
    s.fw_version = 2u;
    check("START", host.start(s));
    const uint32_t half = host.send_image(kSize, 124u, 124u);   // один блок

    check("STATE", host.state());
    check_eq<uint32_t>("STATE: состояние RECEIVING",
                       host.response().state, static_cast<uint8_t>(proto::State::Receiving));
    check_eq<uint32_t>("STATE: принято ровно блок", host.response().next_offset, half);

    // READ — диагностика: сверяем то, что лежит в слоте.
    check("READ 32 байта из слота", host.read(0u, 32u));
    check("READ -> ACK с данными", host.last_is_ack() && host.last_data_len() == 32u);
    check("READ: данные совпали", std::memcmp(host.last_data(), g_image, 32u) == 0);

    check("READ за пределами слота", host.read(static_cast<uint32_t>(kSlotBSize), 16u));
    check("READ за пределом -> NAK",
          host.last_is_nak() &&
          host.response().code == static_cast<uint8_t>(proto::Code::BadOffset));

    // ABORT: сессия закрывается, слот не готов.
    check("ABORT", host.abort(1u));
    check("ABORT -> ACK", host.last_is_ack());
    check_eq<uint32_t>("состояние (Failed)",
                       host.response().state, static_cast<uint8_t>(proto::State::Failed));
    check_eq<uint32_t>("счётчик отмен", st0.aborts(), 1u);
    check("DATA после ABORT -> NAK BAD_STATE",
          host.data(128u, g_image + 128u, 100u) && host.last_is_nak() &&
          host.response().code == static_cast<uint8_t>(proto::Code::BadState));

    // Неизвестная операция: устройство обязано ответить, а не молчать.
    check("неизвестная операция", host.send(static_cast<proto::Op>(0x40u), 0u, nullptr, 0u));
    (void)host.wait_response(50u);
    check("неизвестная операция -> NAK UNSUPPORTED",
          host.last_is_nak() &&
          host.response().code == static_cast<uint8_t>(proto::Code::Unsupported));

    // После отмены слот можно перезаписать: START проходит.
    check("START после отмены", host.start(s));
    check("START после отмены -> ACK", host.last_is_ack());

    // ── VERIFY по проводу: образ уже лежит в слоте ──
    //
    // Так выглядит режим «залито другим каналом, устройству остаётся признать»:
    // тот же путь, что и verify_stored(), но запускается кадром протокола.
    reset_world(wire);
    (void)S().init(make_config(&wire));
    make_image(kSize, 12u);
    F().preload(kSlotBOffset, g_image, kSize);

    Host host6(wire);
    (void)host6.info();
    proto::Verify v{};
    v.image_size = kSize;
    v.image_crc32 = image_crc(kSize);
    v.slot = 1u;
    check("VERIFY слота с верной CRC", host6.verify(v));
    check("VERIFY -> ACK", host6.last_is_ack());
    check_eq<uint32_t>("состояние VERIFIED",
                       host6.response().state, static_cast<uint8_t>(proto::State::Verified));

    v.image_crc32 ^= 0x1u;
    check("VERIFY с неверной CRC", host6.verify(v));
    check("VERIFY -> NAK",
          host6.last_is_nak() &&
          host6.response().code == static_cast<uint8_t>(proto::Code::Verify));

    // ── Образ больше слота отвергается НА СТАРТЕ, а не посреди передачи ──
    reset_world(wire);
    (void)S().init(make_config(&wire));
    Host host7(wire);
    (void)host7.info();
    proto::Start big{};
    big.image_size = kSlotASize + 1u;
    big.image_crc32 = 0x1234u;
    big.fw_version = 2u;
    check("START образа больше слота", host7.start(big));
    check("образ больше слота -> NAK NO_SPACE",
          host7.last_is_nak() &&
          host7.response().code == static_cast<uint8_t>(proto::Code::NoSpace));
}

void test_session_timeout() {
    (void)std::puts("обрыв связи: простой сессии и продолжение:");
    const StatsBase st0;

    MemoryWire& wire = g_wire;
    reset_world(wire);

    Config cfg = make_config(&wire);
    cfg.session_timeout_ms = 100u;               // «хост молчит 100 мс»
    check("init", S().init(cfg) == Status::Ok);

    // Таймер ОС запускаем ТОЛЬКО здесь: в остальных сценах время не нужно, а
    // идущий тик менял бы смысл таймаутов интерфейса.
    check("таймер ОС запущен", config::OsTimer::Start() == Status::Ok);

    constexpr uint32_t kSize = 1024u;
    make_image(kSize, 14u);
    const uint32_t crc = image_crc(kSize);

    Host host(wire);
    (void)host.info();

    proto::Start s{};
    s.image_size = kSize;
    s.image_crc32 = crc;
    s.fw_version = 2u;
    check("START", host.start(s));
    const uint32_t sent = host.send_image(kSize, 124u, 248u);
    check("передана часть", sent >= 248u);

    // Хост молчит: продвигаем время тиками ОС.
    for (uint32_t i = 0u; i < 300u; ++i) config::OsTimer::TickIsr();
    (void)S().poll(0u);

    check_eq<uint32_t>("сессия закрыта по простою", st0.expired(), 1u);
    check("сессия не активна", !S().session_active());

    // Приходит новый хост (или тот же, после перезапуска) — продолжает.
    Host host2(wire);
    (void)host2.info();
    proto::Start r = s;
    r.flags = proto::kStartResume;
    check("START + Resume после простоя", host2.start(r));
    check("Resume -> ACK", host2.last_is_ack());
    const uint32_t resume_from = host2.response().next_offset;
    check("продолжение не дальше отправленного", resume_from <= sent);
    (void)host2.send_image(kSize, 124u, 0u, resume_from);
    check("END", host2.end(kSize, crc, nullptr));
    check("END -> ACK", host2.last_is_ack());
    check_eq<uint32_t>("состояние VERIFIED",
                       host2.response().state, static_cast<uint8_t>(proto::State::Verified));
}
// ─────────── 14. Устойчивость разбора: мусор в линии не ломает приём ───────
//
// Главное свойство приёмника, ради которого декодер написан именно так: ОДНА
// ошибка в линии стоит одного потерянного кадра, а не всего сеанса связи.
// Здесь это проверяется на потоке, который состоит из мусора, «похожего на
// кадры» (в том числе с настоящими SOF и правдоподобными длинами), и валидных
// кадров. Последовательность порождается ГПСЧ с фиксированным зерном, поэтому
// прогон воспроизводим.

void test_decoder_robustness() {
    (void)std::puts("устойчивость разбора: мусор в линии:");

    static uint8_t buf[proto::decoder_buffer_size(proto::kMaxPayload)];
    static proto::Decoder dec;
    dec.init(buf, sizeof buf, proto::kMaxPayload);

    uint32_t x = 0x13579BDFu;
    auto rnd = [&x]() noexcept {
        x = x * 1664525u + 1013904223u;
        return static_cast<uint8_t>(x >> 24);
    };

    constexpr uint32_t kRounds = 200u;
    uint32_t recovered = 0u;
    uint32_t junk_total = 0u;
    uint8_t  frame[proto::kMaxPayload + proto::kFrameOverhead]{};
    uint8_t  zeros[proto::kMaxSkipLen + 64u]{};

    for (uint32_t round = 0u; round < kRounds; ++round) {
        // 1. Мусор: случайные байты, но каждый седьмой — SOF (0x4F/0x54),
        //    чтобы ложные кандидаты возникали регулярно.
        const uint32_t junk_len = 1u + (round % 64u);
        uint8_t junk[96];
        for (uint32_t i = 0u; i < junk_len; ++i) {
            const uint8_t b = rnd();
            junk[i] = ((i % 7u) == 0u) ? (((i % 14u) == 0u) ? proto::kSof0 : proto::kSof1)
                                       : b;
        }

        // 2. «Отстойник»: заведомо больше предела проглатываемого кадра, чтобы
        //    любой ложный кандидат успел завершиться и провалить CRC.
        // 3. Валидный кадр — он обязан быть распознан.
        uint8_t payload[8] = {0u};
        proto::put_u32(payload, round);
        const uint8_t seq = static_cast<uint8_t>(round & 0xFFu);
        const Size flen = proto::encode(frame, sizeof frame, proto::Op::State, 0u,
                                        seq, payload, sizeof payload);

        proto::Frame f{};
        bool ready = false;
        const auto feed_all = [&](const uint8_t* data, Size len) noexcept {
            Size pos = 0u;
            while (pos < len) {
                const auto step = dec.feed(data + pos, 1u, f);
                pos += step.consumed;
                if (step.ready) { ready = true; return; }
            }
        };
        feed_all(junk, junk_len);
        junk_total += junk_len;
        if (!ready) feed_all(zeros, sizeof zeros);
        if (!ready) feed_all(frame, flen);

        if (ready && f.op == proto::Op::State && f.seq == seq && !f.too_large) ++recovered;
    }

    check_eq<uint32_t>("кадры, найденные после мусора", recovered, kRounds);
    check("об ошибках разбора сообщено", dec.fault_count() > 0u);
    // После распознанного кадра состояние разбора обязано быть ЧИСТЫМ: иначе
    // следующая ошибка в линии стоила бы уже двух кадров.
    check("состояние разбора чистое",
          dec.last_fault() == proto::Decoder::Fault::None);
    (void)junk_total;

    // ── Кадр больше предела: op/seq обязаны быть СВОИМИ ──
    //
    // Такой кадр не сохраняется в буфер (его читают «на проходе»), поэтому
    // поля заголовка обязаны быть запомнены при разборе, а не перечитаны из
    // буфера позже: там уже лежат чужие байты.
    {
        dec.reset();
        static uint8_t big[proto::kFrameOverhead + 200u];
        uint8_t big_payload[200];
        for (Size i = 0u; i < sizeof big_payload; ++i) {
            big_payload[i] = static_cast<uint8_t>(i);
        }
        big[0] = proto::kSof0;
        big[1] = proto::kSof1;
        big[2] = proto::kVersion;
        big[3] = static_cast<uint8_t>(proto::Op::Data);
        big[4] = 0u;
        big[5] = 0x77u;                       // запоминаем этот SEQ
        proto::put_u16(big + 6u, sizeof big_payload);
        big[8] = proto::header_crc(big);
        for (Size i = 0u; i < sizeof big_payload; ++i) {
            big[proto::kHeaderInFrame + i] = big_payload[i];
        }
        proto::put_u32(big + proto::kHeaderInFrame + sizeof big_payload,
                       proto::image_crc(big + 2u, proto::crc_cover_len(sizeof big_payload)));

        const uint8_t junk3[] = {0x11u, 0x22u, 0x33u};
        proto::Frame got{};
        bool ready3 = false;
        const auto feed3 = [&](const uint8_t* d, Size n) noexcept {
            Size p2 = 0u;
            while (p2 < n) {
                const auto st = dec.feed(d + p2, 1u, got);
                p2 += st.consumed;
                if (st.ready) { ready3 = true; return; }
            }
        };
        feed3(junk3, sizeof junk3);
        feed3(big, sizeof big);

        check("переросший кадр распознан", ready3 && got.too_large);
        check_eq<uint32_t>("op переросшего кадра", static_cast<uint32_t>(got.op),
                           static_cast<uint32_t>(proto::Op::Data));
        check_eq<uint32_t>("seq переросшего кадра", got.seq, 0x77u);
    }

}

// ─────── 15. Регрессии: то, что нашёл разбор кода, а не счастливый путь ─────
//
// Каждая проверка здесь соответствует конкретному дефекту, найденному при
// ревью модуля. Без них «исправлено» держалось бы на честном слове.

void test_regressions() {
    (void)std::puts("регрессии: исполняемый слот, чужой SIGN, подпись и VERIFY:");
    const StatsBase st0;

    MemoryWire& wire = g_wire;

    // ── 1. Писать в ИСПОЛНЯЕМЫЙ слот нельзя ──
    //
    // Стирание страницы, из которой исполняется код, — это HardFault. Запрет
    // обязан действовать и когда слот назначен явно, и когда выбран
    // автоматически.
    reset_world(wire);
    check("init (активен слот 0)", S().init(make_config(&wire, 0u)) == Status::Ok);

    constexpr uint32_t kSize = 1024u;
    make_image(kSize, 20u);

    Host host(wire);
    (void)host.info();
    proto::Start s{};
    s.image_size  = kSize;
    s.image_crc32 = image_crc(kSize);
    s.fw_version  = 2u;
    s.slot        = 0u;                       // ИСПОЛНЯЕМЫЙ слот
    check("START в исполняемый слот", host.start(s));
    check("исполняемый слот -> NAK DENIED",
          host.last_is_nak() &&
          host.response().code == static_cast<uint8_t>(proto::Code::Denied));
    check("слот не тронут: запись осталась пустой", [&] {
        SlotRecord rec{};
        return S().slot_record(0u, rec) != Status::Ok;
    }());

    // Явное разрешение (код работает из ОЗУ) снимает запрет.
    Config allow = make_config(&wire, 0u);
    allow.allow_active_erase = true;
    check("init с allow_active_erase", S().init(allow) == Status::Ok);
    Host host2(wire);
    (void)host2.info();
    check("START в исполняемый слот разрешён", host2.start(s));
    check("allow_active_erase -> ACK", host2.last_is_ack());

    // ── 2. SIGN вне сессии не смеет менять запись чужого слота ──
    //
    // Кадр приходит после перезагрузки (или после чужого VERIFY), когда
    // никакой сессии нет: «догадываться», о каком слоте речь, он не должен.
    reset_world(wire);
    (void)S().init(make_config(&wire, 0u));
    F().preload(kSlotBOffset, g_image, kSize);
    check("годный образ в слоте 1",
          S().verify_stored(1u, kSize, image_crc(kSize), nullptr) == Status::Ok);

    SlotRecord before{};
    (void)S().slot_record(1u, before);
    check("запись слота 1 = Verified",
          before.state == static_cast<uint8_t>(proto::State::Verified));

    Host host3(wire);
    (void)host3.info();
    uint8_t sig[32]{};
    check("SIGN вне сессии", host3.sign(32u, 0u, 1u, sig, 32u, false));
    check("SIGN вне сессии -> NAK",
          host3.last_is_nak() &&
          host3.response().code == static_cast<uint8_t>(proto::Code::BadState));

    SlotRecord after{};
    (void)S().slot_record(1u, after);
    check("запись слота 1 НЕ испорчена чужой подписью",
          after.state == static_cast<uint8_t>(proto::State::Verified) &&
          after.image_size == before.image_size);

    // ── 3. При require_signature пути без подписи не объявляют образ годным ──
    reset_world(wire);
    Config signed_cfg = make_config(&wire, 0u);
    signed_cfg.require_signature = true;
    signed_cfg.verify_signature  = &test_verifier;
    check("init (нужна подпись)", S().init(signed_cfg) == Status::Ok);

    F().preload(kSlotBOffset, g_image, kSize);
    check("verify_stored при требовании подписи -> PermissionDenied",
          S().verify_stored(1u, kSize, image_crc(kSize), nullptr) ==
          Status::PermissionDenied);

    Host host4(wire);
    (void)host4.info();
    proto::Verify v{};
    v.image_size  = kSize;
    v.image_crc32 = image_crc(kSize);
    v.slot        = 1u;
    check("VERIFY при требовании подписи", host4.verify(v));
    check("VERIFY -> NAK DENIED",
          host4.last_is_nak() &&
          host4.response().code == static_cast<uint8_t>(proto::Code::Denied));
    check("активировать неподписанный слот нельзя",
          S().activate(1u) != Status::Ok);

    // ── 4. Неудачный VERIFY не уничтожает годную запись ──
    reset_world(wire);
    (void)S().init(make_config(&wire, 0u));
    F().preload(kSlotBOffset, g_image, kSize);
    check("годный образ", S().verify_stored(1u, kSize, image_crc(kSize), nullptr) == Status::Ok);
    check("VERIFY с неверной CRC",
          S().verify_stored(1u, kSize, image_crc(kSize) ^ 0x55u, nullptr) == Status::Corruption);
    SlotRecord keep{};
    (void)S().slot_record(1u, keep);
    check("резервная копия осталась годной",
          keep.state == static_cast<uint8_t>(proto::State::Verified));

    // ── 5. Стирание слота снимает метку ДО стирания ──
    //
    // Иначе сбой питания посреди стирания оставил бы слот помеченным
    // «Verified», а в нём — 0xFF, и загрузчик прыгнул бы в пустоту.
    reset_world(wire);
    (void)S().init(make_config(&wire, 0u));
    F().preload(kSlotBOffset, g_image, kSize);
    (void)S().verify_stored(1u, kSize, image_crc(kSize), nullptr);

    // (а) Метку снять НЕ удалось: стирать нельзя вовсе. Пусть лучше останется
    //     старый образ с честной отметкой, чем «пустой» слот с отметкой о
    //     годности.
    F().program_fails_ = true;
    check("erase_slot при отказе записи метки", S().erase_slot(1u) != Status::Ok);
    SlotRecord kept{};
    (void)S().slot_record(1u, kept);
    check("слот НЕ стёрт (метку снять не удалось)",
          kept.state == static_cast<uint8_t>(proto::State::Verified));
    uint8_t still[16]{};
    check("данные слота на месте",
          g_storage.read(kSlotBOffset, still, sizeof still) == Status::Ok &&
          std::memcmp(still, g_image, sizeof still) == 0);
    F().program_fails_ = false;

    // (б) Метка снята, стирание отказало: слот не грузится, хотя данные целы —
    //     именно этого и требует порядок «сначала метка, потом стирание».
    F().erase_fails_ = true;
    F().erase_fails_from = kSlotBOffset;      // отказ только в области слота
    check("erase_slot при отказе стирания", S().erase_slot(1u) != Status::Ok);
    SlotRecord cleared{};
    (void)S().slot_record(1u, cleared);
    check("метка снята ДО стирания",
          cleared.state == static_cast<uint8_t>(proto::State::Idle));
    check("слот больше не активируется",
          S().activate(1u) != Status::Ok);
    F().erase_fails_ = false;

    // ── 6. Активированным остаётся ровно один слот ──
    reset_world(wire);
    (void)S().init(make_config(&wire, 0u));
    F().preload(kSlotAOffset, g_image, kSize);
    F().preload(kSlotBOffset, g_image, kSize);
    (void)S().verify_stored(0u, kSize, image_crc(kSize), nullptr);
    (void)S().verify_stored(1u, kSize, image_crc(kSize), nullptr);
    check("активация слота 1", S().activate(1u) == Status::Ok);
    check("повторная активация того же слота -> AlreadyExists",
          S().activate(1u) == Status::AlreadyExists);

    // Активируем ДРУГОЙ слот (исполняемым теперь считаем слот 1): прежний
    // кандидат обязан перестать быть активированным, иначе «что грузить»
    // решает порядок индексов, а не история.
    power_cycle();
    check("init (активен слот 1)", S().init(make_config(&wire, 1u)) == Status::Ok);
    check("активация слота 0", S().activate(0u) == Status::Ok);
    SlotRecord r0{}, r1{};
    (void)S().slot_record(0u, r0);
    (void)S().slot_record(1u, r1);
    check("слот 0 активирован",
          r0.state == static_cast<uint8_t>(proto::State::Activated));
    check("слот 1 больше не активирован",
          r1.state != static_cast<uint8_t>(proto::State::Activated) &&
          r1.verified());

    // ── 7. Подтверждение загрузки при active_slot = kSlotNone ──
    //
    // Режим «какой слот грузить, решает загрузчик» документирован: boot_check
    // находит активированный слот, значит и confirm_running обязан его найти.
    reset_world(wire);
    Config auto_cfg = make_config(&wire, 1u);
    (void)S().init(auto_cfg);
    F().preload(kSlotAOffset, g_image, kSize);
    (void)S().verify_stored(0u, kSize, image_crc(kSize), nullptr);
    (void)S().activate(0u);

    Config no_active = make_config(&wire, ota::kSlotNone);
    check("init без active_slot", S().init(no_active) == Status::Ok);
    BootInfo bi{};
    check("boot_check нашёл активированный слот",
          S().boot_check(bi) == Status::Ok && bi.valid && bi.slot == 0u);
    check("подтверждение работает без active_slot",
          S().confirm_running() == Status::Ok);
    SlotRecord confirmed{};
    (void)S().slot_record(0u, confirmed);
    check("слот подтверждён",
          confirmed.state == static_cast<uint8_t>(proto::State::Running));

    // ── 8. Откат не возвращается к слоту, провалившему пробные загрузки ──
    reset_world(wire);
    Config rb = make_config(&wire, 0u);
    rb.max_boot_attempts = 1u;
    (void)S().init(rb);
    F().preload(kSlotBOffset, g_image, kSize);
    (void)S().verify_stored(1u, kSize, image_crc(kSize), nullptr);
    (void)S().activate(1u);

    Config rb2 = make_config(&wire, 1u);
    rb2.max_boot_attempts = 1u;
    (void)S().init(rb2);
    for (uint32_t i = 0u; i < 3u; ++i) {
        power_cycle();
        (void)S().init(rb2);
        (void)S().boot_check(bi);              // слот 1 «не подтвердил» себя
    }
    // В слоте 0 годного образа нет → откатываться некуда, и модуль обязан
    // снять активацию со сломанного слота, а не активировать его снова.
    check("откат без годного слота", S().rollback() == Status::NotFound);
    SlotRecord broken{};
    (void)S().slot_record(1u, broken);
    check("сломанный слот деактивирован",
          broken.state == static_cast<uint8_t>(proto::State::Failed));

    // ── 9. Зарезервированный флаг START отвергается, а не игнорируется ──
    reset_world(wire);
    (void)S().init(make_config(&wire, 0u));
    Host host5(wire);
    (void)host5.info();
    proto::Start vs = s;
    vs.slot  = proto::kSlotAuto;
    vs.flags = proto::kStartVerifyOnly;
    check("START с kStartVerifyOnly", host5.start(vs));
    check("нереализованный флаг -> NAK UNSUPPORTED",
          host5.last_is_nak() &&
          host5.response().code == static_cast<uint8_t>(proto::Code::Unsupported));

    (void)st0;
}

// ── 16. Выравнивание записи: нечётные образы и нечётные блоки ─────────────
//
// Встроенная Flash пишет только half-word, а дописывать половину уже
// запрограммированной пары нельзя. Значит, «последний байт» и «блок нечётной
// длины» — не мелочь, а путь, на котором прошивка либо теряет байт, либо
// пытается перепрограммировать ячейку. Модель носителя здесь СТРОГАЯ: она
// отвергает невыровненные записи, как настоящее железо, поэтому тест ловит
// ошибку выравнивания, а не прощает её.

void test_alignment() {
    (void)std::puts("выравнивание записи: нечётный образ и нечётные блоки:");
    const StatsBase st0;

    MemoryWire& wire = g_wire;
    reset_world(wire);
    check("init", S().init(make_config(&wire, 0u)) == Status::Ok);

    // 1023 байта — образ НЕЧЁТНОЙ длины; блоки по 123 (тоже нечётные).
    constexpr uint32_t kOddSize = 1023u;
    make_image(kOddSize, 21u);
    const uint32_t crc = image_crc(kOddSize);

    Host host(wire);
    (void)host.info();

    proto::Start s{};
    s.image_size  = kOddSize;
    s.image_crc32 = crc;
    s.fw_version  = 2u;
    check("START нечётного образа", host.start(s));
    check("START -> ACK", host.last_is_ack());

    const uint32_t sent = host.send_image(kOddSize, 123u);
    check_eq<uint32_t>("передан нечётный образ", sent, kOddSize);

    check("END", host.end(kOddSize, crc, nullptr));
    check("END -> ACK", host.last_is_ack());
    check_eq<uint32_t>("состояние VERIFIED",
                       host.response().state, static_cast<uint8_t>(proto::State::Verified));

    // Байты образа в носителе — те самые (включая последний, нечётный).
    uint8_t back[64]{};
    check("начало образа совпало",
          g_storage.read(kSlotBOffset, back, sizeof back) == Status::Ok &&
          std::memcmp(back, g_image, sizeof back) == 0);
    check("последний байт образа на месте",
          g_storage.read(kSlotBOffset + kOddSize - 1u, back, 1u) == Status::Ok &&
          back[0] == g_image[kOddSize - 1u]);
    // Добитый байт пары лежит ЗА образом и не портит ничего: данные образа
    // читаются ровно столько, сколько заявлено, и в записи стоит размер образа.
    SlotRecord rec{};
    check("образ помечен годным",
          S().slot_record(1u, rec) == Status::Ok && rec.verified());
    check_eq<uint32_t>("размер образа в записи", rec.image_size, kOddSize);

    // ── Продолжение с НЕЧЁТНЫМ префиксом ──
    reset_world(wire);
    (void)S().init(make_config(&wire, 0u));
    Host host2(wire);
    (void)host2.info();
    check("START", host2.start(s));
    const uint32_t part = host2.send_image(kOddSize, 123u, 246u);
    check("передана часть", part >= 246u);
    power_cycle();
    check("init после сбоя", S().init(make_config(&wire, 0u)) == Status::Ok);

    Host host3(wire);
    (void)host3.info();
    proto::Start r = s;
    r.flags = proto::kStartResume;
    check("START + Resume", host3.start(r));
    check("Resume -> ACK", host3.last_is_ack());
    const uint32_t from = host3.response().next_offset;
    check("продолжение не дальше отправленного", from <= part);
    (void)host3.send_image(kOddSize, 123u, 0u, from);
    check("END после продолжения", host3.end(kOddSize, crc, nullptr));
    check("END -> ACK", host3.last_is_ack());
    check("образ в носителе цел",
          g_storage.read(kSlotBOffset, back, 32u) == Status::Ok &&
          std::memcmp(back, g_image, 32u) == 0);

    (void)st0;
}

// ──────────────────────────── Запуск тестов ────────────────────────────────
//
// Без аргументов прогоняются все сцены. С аргументом — только те, в имени
// которых он встречается: так удобно разбирать одну сцену, не прогоняя
// остальные (и не теряя при этом изоляцию: каждая сцена сама сбрасывает мир).
struct Scene {
    const char* name;
    void (*run)();
};

const Scene kScenes[] = {
    {"info",      &test_info_and_layout},
    {"happy",     &test_happy_path},
    {"repeats",   &test_repeats_and_gaps},
    {"verify",    &test_verify_failures},
    {"resume",    &test_resume_after_power_loss},
    {"hash",      &test_hash_signature_policy},
    {"boot",      &test_activation_and_rollback},
    {"policy",    &test_policy},
    {"noline",    &test_no_transport},
    {"uart",      &test_uart_transport},
    {"extrom",    &test_ext_rom_storage},
    {"misc",      &test_misc_ops},
    {"timeout",   &test_session_timeout},
    {"decode",    &test_decoder_robustness},
    {"regress",   &test_regressions},
    {"align",     &test_alignment},
};

bool name_matches(const char* name, const char* filter) {
    if (filter == nullptr) return true;
    const Size n = std::strlen(filter);
    const Size m = std::strlen(name);
    if (n > m) return false;
    for (Size i = 0u; i + n <= m; ++i) {
        if (std::strncmp(name + i, filter, n) == 0) return true;
    }
    return false;
}

// ───────────── 13. Обрыв связи: сессия истекает, но передача продолжается ───
//
// Хост пропал (кабель выдернули, питание у сервера) — это НЕ ошибка образа.
// Сессия закрывается по простою, а записанное остаётся, и следующий START с
// флагом Resume продолжает с проверенного места. Здесь для этого нужен
// идущий таймер ОС: только он даёт «сколько прошло с последнего кадра».


} // namespace

int main(int argc, char** argv) {
    (void)std::puts("=== OTA: приём прошивки ===");
    const char* filter = (argc > 1) ? argv[1] : nullptr;

    // Время ядра: сервис использует его для таймаутов, а тест продвигает тик
    // сам (как и остальные тесты ядра). Тик НЕ запускаем: тогда TimeAvailable()
    // == false и ограничение частоты NAK на испорченные кадры не мешает.
    config::OsTimer::Config tcfg{};
    tcfg.min_tick_ms = 1; tcfg.max_tick_ms = 1000;
    tcfg.default_tick_ms = 1; tcfg.alarm_tick_ms = 1000; tcfg.max_catchup = 3;
    if (config::OsTimer::Init(tcfg) != Status::Ok) {
        (void)std::puts("  [FAIL] инициализация времени");
        return 1;
    }
    power::Config pcfg{};
    pcfg.allow_deep_sleep = false;
    (void)power::Supervisor::instance().init(pcfg);

    g_tick = &device_tick;
    g_storage.set_window(0u, 0u);   // окно = вся встроенная Flash (128 КБ)

    for (const Scene& sc : kScenes) {
        if (name_matches(sc.name, filter)) sc.run();
    }

    S().shutdown();

    (void)std::printf("\nвсего проверок: %d, провалов: %d\n", g_total, g_failed);
    // Слово FAIL печатается ТОЛЬКО при провале: ctest ищет его в выводе.
    if (g_failed != 0) (void)std::puts("FAIL");
    return (g_failed == 0) ? 0 : 1;
}
