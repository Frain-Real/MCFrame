// os/ota/ota.cpp
// ---------------------------------------------------------------------------
// Логика приёма прошивки: сессия, запись в носитель, проверка, активация.
//
// ПОРЯДОК РАЗБОРА ФАЙЛА
//   1. init и проверка настроек;
//   2. приём (poll): чтение интерфейса -> декодер -> обработчик кадра;
//   3. обработчики операций протокола;
//   4. сессия: открытие/продолжение/проверка/запись;
//   5. работа с метаданными и носителем;
//   6. загрузка: boot_check/confirm/activate/rollback.
//
// ЧТО ЗДЕСЬ ГЛАВНОЕ
// Ни один шаг не «доверяет» другому на слово. Записанное перечитывается
// (verify_readback), префикс при продолжении сверяется (verify_resume_prefix),
// а состояние слота пишется в носитель ДО того, как о нём будет объявлено
// хосту: если питание пропадёт между ответом и записью состояния, устройство
// забудет то, что уже подтвердило, — и хост, повторив START, продолжит с
// меньшего смещения. Обратный порядок дал бы дыру в образе.
// ---------------------------------------------------------------------------
#include "ota.hpp"
#include "../log/log.hpp"

#include "../async/async_kernel.hpp"   // yield при пустом чтении интерфейса

namespace embed::ota {

namespace {

/// Признак «нужна подпись» в detail ответа (хост понимает, чего ждёт устройство).
constexpr uint16_t kDetailSignatureRequired = 1u;

/// Тег подписываемого сообщения. Защищает от подписи «похожего» сообщения из
/// другого протокола (та же длина, другие поля).
constexpr char kSignTag[8] = {'M', 'C', 'F', 'R', 'A', 'M', 'E', 'O'};

/// Размер сообщения, которое подписывается: 8 + 4 + 4 + 4 + 32.
constexpr Size kSignMessageSize = 8u + 4u + 4u + 4u + 32u;

/// Собрать подписываемое сообщение.
Size build_sign_message(const SlotRecord& rec, uint8_t* out) noexcept {
    for (Size i = 0u; i < 8u; ++i) out[i] = static_cast<uint8_t>(kSignTag[i]);
    proto::put_u32(out + 8u, rec.image_size);
    proto::put_u32(out + 12u, rec.image_crc32);
    proto::put_u32(out + 16u, rec.fw_version);
    for (Size i = 0u; i < 32u; ++i) out[20u + i] = rec.sha256[i];
    return kSignMessageSize;
}

/// Сравнение двух буферов постоянным временем.
///
/// Да, для «просто CRC/хеша» это выглядит избыточным, но сравнение подписи и
/// дайджеста — ровно тот случай, где обычное сравнение с выходом на первом
/// различии отдаёт наружу информацию о совпавшем префиксе. Стоит копейки.
bool bytes_equal(const uint8_t* a, const uint8_t* b, Size len) noexcept {
    uint8_t diff = 0u;
    for (Size i = 0u; i < len; ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0u;
}

/// Округление вверх до кратного page.
uint32_t align_up(uint32_t value, uint32_t page) noexcept {
    if (page <= 1u) return value;
    const uint32_t rem = value % page;
    return (rem == 0u) ? value : (value + (page - rem));
}

} // namespace

// ────────────────────────────── Инициализация ──────────────────────────────

void Service::shutdown() noexcept {
    session_open_ = false;
    hold_awake(false);
    // Носитель и интерфейс закрываются ЯВНО: у встроенной Flash это взведение
    // защиты от записи (CR.LOCK), у внешней памяти — её деинициализация.
    // Без этого FPEC остаётся разблокированным на всё время работы, и первая
    // же случайная запись по адресу регистров что-нибудь сотрёт.
    if (storage_ != nullptr) storage_->end();
    if (transport_ != nullptr && transport_ != &null_transport_) transport_->end();
    storage_   = nullptr;
    transport_ = nullptr;
    // Записи состояния тоже отвязываются: иначе slot_record() отвечал бы из
    // кэша метаданных, то есть отдавал бы устаревшие сведения как актуальные.
    slots_ = SlotStore{};
    reset_session_state();
    inited_    = false;
}

void Service::reset_session_state() noexcept {
    slot_            = kSlotNone;
    written_         = 0u;
    crc_written_     = proto::image_crc_begin();
    pending_len_     = 0u;
    verified_slot_   = kSlotNone;
    boot_slot_       = kSlotNone;
    state_           = proto::State::Idle;
    image_size_      = 0u;
    expect_crc_      = 0u;
    fw_version_      = 0u;
    next_offset_     = 0u;
    erased_upto_     = 0u;
    next_checkpoint_ = 0u;
    chunk_max_       = 0u;
    hash_kind_       = proto::HashKind::None;
    crc_             = proto::image_crc_begin();
    sha_.reset();
    signature_len_   = 0u;
    signature_kind_  = 0u;
    for (Size i = 0u; i < 32u; ++i) sha_expected_[i] = 0u;
}

Status Service::init(const Config& cfg) noexcept {
    if (inited_) shutdown();
    if (cfg.storage == nullptr) return Status::InvalidArg;

    cfg_ = cfg;
    storage_ = cfg_.storage;      // shutdown() закрывает носитель через это поле

    // Состояние сессии — с чистого листа: иначе кадр, пришедший вне сессии,
    // опирался бы на слот/размеры/CRC ПРЕДЫДУЩЕЙ передачи.
    reset_session_state();

    const uint32_t page = cfg_.storage->page_size();
    if (page == 0u) return Status::NotSupported;   // метаданным нужны страницы

    // Раскладка: метаданные не пересекаются со слотами, образ влезает в слот.
    const LayoutCheck lc = check_layout(*cfg_.storage, cfg_.meta_offset,
                                       cfg_.slot_offset, cfg_.slot_capacity);
    if (!lc.ok) return lc.status;
    page_        = page;
    image_limit_ = lc.image_limit;

    // Метаданные каждого слота — свои две страницы: иначе запись состояния
    // одного слота затирала бы состояние другого (а работающая и новая
    // прошивки описываются одновременно).
    // Записи состояния ведёт общий SlotStore: те же правила читает и
    // загрузчик, поэтому «кто кого грузит» и «сколько раз пытались» не может
    // разойтись между двумя образами (см. ota_storage.hpp).
    Status st = slots_.init(cfg_.storage, cfg_.meta_offset);
    if (st != Status::Ok) return st;

    // Интерфейс приёма: nullptr означает «интерфейса нет» — это законный
    // режим (образ лежит в носителе), и он должен быть явным.
    transport_ = (cfg_.transport != nullptr) ? cfg_.transport : &null_transport_;

    // Предел полезной нагрузки кадра — МИНИМУМ из трёх: настройки приложения,
    // возможностей интерфейса и того, что влезает в буфер приёмника.
    // Минимум, а не «настройка важнее»: кадр, который интерфейс не может
    // передать (или приёмник — сохранить), не станет возможным от того, что
    // приложение написало большее число. Декодер ниже урежет предел ещё раз,
    // если буфер окажется меньше запрошенного.
    uint16_t want = proto::kMaxPayload;
    const uint16_t transport_max = transport_->max_payload();
    if (transport_max != 0u && transport_max < want) want = transport_max;
    if (cfg_.max_payload != 0u && cfg_.max_payload < want) want = cfg_.max_payload;

    // Нижняя граница: ответ (Response) обязан влезать в кадр, иначе устройство
    // станет «немым» — не сможет отправить даже отказ.
    if (want < proto::kResponseSize) return Status::InvalidArg;

    // Грануля программирования носителя: сервис обязан писать кратными
    // порциями (см. stage_bytes). Больше 8 байт — это уже не NOR-flash, а
    // что-то, чего модуль не моделирует.
    const uint32_t gran = cfg_.storage->write_granularity();
    if (gran == 0u || gran > sizeof(pending_)) return Status::NotSupported;
    write_gran_ = static_cast<uint8_t>(gran);

    decoder_.init(rx_frame_, sizeof(rx_frame_), want);
    max_payload_ = decoder_.max_payload();
    if (max_payload_ < proto::kResponseSize) return Status::InvalidArg;

    // Счётчики декодера при init() обнуляются, поэтому и «уже учтённое»
    // сервисом надо обнулить: иначе первая же ошибка после перенастройки
    // провалила бы вычитание и история потерялась бы.
    skipped_seen_ = 0u;
    faults_seen_  = 0u;

    st = cfg_.storage->begin();
    if (st != Status::Ok && st != Status::NotSupported) return st;

    st = transport_->begin();
    if (st != Status::Ok && st != Status::NotSupported && st != Status::Unavailable) {
        return st;
    }

    // Состояние: если в активном слоте лежит незавершённая запись, о ней надо
    // знать (её можно продолжить), но сессии при этом нет.
    state_ = proto::State::Idle;
    if (cfg_.active_slot != kSlotNone) {
        SlotRecord rec{};
        if (read_slot_record(cfg_.active_slot, rec) == Status::Ok) {
            state_ = static_cast<proto::State>(rec.state);
        }
    }

    last_frame_ms_ = now_ms();
    inited_ = true;
    log::write(log::Level::Info, "OTA", "init ok");
    return Status::Ok;
}

proto::Iface Service::iface() const noexcept {
    return (transport_ != nullptr) ? transport_->iface() : proto::Iface::None;
}

uint64_t Service::now_ms() const noexcept {
    return config::OsTimer::TimeAvailable() ? config::OsTimer::NowMs() : 0u;
}

// ──────────────────────────── Интеграция со средой ─────────────────────────

void Service::hold_awake(bool on) noexcept {
    if (!cfg_.keep_awake) return;
    if (on && !keep_awake_held_) {
        lease_ = power::Supervisor::instance().keep_awake();
        keep_awake_held_ = true;
    } else if (!on && keep_awake_held_) {
        lease_.release();
        keep_awake_held_ = false;
    }
}

void Service::stall_begin() noexcept {
    if (!cfg_.use_watchdog_stall || cfg_.erase_stall_ms == 0u) return;
    if (watchdog::Service::instance().allow_stall(cfg_.erase_stall_ms) == Status::Ok) {
        stall_open_ = true;
    }
}

void Service::stall_end() noexcept {
    // Окно закрывается само (оно ограничено), поэтому здесь только отметка:
    // закрывать его принудительно нельзя — сторож не предоставляет отмены.
    stall_open_ = false;
}

void Service::emit(Event ev) noexcept {
    last_event_ = ev;
    if (cfg_.on_event != nullptr) cfg_.on_event(cfg_.event_arg);
}

void Service::note_reset(uint8_t slot, uint32_t attempts, uint32_t state) noexcept {
    const platform::WatchdogOps* wd = platform::ops().watchdog;
    if (wd == nullptr || wd->note_write == nullptr) return;
    (void)wd->note_write(cfg_.note_base + kNoteIdxMagic, kNoteMagic);
    (void)wd->note_write(cfg_.note_base + kNoteIdxState,
                         static_cast<uint16_t>(state & 0xFFFFu));
    (void)wd->note_write(cfg_.note_base + kNoteIdxAttempts,
                         static_cast<uint16_t>(attempts & 0xFFFFu));
    (void)wd->note_write(cfg_.note_base + kNoteIdxSlot,
                         static_cast<uint16_t>(slot == kSlotNone ? 0xFFu : slot));
}

// ───────────────────────────────── Приём ───────────────────────────────────

Status Service::poll(uint32_t budget_ms) noexcept {
    if (!inited_) return Status::Unavailable;
    return serve(budget_ms);
}

Status Service::serve(uint32_t budget_ms) noexcept {
    const bool timed = config::OsTimer::TimeAvailable();
    const uint64_t start = now_ms();
    bool did = false;

    for (;;) {
        // 1. Доразобрать то, что уже лежит в буфере декодера: несколько кадров
        //    могли прийти одним куском интерфейса.
        proto::Frame frame{};
        while (decoder_.resume(frame)) {
            ++stats_.frames;
            handle(frame);
            did = true;
        }

        // 2. Принять порцию с интерфейса.
        Size got = 0u;
        const Status st = transport_->read(rx_chunk_, sizeof(rx_chunk_), got,
                                          cfg_.frame_timeout_ms);
        if (st == Status::Ok) {
            if (got > 0u) {
                bool got_frame = false;
                on_bytes(rx_chunk_, got, got_frame);
                did = did || got_frame;
            } else {
                // Неблокирующий драйвер: данных нет, но и ошибки нет.
                async::kernel::yield();
            }
        } else if (st == Status::Timeout) {
            on_idle();
            return did ? Status::Ok : Status::Timeout;
        } else {
            ++stats_.transport_errors;
            return st;
        }

        if (budget_ms == 0u && did) return Status::Ok;
        if (budget_ms != 0u && timed && (now_ms() - start) >= budget_ms) {
            return did ? Status::Ok : Status::Timeout;
        }
        if (!timed) return did ? Status::Ok : Status::Timeout;
    }
}

void Service::on_bytes(const uint8_t* data, Size len, bool& got_frame) noexcept {
    Size pos = 0u;
    while (pos < len) {
        proto::Frame frame{};
        const proto::Decoder::Step step = decoder_.feed(data + pos, len - pos, frame);
        pos += step.consumed;
        if (!step.ready) break;      // кадр не собран: остальное — следующий раз
        ++stats_.frames;
        handle(frame);
        got_frame = true;
    }

    // Потерянные кадры и байты: счётчики декодера накопительные, поэтому
    // разницу учитываем один раз.
    const uint32_t skipped = decoder_.skipped_bytes();
    if (skipped > skipped_seen_) {
        stats_.resync_bytes += (skipped - skipped_seen_);
        skipped_seen_ = skipped;
    }
    const uint32_t faults = decoder_.fault_count();
    if (faults != faults_seen_) {
        stats_.bad_frames += (faults - faults_seen_);
        faults_seen_ = faults;
        // Отвечаем на испорченный кадр не чаще, чем раз в
        // bad_frame_nak_period_ms: на плохой линии ответ на каждый мусорный
        // байт только удваивает трафик и мешает хосту найти синхронизацию.
        const uint64_t now = now_ms();
        const bool can = (cfg_.bad_frame_nak_period_ms == 0u) ||
                         (!config::OsTimer::TimeAvailable()) ||
                         ((now - last_bad_nak_ms_) >= cfg_.bad_frame_nak_period_ms);
        if (can) {
            last_bad_nak_ms_ = now;
            ++stats_.naks;
            proto::Response r{};
            make_response(r);
            r.code   = static_cast<uint8_t>(proto::Code::BadFrame);
            r.detail = static_cast<uint16_t>(decoder_.last_fault());
            reply(proto::Op::Nak, r, nullptr, 0u);
        }
    }
}

/// Простой интерфейса: следим за сроком сессии.
void Service::on_idle() noexcept {
    if (!session_open_) return;
    if (cfg_.session_timeout_ms == 0u) return;
    if (!config::OsTimer::TimeAvailable()) return;
    if ((now_ms() - last_frame_ms_) < cfg_.session_timeout_ms) return;

    close_session(true);
}

void Service::handle(const proto::Frame& f) noexcept {
    last_frame_ms_ = now_ms();

    if (f.too_large) {
        // Кадр целый по CRC, но полезная нагрузка не влезла: честно сообщаем
        // предел, который примем, — хост уменьшит блок и повторит.
        ++stats_.too_large;
        ++stats_.naks;
        proto::Response r{};
        make_response(r);
        r.code   = static_cast<uint8_t>(proto::Code::TooLarge);
        r.detail = max_block();   // предел БЛОКА данных, а не кадра
        reply(proto::Op::Nak, r, nullptr, 0u);
        return;
    }

    switch (f.op) {
        case proto::Op::Info:     on_info(f);     break;
        case proto::Op::Start:    on_start(f);    break;
        case proto::Op::Data:     on_data(f);     break;
        case proto::Op::End:      on_end(f);      break;
        case proto::Op::Sign:     on_sign(f);     break;
        case proto::Op::Abort:    on_abort(f);    break;
        case proto::Op::Erase:    on_erase(f);    break;
        case proto::Op::Verify:   on_verify(f);   break;
        case proto::Op::Activate: on_activate(f); break;
        case proto::Op::Confirm:  on_confirm(f);  break;
        case proto::Op::State:    on_state(f);    break;
        case proto::Op::Read:     on_read(f);     break;
        case proto::Op::Ack:
        case proto::Op::Nak:
            // Ответы приходят только хосту: если устройство получило ответ,
            // значит на линии кто-то ещё (или эхо). Молча игнорируем.
            break;
        default:
            // Неизвестная операция: отвечаем, а не молчим. Молчание хост
            // истолкует как потерю кадра и будет повторять её вечно.
            reply_nak(proto::Code::Unsupported, static_cast<uint16_t>(f.op));
            break;
    }
}

// ──────────────────────────────── Ответы ───────────────────────────────────

void Service::make_response(proto::Response& r) const noexcept {
    r = proto::Response{};
    r.code        = static_cast<uint8_t>(proto::Code::Ok);
    r.state       = static_cast<uint8_t>(state_);
    r.max_payload = max_payload_;
    r.next_offset = next_offset_;
    r.fw_version  = fw_version_;
    if (slot_ != kSlotNone) {
        r.slot          = slot_;
        r.slot_offset   = cfg_.slot_offset[slot_];
        r.slot_capacity = cfg_.slot_capacity[slot_];
        r.image_crc32   = proto::image_crc_finish(crc_);
    }
}

void Service::reply(const proto::Op op, const proto::Response& r,
                    const void* extra, Size extra_len) noexcept {
    if (transport_ == nullptr) return;

    Size len = proto::kResponseSize + extra_len;
    if (len > max_payload_) {
        // Ответ не влезает: это возможно только для OpRead, где данные идут
        // вслед за Response. Отвечаем максимумом, который влезает.
        if (extra_len == 0u) return;
        len = max_payload_;
        extra_len = len - proto::kResponseSize;
    }

    proto::encode_response(tx_frame_ + proto::kHeaderInFrame, r);
    if (extra_len > 0u && extra != nullptr) {
        const auto* src = static_cast<const uint8_t*>(extra);
        for (Size i = 0u; i < extra_len; ++i) {
            tx_frame_[proto::kHeaderInFrame + proto::kResponseSize + i] = src[i];
        }
    }
    send_frame(op, len);
}

void Service::reply_ack(const proto::Response& r, const void* extra, Size extra_len) noexcept {
    reply(proto::Op::Ack, r, extra, extra_len);
}

void Service::reply_nak(proto::Code code, uint16_t detail) noexcept {
    ++stats_.naks;
    proto::Response r{};
    make_response(r);
    r.code   = static_cast<uint8_t>(code);
    r.detail = detail;
    reply(proto::Op::Nak, r, nullptr, 0u);
}

/// Завершить и отправить кадр, полезная нагрузка которого УЖЕ лежит в
/// tx_frame_ (см. reply и on_read: так данные читаются из носителя сразу в
/// буфер кадра, минуя стек).
void Service::send_frame(proto::Op op, Size payload_len) noexcept {
    const Size total = proto::frame_reserve(tx_frame_, sizeof(tx_frame_), op, 0u,
                                           tx_seq_, payload_len);
    if (total == 0u) return;
    proto::frame_seal(tx_frame_, payload_len);
    ++tx_seq_;
    (void)transport_->write(tx_frame_, total, cfg_.frame_timeout_ms);
}

// ───────────────────────── Обработчики операций ────────────────────────────

void Service::on_info(const proto::Frame& f) noexcept {
    // Хост может прислать свою версию протокола: несовместимую отвергаем
    // явно, чтобы он не гадал, почему устройство «странно себя ведёт».
    if (f.len >= proto::kInfoReqSize && f.payload != nullptr && f.payload[0] != proto::kVersion) {
        reply_nak(proto::Code::Unsupported, f.payload[0]);
        return;
    }

    proto::Info info{};
    info.proto_version = proto::kVersion;
    info.iface         = static_cast<uint8_t>(iface());
    info.max_payload   = max_payload_;
    info.page_size     = static_cast<uint16_t>(page_ > 0xFFFFu ? 0xFFFFu : page_);
    info.fw_version    = cfg_.fw_version;
    info.image_limit   = image_limit_;

    uint8_t caps = proto::kCapSha256 | proto::kCapResume | proto::kCapVerify;
    if (cfg_.verify_readback)     caps |= proto::kCapReadback;
    if (cfg_.verify_signature != nullptr) caps |= proto::kCapSign;
    if (cfg_.slot_capacity[0] != 0u && cfg_.slot_capacity[1] != 0u) caps |= proto::kCapDualSlot;
    info.caps = caps;

    for (uint8_t s = 0u; s < 2u; ++s) {
        info.slot_offset[s]   = cfg_.slot_offset[s];
        info.slot_capacity[s] = cfg_.slot_capacity[s];
    }

    // Ответ на INFO — не Response, а Info: хосту нужны возможности, а не
    // «принято/не принято». Это единственное исключение, и оно описано в
    // спецификации протокола.
    uint8_t payload[proto::kInfoSize];
    proto::encode_info(payload, info);
    if (proto::kInfoSize > max_payload_) {
        reply_nak(proto::Code::TooLarge, max_payload_);
        return;
    }
    for (Size i = 0u; i < proto::kInfoSize; ++i) {
        tx_frame_[proto::kHeaderInFrame + i] = payload[i];
    }
    send_frame(proto::Op::Ack, proto::kInfoSize);
}

void Service::on_start(const proto::Frame& f) noexcept {
    proto::Start start{};
    if (!proto::decode_start(f.payload, f.len, start)) {
        reply_nak(proto::Code::BadRequest, proto::kStartSize);
        return;
    }

    // Флаг «только проверить, не писать» не реализован: молча открыть
    // обычную сессию (со стиранием слота) было бы худшим ответом, чем отказ.
    if ((start.flags & proto::kStartVerifyOnly) != 0u) {
        reply_nak(proto::Code::Unsupported, start.flags);
        return;
    }

    proto::Response r{};
    bool resumed = false;
    const Status st = open_session(start, r, resumed);
    if (st != Status::Ok) {
        // Причина отказа разложена по коду протокола вызывающим. Если он её не
        // проставил (ветка с ошибкой носителя), подставляем Storage: NAK с
        // code = Ok хост прочитал бы как «сессия открыта» и продолжил слать
        // DATA, которых никто не ждёт.
        proto::Code code = static_cast<proto::Code>(r.code);
        if (code == proto::Code::Ok) code = proto::Code::Storage;
        reply_nak(code, r.detail);
        return;
    }
    reply_ack(r);
    emit(resumed ? Event::SessionResumed : Event::SessionStarted);
}

void Service::on_data(const proto::Frame& f) noexcept {
    // Предел БЛОКА: полезная нагрузка кадра DATA = 4 байта смещения + данные,
    // поэтому блок не может быть больше max_payload_ - 4. Именно это число
    // уходит хосту в detail при отказе TooLarge — иначе хост повторил бы блок
    // того же размера и получил бы тот же отказ.

    if (!session_open_ || !proto::state_accepts_data(state_)) {
        reply_nak(proto::Code::BadState, static_cast<uint16_t>(state_));
        return;
    }
    if (f.len < proto::kDataHeaderSize || f.payload == nullptr) {
        reply_nak(proto::Code::BadRequest, proto::kDataHeaderSize);
        return;
    }

    const uint32_t offset = proto::get_u32(f.payload);
    const uint8_t* data = f.payload + proto::kDataHeaderSize;
    const Size n = static_cast<Size>(f.len) - proto::kDataHeaderSize;

    if (n == 0u) {
        reply_nak(proto::Code::BadRequest, 0u);
        return;
    }
    if (n > chunk_max_) {
        reply_nak(proto::Code::TooLarge, chunk_max_);
        return;
    }
    if (offset > image_size_ || (offset + n) > image_size_) {
        reply_nak(proto::Code::BadOffset, static_cast<uint16_t>(next_offset_));
        return;
    }

    if (offset < next_offset_) {
        // Повтор: либо кадр был принят, а ответ потерян (offset + n <=
        // next_offset — данные уже лежат), либо хост отстал сильнее, чем на
        // блок. Во втором случае писать «внахлёст» нельзя: на NOR-flash
        // записать уже записанное место невозможно, поэтому просим хост
        // вернуться к next_offset.
        if ((offset + n) <= next_offset_) {
            ++stats_.duplicates;
            proto::Response r{};
            make_response(r);
            reply_ack(r);
            return;
        }
        ++stats_.gaps;
        reply_nak(proto::Code::BadOffset, static_cast<uint16_t>(next_offset_ & 0xFFFFu));
        return;
    }

    if (offset > next_offset_) {
        // Разрыв: часть кадров не дошла. Данные НЕ пропускаем и не пишем —
        // иначе в образе осталась бы дыра, а CRC сошлась бы «на другом».
        ++stats_.gaps;
        proto::Response r{};
        make_response(r);
        r.code   = static_cast<uint8_t>(proto::Code::BadOffset);
        r.detail = static_cast<uint16_t>(next_offset_ & 0xFFFFu);
        ++stats_.naks;
        reply(proto::Op::Nak, r, nullptr, 0u);
        return;
    }

    // Штатный путь: offset == next_offset.
    const Status wst = write_image(next_offset_, data, n);
    if (wst != Status::Ok) {
        fail_session(proto::Code::Storage, static_cast<uint16_t>(wst), slot_);
        return;
    }

    crc_ = proto::image_crc_update(crc_, data, n);
    if (hash_kind_ == proto::HashKind::Sha256) sha_.update(data, n);
    next_offset_ += static_cast<uint32_t>(n);
    // Перехода Started -> Receiving достаточно одного записанного байта:
    // состояние уходит хосту в ответе и в записи слота, и «Started» после
    // записи данных означало бы, что слот пуст.
    if (next_offset_ >= image_size_)      state_ = proto::State::Complete;
    else if (state_ == proto::State::Started) state_ = proto::State::Receiving;

    // Состояние — ДО ответа: если ответ уйдёт, а питание пропадёт сразу после,
    // хост продолжит с того, что действительно записано.
    const Status cst = checkpoint(false);
    if (cst != Status::Ok) {
        fail_session(proto::Code::Storage, static_cast<uint16_t>(cst), slot_);
        return;
    }

    proto::Response r{};
    make_response(r);
    reply_ack(r);
}

void Service::on_end(const proto::Frame& f) noexcept {
    if (!session_open_) {
        reply_nak(proto::Code::BadState, static_cast<uint16_t>(state_));
        return;
    }
    proto::End end{};
    if (!proto::decode_end(f.payload, f.len, end)) {
        reply_nak(proto::Code::BadRequest, proto::kEndSize);
        return;
    }
    if (end.image_size != image_size_) {
        reply_nak(proto::Code::BadRequest, 0u);
        return;
    }
    if (next_offset_ != image_size_) {
        // Образ принят не весь: говорим, откуда продолжать.
        proto::Response r{};
        make_response(r);
        r.code   = static_cast<uint8_t>(proto::Code::BadOffset);
        r.detail = static_cast<uint16_t>(next_offset_ & 0xFFFFu);
        ++stats_.naks;
        reply(proto::Op::Nak, r, nullptr, 0u);
        return;
    }

    // Хвост образа дописываем ДО проверок: проверка чтением читает носитель,
    // и в нём обязано лежать всё принятое.
    const Status fst = flush_pending();
    if (fst != Status::Ok) {
        fail_session(proto::Code::Storage, static_cast<uint16_t>(fst), slot_);
        return;
    }

    // 1. Целостность по подсчёту во время приёма.
    const uint32_t crc_calc = proto::image_crc_finish(crc_);
    if (crc_calc != end.image_crc32 || end.image_crc32 != expect_crc_) {
        ++stats_.verify_fail;
        fail_session(proto::Code::Verify, 0u, slot_);
        return;
    }

    // 2. Хеш, если он заявлен и ожидался.
    uint8_t digest[32]{};
    if (hash_kind_ == proto::HashKind::Sha256) {
        sha_.finalize(digest);
        if (!bytes_equal(digest, end.sha256, 32u) || !bytes_equal(digest, sha_expected_, 32u)) {
            ++stats_.verify_fail;
            log::write(log::Level::Error, "OTA", "image hash mismatch");
            fail_session(proto::Code::Verify, 1u, slot_);
            return;
        }
    }

    // 3. Проверка ПРОЧИТАННОГО из носителя: «запись принята» и «в памяти лежит
    //    то, что писали» — разные утверждения, и второе проверяется чтением.
    uint32_t flags = kRecFlagCrcOk;
    if (hash_kind_ == proto::HashKind::Sha256) flags |= kRecFlagHashOk;

    if (cfg_.verify_readback && !cfg_.storage->is_volatile()) {
        uint32_t read_crc = 0u;
        const Status vst = verify_image(slot_, image_size_, expect_crc_,
                                        (hash_kind_ == proto::HashKind::Sha256) ? sha_expected_ : nullptr,
                                        hash_kind_, read_crc);
        if (vst != Status::Ok) {
            ++stats_.verify_fail;
            ++stats_.readback_fail;
            fail_session(proto::Code::Storage, static_cast<uint16_t>(vst), slot_);
            return;
        }
        flags |= kRecFlagReadbackOk;
    }

    ++stats_.verify_ok;

    // 4. Подпись: целостность — ещё не аутентичность. Пока подпись не
    //    проверена, слот НЕ помечается готовым к загрузке.
    if (cfg_.require_signature) {
        if (cfg_.verify_signature == nullptr || hash_kind_ != proto::HashKind::Sha256) {
            ++stats_.signature_fail;
            fail_session(proto::Code::Auth, 1u, slot_);
            return;
        }
        state_ = proto::State::Complete;
        SlotRecord rec = build_record(slot_, proto::State::Complete);
        rec.flags = flags;
        if (write_slot_record(rec) != Status::Ok) {
            fail_session(proto::Code::Storage, 0u, slot_);
            return;
        }
        // Слот, который ждёт подпись: кадр SIGN обязан относиться ИМЕННО к
        // нему, иначе (после перезагрузки или чужого VERIFY) он переписал бы
        // запись другого слота.
        verified_slot_ = slot_;
        hold_awake(false);
        session_open_ = false;

        proto::Response r{};
        make_response(r);
        r.detail = kDetailSignatureRequired;
        reply_ack(r);
        return;
    }

    state_ = proto::State::Verified;
    SlotRecord rec = build_record(slot_, proto::State::Verified);
    rec.flags = flags;
    const Status rst = write_slot_record(rec);
    if (rst != Status::Ok) {
        fail_session(proto::Code::Storage, static_cast<uint16_t>(rst), slot_);
        return;
    }
    // До подтверждения приложением этот слот — «проверенный»: запись менять
    // может только SIGN по нему же.
    verified_slot_ = slot_;
    hold_awake(false);
    session_open_ = false;

    proto::Response r{};
    make_response(r);
    reply_ack(r);
    log::write_u32(log::Level::Info, "OTA", "verified slot", slot_);
    emit(Event::ImageVerified);
}

void Service::on_sign(const proto::Frame& f) noexcept {
    // Подпись относится к КОНКРЕТНОМУ слоту, образ в котором только что прошёл
    // проверку целостности. Вне этой ситуации (нет ожидающего слота) кадр SIGN
    // отвергается: иначе он переписал бы запись чужого слота — например после
    // перезагрузки, когда state_ восстановлен из записи активного слота, или
    // после VERIFY, который никакой подписи не нёс.
    if (verified_slot_ == kSlotNone || state_ != proto::State::Complete) {
        reply_nak(proto::Code::BadState, static_cast<uint16_t>(state_));
        return;
    }
    const uint8_t slot = verified_slot_;
    if (cfg_.verify_signature == nullptr) {
        reply_nak(proto::Code::Unsupported, 0u);
        return;
    }

    proto::Sign hdr{};
    if (!proto::decode_sign_header(f.payload, f.len, hdr)) {
        reply_nak(proto::Code::BadRequest, proto::kSignHeaderSize);
        return;
    }
    if (hdr.total_len == 0u || hdr.total_len > proto::kMaxSignature) {
        reply_nak(proto::Code::TooLarge, proto::kMaxSignature);
        return;
    }
    const Size chunk = static_cast<Size>(f.len) - proto::kSignHeaderSize;
    if (hdr.offset != signature_len_) {
        // Фрагменты обязаны идти подряд: говорим, сколько уже принято.
        proto::Response r{};
        make_response(r);
        r.code   = static_cast<uint8_t>(proto::Code::BadOffset);
        r.detail = signature_len_;
        ++stats_.naks;
        reply(proto::Op::Nak, r, nullptr, 0u);
        return;
    }
    if ((static_cast<uint32_t>(signature_len_) + chunk) > hdr.total_len) {
        reply_nak(proto::Code::BadRequest, signature_len_);
        return;
    }

    for (Size i = 0u; i < chunk; ++i) {
        signature_[signature_len_ + i] = f.payload[proto::kSignHeaderSize + i];
    }
    signature_len_ = static_cast<uint16_t>(signature_len_ + chunk);
    signature_kind_ = hdr.kind;

    if (signature_len_ < hdr.total_len) {
        // Нужен ещё фрагмент: отвечаем, сколько подписи уже принято.
        proto::Response r{};
        make_response(r);
        r.next_offset = signature_len_;
        reply_ack(r);
        return;
    }

    // Подпись собрана: проверяем её на том самом сообщении, которое подписал
    // хост (см. ota.hpp, SignatureVerifier).
    SlotRecord rec = build_record(slot, proto::State::Complete);
    uint8_t message[kSignMessageSize];
    const Size message_len = build_sign_message(rec, message);

    const bool ok = cfg_.verify_signature(cfg_.signature_ctx, signature_kind_,
                                          message, message_len,
                                          signature_, signature_len_);
    if (!ok) {
        ++stats_.signature_fail;
        fail_session(proto::Code::Auth, 2u, slot);
        return;
    }

    ++stats_.signature_ok;
    state_ = proto::State::Verified;
    // Флаги проверок берём из записи, которую оставил END (CRC/хеш/чтение):
    // перезапись «с нуля» стёрла бы то, что уже подтверждено, и слот остался
    // бы с подписью, но без признака сошедшейся CRC.
    SlotRecord saved{};
    if (read_slot_record(slot, saved) == Status::Ok) {
        rec.flags = saved.flags;
        rec.crc_at_stored = saved.crc_at_stored;
        rec.stored_bytes = saved.stored_bytes;
    }
    rec.state = static_cast<uint8_t>(proto::State::Verified);
    rec.flags |= kRecFlagSignOk;
    const Status rst = write_slot_record(rec);
    if (rst != Status::Ok) {
        fail_session(proto::Code::Storage, static_cast<uint16_t>(rst), slot);
        return;
    }
    verified_slot_ = kSlotNone;   // подпись принята: слот больше не «ожидает»
    log::write_u32(log::Level::Info, "OTA", "signature ok, slot", slot);

    proto::Response r{};
    make_response(r);
    reply_ack(r);
    emit(Event::ImageVerified);
}

void Service::on_abort(const proto::Frame& f) noexcept {
    const uint8_t reason = (f.len >= proto::kAbortSize && f.payload != nullptr)
                         ? f.payload[0] : 0u;
    if (session_open_ && slot_ != kSlotNone) {
        ++stats_.aborts;
        // Отмена — не ошибка носителя: записанное остаётся, но слот не готов
        // к загрузке (состояние Failed). Продолжить его можно новым START с
        // флагом Resume: то, что реально лежит, будет перепроверено.
        state_ = proto::State::Failed;
        SlotRecord rec = build_record(slot_, proto::State::Failed);
        (void)write_slot_record(rec);
        verified_slot_ = kSlotNone;
        hold_awake(false);
        session_open_ = false;
    }
    proto::Response r{};
    make_response(r);
    r.detail = reason;
    reply_ack(r);
    emit(Event::Failed);
}

void Service::on_erase(const proto::Frame& f) noexcept {
    uint16_t slot = 0u;
    if (!proto::decode_erase(f.payload, f.len, slot)) {
        reply_nak(proto::Code::BadRequest, proto::kEraseSize);
        return;
    }
    if (!slot_valid(slot)) {
        reply_nak(proto::Code::BadRequest, slot);
        return;
    }
    const Status st = erase_slot(static_cast<uint8_t>(slot));
    if (st != Status::Ok) {
        reply_nak(st == Status::PermissionDenied ? proto::Code::Denied : proto::Code::Storage,
                  static_cast<uint16_t>(st));
        return;
    }
    // Ответ на ERASE — не про сессию, а про слот: собираем его вручную,
    // потому что сессии к этому моменту может уже не быть.
    proto::Response r{};
    r.code          = static_cast<uint8_t>(proto::Code::Ok);
    r.state         = static_cast<uint8_t>(proto::State::Idle);
    r.slot          = slot;
    r.max_payload   = max_payload_;
    r.slot_offset   = cfg_.slot_offset[slot];
    r.slot_capacity = cfg_.slot_capacity[slot];
    reply_ack(r);
}

void Service::on_verify(const proto::Frame& f) noexcept {
    proto::Verify v{};
    if (!proto::decode_verify(f.payload, f.len, v)) {
        reply_nak(proto::Code::BadRequest, proto::kVerifySize);
        return;
    }
    if (!slot_valid(v.slot)) {
        reply_nak(proto::Code::BadRequest, v.slot);
        return;
    }
    if (v.image_size == 0u || v.image_size > cfg_.slot_capacity[v.slot]) {
        reply_nak(proto::Code::NoSpace, static_cast<uint16_t>(cfg_.slot_capacity[v.slot]));
        return;
    }
    const proto::HashKind kind = static_cast<proto::HashKind>(v.hash_kind);
    if (kind != proto::HashKind::None && kind != proto::HashKind::Sha256) {
        reply_nak(proto::Code::Unsupported, v.hash_kind);
        return;
    }
    // Кадр VERIFY подписи не несёт, поэтому при require_signature он НЕ МОЖЕТ
    // пометить слот годным: иначе достаточно залить образ другим каналом и
    // попросить VERIFY, чтобы обойти проверку подписи целиком.
    if (cfg_.require_signature) {
        reply_nak(proto::Code::Denied, 0u);
        return;
    }

    const Status st = verify_stored(static_cast<uint8_t>(v.slot), v.image_size,
                                    v.image_crc32,
                                    (kind == proto::HashKind::Sha256) ? v.sha256 : nullptr);

    proto::Response r{};
    r.max_payload   = max_payload_;
    r.slot          = v.slot;
    r.slot_offset   = cfg_.slot_offset[v.slot];
    r.slot_capacity = cfg_.slot_capacity[v.slot];
    r.image_crc32   = v.image_crc32;
    if (st == Status::Ok) {
        r.code  = static_cast<uint8_t>(proto::Code::Ok);
        r.state = static_cast<uint8_t>(proto::State::Verified);
        reply_ack(r);
    } else {
        r.code  = static_cast<uint8_t>(proto::Code::Verify);
        r.state = static_cast<uint8_t>(proto::State::Failed);
        r.detail = static_cast<uint16_t>(st);
        ++stats_.naks;
        reply(proto::Op::Nak, r, nullptr, 0u);
    }
}

void Service::on_activate(const proto::Frame& f) noexcept {
    uint16_t slot = proto::kSlotAuto;
    if (f.len >= proto::kEraseSize && f.payload != nullptr) {
        slot = proto::get_u16(f.payload);
    }
    const uint8_t target = slot_valid(slot) ? static_cast<uint8_t>(slot)
                                            : ((slot_ != kSlotNone) ? slot_ : pick_slot(slot));
    const Status st = activate(target);
    if (st == Status::AlreadyExists) {
        // Слот и так исполняется: активировать нечего, но это не ошибка.
        proto::Response r{};
        make_response(r);
        r.state = static_cast<uint8_t>(proto::State::Running);
        r.slot  = target;
        reply_ack(r);
        return;
    }
    if (st != Status::Ok) {
        reply_nak(st == Status::PermissionDenied ? proto::Code::Denied : proto::Code::BadState,
                  static_cast<uint16_t>(st));
        return;
    }
    proto::Response r{};
    make_response(r);
    r.slot  = target;
    r.state = static_cast<uint8_t>(state_);
    r.slot_offset = cfg_.slot_offset[target];
    r.slot_capacity = cfg_.slot_capacity[target];
    reply_ack(r);
}

void Service::on_confirm(const proto::Frame& /*f*/) noexcept {
    const Status st = confirm_running();
    proto::Response r{};
    make_response(r);
    if (st == Status::Ok) {
        reply_ack(r);
    } else {
        reply_nak(proto::Code::BadState, static_cast<uint16_t>(st));
    }
}

void Service::on_state(const proto::Frame& /*f*/) noexcept {
    proto::Response r{};
    make_response(r);

    // Пока сессия открыта, отвечаем ЖИВЫМИ значениями: запись состояния
    // сохраняется не после каждого блока (иначе носитель изнашивался бы
    // втрое быстрее), поэтому она отстаёт — и хост, поверив ей, повторил бы
    // уже принятые блоки. Запись читаем только тогда, когда сессии нет:
    // именно она отвечает на вопрос «что осталось от прерванной передачи».
    if (!session_open_ && slot_ != kSlotNone) {
        SlotRecord rec{};
        if (read_slot_record(slot_, rec) == Status::Ok) {
            r.next_offset = rec.stored_bytes;
            r.image_crc32 = rec.image_crc32;
            r.fw_version  = rec.fw_version;
            r.state       = rec.state;
        }
    }
    reply_ack(r);
}

void Service::on_read(const proto::Frame& f) noexcept {
    proto::Read req{};
    if (!proto::decode_read(f.payload, f.len, req)) {
        reply_nak(proto::Code::BadRequest, proto::kReadSize);
        return;
    }
    const uint8_t target = (slot_ != kSlotNone) ? slot_ : cfg_.active_slot;
    if (target == kSlotNone) {
        reply_nak(proto::Code::BadState, 0u);
        return;
    }

    // Сколько влезет: payload ограничен пределом кадра, а он — интерфейсом и
    // буфером приёмника хоста. Просим меньше — отвечаем меньше.
    const uint32_t room = (max_payload_ > proto::kResponseSize)
                        ? static_cast<uint32_t>(max_payload_ - proto::kResponseSize) : 0u;
    uint32_t want = req.len;
    if (want > room) want = room;
    if (want > cfg_.slot_capacity[target]) want = cfg_.slot_capacity[target];
    if (req.offset >= cfg_.slot_capacity[target]) want = 0u;
    if (want > (cfg_.slot_capacity[target] - req.offset)) {
        want = cfg_.slot_capacity[target] - req.offset;
    }

    proto::Response r{};
    r.max_payload   = max_payload_;
    r.slot          = target;
    r.slot_offset   = cfg_.slot_offset[target];
    r.slot_capacity = cfg_.slot_capacity[target];
    r.next_offset   = req.offset;
    r.detail        = static_cast<uint16_t>(want);

    if (want == 0u) {
        r.code = static_cast<uint8_t>(proto::Code::BadOffset);
        ++stats_.naks;
        reply(proto::Op::Nak, r, nullptr, 0u);
        return;
    }

    // Данные читаются СРАЗУ в буфер кадра: вторая копия на стеке не нужна
    // (стек задачи измеряется десятками слов) и не нужна копия в отдельном
    // буфере — кадр собирается на месте.
    const Status st = cfg_.storage->read(cfg_.slot_offset[target] + req.offset,
                                         tx_frame_ + proto::kHeaderInFrame + proto::kResponseSize,
                                         want);
    if (st != Status::Ok) {
        reply_nak(proto::Code::Storage, static_cast<uint16_t>(st));
        return;
    }

    r.code = static_cast<uint8_t>(proto::Code::Ok);
    r.state = static_cast<uint8_t>(state_);
    proto::encode_response(tx_frame_ + proto::kHeaderInFrame, r);
    send_frame(proto::Op::Ack, proto::kResponseSize + want);
}

// ──────────────────────────────── Сессия ───────────────────────────────────

uint8_t Service::pick_slot(uint16_t requested) noexcept {
    if (slot_valid(requested)) return static_cast<uint8_t>(requested);

    // Авто. Правило, а не догадка: новый образ пишется в слот, который
    //   1) не исполняется сейчас (иначе стирание убьёт работающий код) и
    //   2) ещё не содержит проверенного образа — это резервная копия, ради
    //      которой схема A/B и существует (её затирают только явно,
    //      флагом kStartOverwrite).
    // Если оба слота заняты проверенными образами, выбираем неисполняемый:
    // приём всё равно потребует явного согласия на перезапись.
    uint8_t fallback = (cfg_.active_slot == 0u) ? 1u : 0u;

    for (uint8_t s = 0u; s < 2u; ++s) {
        if (cfg_.slot_capacity[s] == 0u) continue;
        if (s == cfg_.active_slot) continue;

        SlotRecord rec{};
        if (read_slot_record(s, rec) != Status::Ok) return s;   // слот пуст
        if (!rec.verified()) return s;                          // незавершённая передача
    }

    if (cfg_.slot_capacity[fallback] != 0u) return fallback;
    return (cfg_.slot_capacity[0] != 0u) ? 0u : 1u;
}

Status Service::open_session(const proto::Start& s, proto::Response& r, bool& resumed) noexcept {
    resumed = false;
    r = proto::Response{};
    r.max_payload = max_payload_;

    // ── Проверки, которые не зависят от состояния ──
    if (s.image_size == 0u) {
        r.code = static_cast<uint8_t>(proto::Code::BadRequest);
        return Status::InvalidArg;
    }
    if (s.image_size > image_limit_) {
        r.code = static_cast<uint8_t>(proto::Code::NoSpace);
        r.detail = static_cast<uint16_t>(image_limit_ > 0xFFFFu ? 0xFFFFu : image_limit_);
        return Status::NoMemory;
    }
    const proto::HashKind kind = static_cast<proto::HashKind>(s.hash_kind);
    if (kind != proto::HashKind::None && kind != proto::HashKind::Sha256) {
        r.code = static_cast<uint8_t>(proto::Code::Unsupported);
        r.detail = s.hash_kind;
        return Status::NotSupported;
    }

    const uint8_t target = pick_slot(s.slot);
    if (cfg_.slot_capacity[target] < s.image_size) {
        r.code = static_cast<uint8_t>(proto::Code::NoSpace);
        r.detail = static_cast<uint16_t>(cfg_.slot_capacity[target]);
        return Status::NoMemory;
    }

    // ── Слот, из которого ИСПОЛНЯЕТСЯ код, трогать нельзя ──
    //
    // Это не «политика вежливости»: стирание страницы исполняемой прошивки —
    // это мгновенный HardFault (или тихий уход в мусор), и спасти устройство
    // сможет только загрузчик. Разрешается лишь явно (allow_active_erase) —
    // для случая, когда код работает из ОЗУ, а слот целиком в его власти.
    if (target == cfg_.active_slot && !cfg_.allow_active_erase) {
        r.code   = static_cast<uint8_t>(proto::Code::Denied);
        r.detail = target;
        return Status::PermissionDenied;
    }

    // ── Повторный START той же сессии: ответ мог потеряться ──
    if (session_open_) {
        const bool same = (slot_ == target) && (image_size_ == s.image_size) &&
                          (expect_crc_ == s.image_crc32) &&
                          (fw_version_ == s.fw_version);
        if (!same) {
            r.code = static_cast<uint8_t>(proto::Code::Busy);
            r.state = static_cast<uint8_t>(state_);
            r.next_offset = next_offset_;
            r.slot = slot_;
            return Status::Busy;
        }
        r.code = static_cast<uint8_t>(proto::Code::Ok);
        r.state = static_cast<uint8_t>(state_);
        r.slot = slot_;
        r.slot_offset = cfg_.slot_offset[slot_];
        r.slot_capacity = cfg_.slot_capacity[slot_];
        r.next_offset = next_offset_;
        r.image_crc32 = proto::image_crc_finish(crc_);
        r.fw_version = fw_version_;
        resumed = (next_offset_ > 0u);
        return Status::Ok;
    }

    // ── Откат версии ──
    if (!cfg_.allow_downgrade && s.fw_version < cfg_.fw_version) {
        r.code = static_cast<uint8_t>(proto::Code::Denied);
        r.detail = static_cast<uint16_t>(cfg_.fw_version);
        return Status::PermissionDenied;
    }

    if ((s.flags & proto::kStartResume) != 0u) {
        const Status st = resume_session(s, r);
        if (st == Status::Ok) { resumed = (r.next_offset > 0u); return Status::Ok; }
        if (st != Status::NotFound) {
            // Отказ носителя при сверке префикса: сообщаем ИМЕННО это, а не
            // «продолжать нечего».
            if (r.code == static_cast<uint8_t>(proto::Code::Ok)) {
                r.code = static_cast<uint8_t>(st == Status::NotSupported
                                                ? proto::Code::Unsupported
                                                : proto::Code::Storage);
            }
            return st;
        }
        // Продолжать нечего — начинаем с нуля (возврат Ok от finish_fresh).
        return start_fresh(s, r);
    }
    return start_fresh(s, r);
}

/// Начать передачу «с нуля»: слот помечается как принимающий, данные
/// стираются ПОСТРАНИЧНО по мере записи (см. ensure_erased).
Status Service::start_fresh(const proto::Start& s, proto::Response& r) noexcept {
    const uint8_t target = pick_slot(s.slot);

    // Если в слоте лежит проверенный образ, а хост пишет ДРУГОЙ — это не
    // «обновление», а затирание резервной копии, ради которой схема A/B и
    // существует. Требуем явного согласия (kStartOverwrite).
    // Исполняемый слот сюда не попадает: он отсечён в open_session().
    SlotRecord old{};
    if (read_slot_record(target, old) == Status::Ok && old.verified() &&
        !((s.flags & proto::kStartOverwrite) != 0u)) {
        const bool same_image = (old.image_size == s.image_size) &&
                                (old.image_crc32 == s.image_crc32);
        if (!same_image) {
            r.code = static_cast<uint8_t>(proto::Code::Denied);
            r.detail = static_cast<uint16_t>(proto::State::Verified);
            return Status::PermissionDenied;
        }
    }

    slot_            = target;
    state_           = proto::State::Started;
    image_size_      = s.image_size;
    expect_crc_      = s.image_crc32;
    fw_version_      = s.fw_version;
    chunk_max_       = (s.chunk_max == 0u || s.chunk_max > max_block())
                     ? max_block() : s.chunk_max;
    hash_kind_       = static_cast<proto::HashKind>(s.hash_kind);
    next_offset_     = 0u;
    written_         = 0u;
    crc_written_     = proto::image_crc_begin();
    pending_len_     = 0u;
    erased_upto_     = 0u;
    next_checkpoint_ = cfg_.checkpoint_bytes;
    crc_             = proto::image_crc_begin();
    sha_.reset();
    signature_len_   = 0u;
    signature_kind_  = 0u;
    for (Size i = 0u; i < 32u; ++i) sha_expected_[i] = s.sha256[i];
    session_open_    = true;
    ++stats_.sessions;
    hold_awake(true);

    // Запись состояния: с этого момента слот «занят приёмом». Если питание
    // пропадёт сразу, хост продолжит с нуля — потерь нет, данные не писались.
    verified_slot_ = kSlotNone;
    SlotRecord rec = build_record(target, proto::State::Started);
    const Status st = write_slot_record(rec);
    if (st != Status::Ok) {
        session_open_ = false;
        hold_awake(false);
        r.code = static_cast<uint8_t>(proto::Code::Storage);
        r.detail = static_cast<uint16_t>(st);
        return st;
    }
    last_frame_ms_ = now_ms();

    r.code = static_cast<uint8_t>(proto::Code::Ok);
    r.state = static_cast<uint8_t>(state_);
    r.slot = slot_;
    r.slot_offset = cfg_.slot_offset[slot_];
    r.slot_capacity = cfg_.slot_capacity[slot_];
    r.next_offset = 0u;
    r.image_crc32 = expect_crc_;
    r.fw_version = fw_version_;
    return Status::Ok;
}

/// Продолжить прерванную передачу. Возврат NotFound означает «продолжать
/// нечего» — вызывающий начнёт с нуля.
Status Service::resume_session(const proto::Start& s, proto::Response& r) noexcept {
    const uint8_t target = pick_slot(s.slot);

    SlotRecord rec{};
    if (read_slot_record(target, rec) != Status::Ok) return Status::NotFound;

    // Другой образ — это не «продолжение», а новая передача.
    if (rec.image_size != s.image_size || rec.image_crc32 != s.image_crc32 ||
        rec.fw_version != s.fw_version ||
        rec.hash_kind != s.hash_kind) {
        return Status::NotFound;
    }
    // Для ПРОВЕРЕННОГО образа «больше размера» — норма: последняя гранула
    // добита значением 0xFF. Для незавершённой передачи это признак ошибки.
    const proto::State rec_state0 = static_cast<proto::State>(rec.state);
    if (rec.stored_bytes > s.image_size && !proto::state_is_loadable(rec_state0)) {
        return Status::NotFound;
    }
    uint32_t prefix_limit = rec.stored_bytes;
    if (prefix_limit > s.image_size) prefix_limit = s.image_size;

    // Состояние, о котором сообщим хосту: если образ уже признан годным,
    // «продолжение» означает лишь повторную проверку, и состояние надо
    // сохранить (иначе хост решит, что слот снова пуст).
    const proto::State rec_state = static_cast<proto::State>(rec.state);

    // ── Проверка того, что РЕАЛЬНО лежит ──
    //
    // Хосту верить нельзя: он может «помнить» 32 КБ, которых в носителе нет
    // (сбой записи), или наоборот не знать про записанное. Читаем префикс и
    // считаем его CRC32 — только это доказывает, что продолжение не оставит
    // дыру. Для SHA-256 чтение обязательно в любом случае: хеш нельзя
    // «достроить с середины», промежуточное состояние не выводится из дайджеста.
    const bool need_read = cfg_.verify_resume_prefix || (static_cast<proto::HashKind>(rec.hash_kind) ==
                                                         proto::HashKind::Sha256);
    uint32_t prefix = prefix_limit;

    crc_ = proto::image_crc_begin();
    sha_.reset();
    uint32_t read_crc = 0u;

    if (need_read && prefix > 0u) {
        const Status vst = verify_prefix(target, prefix, rec.crc_at_stored,
                                        static_cast<proto::HashKind>(rec.hash_kind),
                                        rec.sha256, read_crc);
        if (vst != Status::Ok) {
            // Префикс не сошёлся: продолжать нельзя, начинаем заново.
            ++stats_.resume_rejected;
            return Status::NotFound;
        }
    } else if (prefix > 0u) {
        // Проверка выключена (допустимо только для носителя, гарантирующего
        // запись): продолжаем расчёт CRC с сохранённого состояния.
        crc_ = proto::image_crc_resume_state(rec.crc_at_stored);
    }

    // ── Восстановить сессию ──
    slot_            = target;
    image_size_      = rec.image_size;
    expect_crc_      = rec.image_crc32;
    fw_version_      = rec.fw_version;
    hash_kind_       = static_cast<proto::HashKind>(rec.hash_kind);
    chunk_max_       = (s.chunk_max == 0u || s.chunk_max > max_block()) ? max_block() : s.chunk_max;
    next_offset_     = prefix;
    written_         = prefix;
    crc_written_     = crc_;        // префикс прочитан и проверен: он же записан
    pending_len_     = 0u;
    erased_upto_     = align_up(prefix, page_);
    next_checkpoint_ = prefix + cfg_.checkpoint_bytes;
    signature_len_   = 0u;
    session_open_    = true;
    // Готовый образ остаётся готовым: сессия открыта лишь для того, чтобы
    // хост мог повторно прислать END (и, если нужно, SIGN).
    if (rec_state == proto::State::Verified || rec_state == proto::State::Activated ||
        rec_state == proto::State::Running) {
        state_ = rec_state;
    } else {
        state_ = (next_offset_ >= image_size_) ? proto::State::Complete
                                               : proto::State::Receiving;
    }
    for (Size i = 0u; i < 32u; ++i) sha_expected_[i] = rec.sha256[i];
    ++stats_.sessions;
    ++stats_.resumed;
    hold_awake(true);
    last_frame_ms_ = now_ms();

    r.code = static_cast<uint8_t>(proto::Code::Ok);
    r.state = static_cast<uint8_t>(state_);
    r.slot = slot_;
    r.slot_offset = cfg_.slot_offset[slot_];
    r.slot_capacity = cfg_.slot_capacity[slot_];
    r.next_offset = next_offset_;
    r.image_crc32 = expect_crc_;
    r.fw_version = fw_version_;
    return Status::Ok;
}

void Service::close_session(bool expired) noexcept {
    if (!session_open_) return;
    if (expired) {
        ++stats_.expired;
        // Записываем состояние: продолжить можно будет с того, что лежит.
        (void)checkpoint(true);
    }
    session_open_ = false;
    hold_awake(false);
}

void Service::fail_session(proto::Code code, uint16_t detail, uint8_t slot) noexcept {
    const uint8_t target = (slot != kSlotNone) ? slot : slot_;
    state_ = proto::State::Failed;
    if (target != kSlotNone) {
        SlotRecord rec = build_record(target, proto::State::Failed);
        rec.flags = 0u;   // не проверен — не загружаем
        (void)write_slot_record(rec);
    }
    verified_slot_ = kSlotNone;
    hold_awake(false);
    session_open_ = false;
    reply_nak(code, detail);
    emit(Event::Failed);
}

SlotRecord Service::build_record(uint8_t slot, proto::State state) const noexcept {
    SlotRecord rec{};
    // Слот передаётся ЯВНО: «если неизвестно, то слот 0» — это молчаливая
    // запись в чужой слот, а не догадка.
    rec.slot          = slot;
    rec.state         = static_cast<uint8_t>(state);
    rec.hash_kind     = static_cast<uint8_t>(hash_kind_);
    rec.image_size    = image_size_;
    rec.image_crc32   = expect_crc_;
    // «Продолжать отсюда» — это то, что РЕАЛЬНО лежит в носителе, а не то,
    // что принято: недобранная гранула лежит в буфере сервиса и после сброса
    // исчезнет.
    rec.stored_bytes  = written_;
    rec.crc_at_stored = proto::image_crc_finish(crc_written_);
    rec.fw_version    = fw_version_;
    for (Size i = 0u; i < 32u; ++i) rec.sha256[i] = sha_expected_[i];
    return rec;
}

// ─────────────────────────── Носитель и метаданные ─────────────────────────

Status Service::ensure_erased(uint32_t upto) noexcept {
    const uint32_t page = page_;
    if (page <= 1u || slot_ == kSlotNone) return Status::Ok;

    uint32_t want = align_up(upto, page);
    if (want > cfg_.slot_capacity[slot_]) {
        // Стирать «часть страницы» нельзя, а выходить за слот — тем более:
        // честный отказ вместо стирания с некратной длиной (его носитель
        // отвергнет уже после частичного стирания).
        ++stats_.storage_errors;
        return Status::NoMemory;
    }
    if (want <= erased_upto_) return Status::Ok;

    stall_begin();
    const Status st = cfg_.storage->erase(cfg_.slot_offset[slot_] + erased_upto_,
                                         want - erased_upto_);
    stall_end();
    if (st != Status::Ok) {
        ++stats_.storage_errors;
        return st;
    }
    stats_.page_erases += (want - erased_upto_) / page;
    erased_upto_ = want;
    return Status::Ok;
}

Status Service::write_image(uint32_t /*offset*/, const void* data, Size len) noexcept {
    // Смещение здесь не нужно: запись всегда идёт строго вперёд, от written_.
    return stage_bytes(static_cast<const uint8_t*>(data), len);
}

Status Service::stage_bytes(const uint8_t* data, Size len) noexcept {
    if (slot_ == kSlotNone) return Status::Unavailable;
    if (data == nullptr && len != 0u) return Status::InvalidArg;

    Size pos = 0u;
    while (pos < len) {
        // Копим до полной гранулы.
        while (pos < len && pending_len_ < write_gran_) {
            pending_[pending_len_++] = data[pos++];
        }
        if (pending_len_ < write_gran_) break;      // гранула ещё не набрана

        const Status est = ensure_erased(written_ + write_gran_);
        if (est != Status::Ok) return est;

        const Status st = cfg_.storage->write(cfg_.slot_offset[slot_] + written_,
                                             pending_, write_gran_);
        if (st != Status::Ok) {
            ++stats_.storage_errors;
            return st;
        }
        crc_written_ = proto::image_crc_update(crc_written_, pending_, write_gran_);
        stats_.bytes_written += write_gran_;
        written_  += write_gran_;
        pending_len_ = 0u;
    }
    return Status::Ok;
}

Status Service::flush_pending() noexcept {
    if (slot_ == kSlotNone || pending_len_ == 0u) return Status::Ok;

    // Добиваем порцию до гранулы значением 0xFF: байты за концом образа лежат
    // в свободной части слота, и запись туда ничего не портит (стирание слота
    // уже прошло). Именно так «один последний байт» перестаёт быть проблемой
    // для носителя, умеющего писать только half-word.
    while (pending_len_ < write_gran_) pending_[pending_len_++] = 0xFFu;

    const Status est = ensure_erased(written_ + write_gran_);
    if (est != Status::Ok) return est;

    const Status st = cfg_.storage->write(cfg_.slot_offset[slot_] + written_,
                                         pending_, write_gran_);
    if (st != Status::Ok) {
        ++stats_.storage_errors;
        return st;
    }
    crc_written_ = proto::image_crc_update(crc_written_, pending_, write_gran_);
    stats_.bytes_written += write_gran_;
    written_    += write_gran_;
    pending_len_ = 0u;
    return Status::Ok;
}

Status Service::checkpoint(bool force) noexcept {
    if (!session_open_ && !force) return Status::Ok;
    if (slot_ == kSlotNone) return Status::Unavailable;
    if (!force) {
        if (cfg_.checkpoint_bytes == 0u) return Status::Ok;
        if (next_offset_ < next_checkpoint_) return Status::Ok;
    }
    SlotRecord rec = build_record(slot_, state_);
    const Status st = write_slot_record(rec);
    if (st != Status::Ok) {
        ++stats_.storage_errors;
        return st;
    }
    ++stats_.checkpoints;
    next_checkpoint_ = next_offset_ + cfg_.checkpoint_bytes;
    return Status::Ok;
}

Status Service::read_slot_record(uint8_t slot, SlotRecord& out) noexcept {
    return slots_.load(slot, out);
}

Status Service::write_slot_record(const SlotRecord& rec) noexcept {
    // kSlotNone здесь — ошибка вызывающего, а не «слот по умолчанию»: запись
    // состояния обязана относиться к конкретному слоту.
    if (!slot_valid(rec.slot)) return Status::InvalidArg;
    return slots_.store(rec.slot, rec);
}

Status Service::slot_record(uint8_t slot, SlotRecord& out) noexcept {
    if (!inited_) return Status::Unavailable;
    return read_slot_record(slot, out);
}

/// Посчитать CRC32 (и SHA-256) по префиксу образа, лежащего в слоте.
///
/// ЗАЧЕМ ОТДЕЛЬНАЯ ФУНКЦИЯ, А НЕ «ПРОСТО СРАВНИТЬ CRC»
/// Кроме ответа «префикс цел», нужен ещё и РАЗОГРЕТЫЙ расчёт: продолжать
/// приём можно только с того состояния CRC32/SHA-256, которое соответствует
/// уже лежащим байтам. Здесь оно и восстанавливается:
///   * CRC32 — читая префикс, попутно вычисляем его состояние (в конце оно
///     остаётся в crc_, «сырым», то есть готовым к продолжению);
///   * SHA-256 — состояние НЕ выводится из дайджеста, поэтому хеш тоже
///     считается по префиксу заново; для сверки берётся копия состояния
///     (finalize() обнуляет объект, и после него продолжать было бы нельзя).
Status Service::verify_prefix(uint8_t slot, uint32_t len, uint32_t expect_crc,
                              proto::HashKind kind, const uint8_t sha_expected[32],
                              uint32_t& out_crc) noexcept {
    uint32_t crc = proto::image_crc_begin();
    sha_.reset();

    uint32_t done = 0u;
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof(io_buf_)) chunk = sizeof(io_buf_);

        const Status st = cfg_.storage->read(cfg_.slot_offset[slot] + done, io_buf_, chunk);
        if (st != Status::Ok) {
            ++stats_.storage_errors;
            return st;
        }
        crc = proto::image_crc_update(crc, io_buf_, chunk);
        if (kind == proto::HashKind::Sha256) sha_.update(io_buf_, chunk);
        done += chunk;
    }

    out_crc = proto::image_crc_finish(crc);
    if (out_crc != expect_crc) return Status::Corruption;

    if (kind == proto::HashKind::Sha256 && sha_expected != nullptr) {
        // Копия состояния: finalize() возвращает объект в исходное состояние,
        // а нам нужно и сверить дайджест префикса, и продолжить считать хеш
        // всего образа с этого места.
        crypto::Sha2<crypto::Sha2Variant::Sha256> probe = sha_;
        uint8_t digest[32]{};
        probe.finalize(digest);
        if (!bytes_equal(digest, sha_expected, 32u)) return Status::Corruption;
    }

    crc_ = crc;   // продолжение расчёта CRC32 — с прочитанного префикса
    return Status::Ok;
}

Status Service::verify_image(uint8_t slot, uint32_t image_size, uint32_t expect_crc,
                            const uint8_t* expect_sha, proto::HashKind kind,
                            uint32_t& out_crc) noexcept {
    uint32_t crc = proto::image_crc_begin();
    crypto::Sha2<crypto::Sha2Variant::Sha256> hasher{};

    uint32_t done = 0u;
    while (done < image_size) {
        uint32_t chunk = image_size - done;
        if (chunk > sizeof(io_buf_)) chunk = sizeof(io_buf_);

        const Status st = cfg_.storage->read(cfg_.slot_offset[slot] + done, io_buf_, chunk);
        if (st != Status::Ok) {
            ++stats_.storage_errors;
            return st;
        }
        crc = proto::image_crc_update(crc, io_buf_, chunk);
        if (kind == proto::HashKind::Sha256) hasher.update(io_buf_, chunk);
        done += chunk;
    }

    out_crc = proto::image_crc_finish(crc);
    if (out_crc != expect_crc) return Status::Corruption;

    if (kind == proto::HashKind::Sha256 && expect_sha != nullptr) {
        uint8_t digest[32]{};
        hasher.finalize(digest);
        if (!bytes_equal(digest, expect_sha, 32u)) return Status::Corruption;
    }
    return Status::Ok;
}

// ──────────────────────── Загрузка и активация ─────────────────────────────

Status Service::boot_check(BootInfo& out) noexcept {
    out = BootInfo{};
    if (!inited_) return Status::Unavailable;
    ++stats_.boot_checks;

    uint8_t slot = cfg_.active_slot;
    if (!slot_valid(slot)) {
        // Активный слот не задан: ищем активированный — именно он и есть
        // «кандидат на загрузку».
        for (uint8_t s = 0u; s < 2u; ++s) {
            SlotRecord rec{};
            if (read_slot_record(s, rec) == Status::Ok &&
                static_cast<proto::State>(rec.state) == proto::State::Activated) {
                slot = s;
                break;
            }
        }
    }
    if (!slot_valid(slot)) {
        boot_slot_ = kSlotNone;
        return Status::Ok;
    }
    boot_slot_ = slot;

    SlotRecord rec{};
    if (read_slot_record(slot, rec) != Status::Ok) {
        // Записи нет: устройство работает на «заводской» прошивке.
        return Status::Ok;
    }

    // Резервный источник счётчика попыток: резервные регистры (они переживают
    // сброс и не портятся отказом носителя). Нужен ровно в одном случае — если
    // запись метаданных не удалась, счётчик в носителе не растёт, и защита от
    // бесконечной перезагрузки сломалась бы молча.
    const platform::WatchdogOps* wd = platform::ops().watchdog;
    if (wd != nullptr && wd->note_read != nullptr &&
        wd->note_read(cfg_.note_base + kNoteIdxMagic) == kNoteMagic) {
        const uint32_t note_attempts = wd->note_read(cfg_.note_base + kNoteIdxAttempts);
        if (note_attempts > rec.boot_attempts) rec.boot_attempts = note_attempts;
    }

    out.valid         = true;
    out.slot          = slot;
    out.state         = static_cast<proto::State>(rec.state);
    out.fw_version    = rec.fw_version;
    out.image_size    = rec.image_size;
    out.image_crc32   = rec.image_crc32;
    out.boot_attempts = rec.boot_attempts;

    if (static_cast<proto::State>(rec.state) == proto::State::Activated) {
        // Активированный слот означает: «эта загрузка — пробная». Считаем
        // попытки ОБЩИМ правилом (см. ota_storage.hpp): то же правило
        // использует загрузчик, поэтому «кто виноват в перезагрузках» не
        // зависит от того, кто именно считает.
        const bool exhausted = record_take_trial(rec, cfg_.max_boot_attempts);
        out.boot_attempts = rec.boot_attempts;
        out.rollback_needed = exhausted;
        out.trial = !exhausted;

        // Счётчик попыток пишется В НОСИТЕЛЬ, и его отказ нельзя игнорировать:
        // без этого прошивка, падающая на старте, перезагружалась бы вечно.
        const Status wst = write_slot_record(rec);
        // Отметка в резервных регистрах — второй источник счётчика.
        note_reset(slot, rec.boot_attempts, rec.state);
        if (wst != Status::Ok) {
            ++stats_.storage_errors;
        }
    }
    return Status::Ok;
}

Status Service::confirm_running() noexcept {
    if (!inited_) return Status::Unavailable;

    // Слот подтверждаемой загрузки: заданный в настройке, иначе — тот, который
    // выбрал boot_check() при старте, иначе — единственный активированный.
    // Требовать именно Config::active_slot нельзя: режим «active_slot не задан,
    // слот выбирает загрузчик» документирован и поддерживается boot_check().
    uint8_t slot = cfg_.active_slot;
    if (!slot_valid(slot)) slot = boot_slot_;
    if (!slot_valid(slot)) {
        for (uint8_t s = 0u; s < 2u; ++s) {
            SlotRecord cand{};
            if (read_slot_record(s, cand) == Status::Ok &&
                static_cast<proto::State>(cand.state) == proto::State::Activated) {
                slot = s;
                break;
            }
        }
    }
    if (!slot_valid(slot)) return Status::Unavailable;

    SlotRecord rec{};
    if (read_slot_record(slot, rec) != Status::Ok) {
        // Подтверждать нечего: записи нет, устройство работает на том, что
        // было прошито программатором. Это не ошибка.
        return Status::Ok;
    }
    record_mark_running(rec);
    const Status st = write_slot_record(rec);
    if (st != Status::Ok) return st;

    ++stats_.confirms;
    log::write_u32(log::Level::Info, "OTA", "confirmed slot", slot);
    boot_slot_ = slot;
    if (slot_ == slot) state_ = proto::State::Running;
    else if (cfg_.active_slot == slot) state_ = proto::State::Running;
    emit(Event::Confirmed);
    return Status::Ok;
}

Status Service::activate(uint8_t slot) noexcept {
    if (!inited_) return Status::Unavailable;
    if (!slot_valid(slot)) return Status::InvalidArg;

    SlotRecord rec{};
    if (read_slot_record(slot, rec) != Status::Ok) return Status::NotFound;
    if (!rec.verified()) return Status::NotSupported;   // не проверен — не активируем
    // При требовании подписи активируется только слот, у которого подпись
    // ПРОВЕРЕНА: иначе неподписанный образ (залитый другим каналом) ушёл бы в
    // загрузку, и вся проверка подписи оказалась бы необязательной.
    if (cfg_.require_signature && (rec.flags & kRecFlagSignOk) == 0u) {
        return Status::PermissionDenied;
    }

    if (slot == cfg_.active_slot) {
        // Этот слот и так исполняется: «активировать» его нечего.
        state_ = proto::State::Running;
        return Status::AlreadyExists;
    }
    if (static_cast<proto::State>(rec.state) == proto::State::Activated) {
        // Слот уже ждёт перезагрузки. Повтор — не ошибка, но и не повод
        // сбрасывать счётчик попыток: иначе удалённый запрос «активируй ещё
        // раз» обнулял бы защиту от boot-loop.
        boot_slot_ = slot;
        return Status::AlreadyExists;
    }

    // Активированным может быть РОВНО ОДИН слот: иначе в метаданных лежат две
    // записи Activated, и что грузить решает порядок индексов, а не история.
    // Прежний кандидат возвращается в состояние Verified: образ остаётся
    // годным (его можно активировать снова), но «грузить его» больше не
    // просят.
    const uint8_t other = (slot == 0u) ? 1u : 0u;
    if (slot_valid(other)) {
        SlotRecord prev{};
        if (read_slot_record(other, prev) == Status::Ok &&
            static_cast<proto::State>(prev.state) == proto::State::Activated) {
            record_mark_verified(prev);      // образ остаётся годным, но не грузится
            (void)write_slot_record(prev);
        }
    }

    record_mark_activated(rec);
    const Status st = write_slot_record(rec);
    if (st != Status::Ok) return st;

    state_ = proto::State::Activated;
    boot_slot_ = slot;
    ++stats_.activations;
    log::write_u32(log::Level::Info, "OTA", "activated slot", slot);
    note_reset(slot, 0u, rec.state);
    emit(Event::Activated);
    return Status::Ok;
}

Status Service::rollback() noexcept {
    if (!inited_) return Status::Unavailable;

    const uint8_t active = cfg_.active_slot;
    const uint8_t other = (active == 0u) ? 1u : 0u;
    const uint32_t max_attempts = cfg_.max_boot_attempts;

    // Кандидат на откат — не «любой другой слот», а тот, который ДОКАЗАЛ
    // работоспособность: состояние Running (уже подтверждался) лучше, чем
    // Verified (проверен, но не запускался). Слот, исчерпавший пробные
    // загрузки, кандидатом быть не может: иначе откат возвращал бы именно ту
    // прошивку, которая и не даёт устройству стартовать.
    SlotRecord rec{};
    bool found = false;
    int best_rank = -1;

    for (uint8_t s = 0u; s < 2u; ++s) {
        if (s == active) continue;
        SlotRecord cand{};
        if (read_slot_record(s, cand) != Status::Ok) continue;

        const proto::State st = static_cast<proto::State>(cand.state);
        if (!proto::state_is_loadable(st)) continue;
        if (st == proto::State::Activated && max_attempts != 0u &&
            cand.boot_attempts > max_attempts) {
            // Этот слот уже признан негодным: помечаем и не возвращаемся к нему.
            record_mark_failed(cand, 3u);
            (void)write_slot_record(cand);
            continue;
        }
        const int rank = (st == proto::State::Running) ? 2
                       : ((st == proto::State::Verified) ? 1
                       : ((st == proto::State::Activated) ? 0 : -1));
        if (rank > best_rank) {
            best_rank = rank;
            rec = cand;
            found = true;
        }
        (void)other;
    }

    if (found) {
        record_mark_activated(rec);
        const Status st = write_slot_record(rec);
        if (st != Status::Ok) return st;
        state_ = proto::State::Activated;
        boot_slot_ = rec.slot;
        ++stats_.rollbacks;
        note_reset(rec.slot, 0u, rec.state);
        emit(Event::Rollback);
        return Status::Ok;
    }

    // Возвращаться некуда: снимаем активацию с текущего слота, чтобы
    // устройство не пыталось грузить слот, который не подтвердил себя.
    if (slot_valid(active) && read_slot_record(active, rec) == Status::Ok) {
        record_mark_failed(rec, 4u);
        (void)write_slot_record(rec);
    }
    boot_slot_ = kSlotNone;
    return Status::NotFound;
}

Status Service::erase_slot(uint8_t slot) noexcept {
    if (!inited_) return Status::Unavailable;
    if (!slot_valid(slot)) return Status::InvalidArg;
    if (slot == cfg_.active_slot && !cfg_.allow_active_erase) {
        // Стирание слота, из которого исполняется код, — это стирание себя.
        // Без загрузчика такое разрешать нельзя.
        return Status::PermissionDenied;
    }

    if (session_open_ && slot_ == slot) {
        // Сессию по этому слоту надо закрыть: иначе её next_offset указывал бы
        // на стёртые данные.
        session_open_ = false;
        hold_awake(false);
        next_offset_ = 0u;
        state_ = proto::State::Idle;
    }

    const uint32_t cap = cfg_.slot_capacity[slot];
    if (cap == 0u) return Status::InvalidArg;

    // ── СНАЧАЛА снимаем метку, ПОТОМ стираем ──
    //
    // Обратный порядок оставлял бы окно в сотни миллисекунд (28 КБ страницами
    // по 1 КБ на STM32F1 — это 0,6…1,1 с), в котором запись всё ещё говорит
    // «Verified», а в слоте уже 0xFF. Сбой питания в этом окне = загрузка
    // мусора. Если метку снять не удалось — не стираем вовсе: пусть лучше
    // останется старый образ, чем «пустой» слот с отметкой о годности.
    SlotRecord rec{};
    rec.slot  = slot;
    rec.state = static_cast<uint8_t>(proto::State::Idle);
    const Status mark = write_slot_record(rec);
    if (mark != Status::Ok) {
        ++stats_.storage_errors;
        return mark;
    }

    // Стирание — постранично и с окном сторожу на каждую страницу: страница
    // STM32F1 стирается десятки миллисекунд, и «одним куском» это вышло бы за
    // любой разумный срок сторожа. Шаг — кратный странице и не меньше 4 КБ.
    uint32_t step = (page_ > 4096u) ? page_ : 4096u;
    step = ((step + page_ - 1u) / page_) * page_;
    if (step > cap) step = cap;

    uint32_t done = 0u;
    while (done < cap) {
        uint32_t chunk = cap - done;
        if (chunk > step) chunk = step;
        if ((chunk % page_) != 0u) chunk = cap - done;   // хвост — целиком
        stall_begin();
        const Status st = cfg_.storage->erase(cfg_.slot_offset[slot] + done, chunk);
        stall_end();
        if (st != Status::Ok) {
            ++stats_.storage_errors;
            return st;
        }
        stats_.page_erases += (page_ > 0u) ? (chunk / page_) : 0u;
        done += chunk;
    }

    verified_slot_ = kSlotNone;
    if (cfg_.active_slot == slot) state_ = proto::State::Idle;
    if (slot_ == slot) {
        session_open_ = false;
        reset_session_state();
    }
    emit(Event::Erased);
    return Status::Ok;
}

Status Service::invalidate_slot(uint8_t slot, uint16_t detail) noexcept {
    if (!inited_) return Status::Unavailable;
    if (!slot_valid(slot)) return Status::InvalidArg;

    SlotRecord rec{};
    if (read_slot_record(slot, rec) != Status::Ok) {
        // Записи нет — создаём «пустую негодную»: важно, чтобы состояние слота
        // было явным, а не «неизвестным».
        rec = SlotRecord{};
        rec.slot = slot;
    }
    record_mark_failed(rec, detail);
    const Status st = write_slot_record(rec);
    if (st != Status::Ok) return st;

    ++stats_.invalidations;
    if (slot == slot_) {
        // Если этот слот — предмет текущей сессии, сессию тоже закрываем:
        // писать в слот, помеченный негодным, нельзя.
        session_open_ = false;
        hold_awake(false);
    }
    return Status::Ok;
}

Status Service::verify_stored(uint8_t slot, uint32_t size, uint32_t crc32,
                             const uint8_t* sha256) noexcept {
    if (!inited_) return Status::Unavailable;
    if (!slot_valid(slot)) return Status::InvalidArg;
    if (size == 0u) return Status::InvalidArg;
    if (size > cfg_.slot_capacity[slot]) return Status::NoMemory;
    // Целостность — ещё не аутентичность: при требовании подписи этот путь
    // (в нём подписи нет) не имеет права объявить образ годным.
    if (cfg_.require_signature) return Status::PermissionDenied;

    const proto::HashKind kind = (sha256 != nullptr) ? proto::HashKind::Sha256
                                                     : proto::HashKind::None;
    uint32_t computed = 0u;
    const Status vst = verify_image(slot, size, crc32, sha256, kind, computed);
    if (vst != Status::Ok) {
        ++stats_.verify_fail;
        // Запись портим ТОЛЬКО если в слоте не лежит годный образ: иначе
        // ошибочный (или злонамеренный) VERIFY с неверными size/crc уничтожил
        // бы резервную копию, ради которой всё и делается.
        SlotRecord cur{};
        const bool had_good = (read_slot_record(slot, cur) == Status::Ok) && cur.verified();
        if (!had_good) {
            SlotRecord rec{};
            rec.slot = slot;
            rec.state = static_cast<uint8_t>(proto::State::Failed);
            rec.image_size = size;
            rec.image_crc32 = crc32;
            (void)write_slot_record(rec);
        }
        return vst;
    }

    SlotRecord rec{};
    rec.slot         = slot;
    rec.state        = static_cast<uint8_t>(proto::State::Verified);
    rec.hash_kind    = static_cast<uint8_t>(kind);
    rec.image_size   = size;
    rec.image_crc32  = crc32;
    rec.stored_bytes = size;
    rec.crc_at_stored= crc32;
    rec.flags        = kRecFlagCrcOk | kRecFlagReadbackOk;
    if (kind == proto::HashKind::Sha256) {
        rec.flags |= kRecFlagHashOk;
        for (Size i = 0u; i < 32u; ++i) rec.sha256[i] = sha256[i];
    }
    const Status st = write_slot_record(rec);
    if (st != Status::Ok) return st;

    ++stats_.verify_ok;
    state_ = proto::State::Verified;
    // Слот проверен, но подписи в этом пути не было: подтверждением «грузить»
    // он не становится — это делается кадром ACTIVATE (или SIGN в сессии).
    verified_slot_ = kSlotNone;
    emit(Event::ImageVerified);
    return Status::Ok;
}

Stats Service::stats() const noexcept {
    return stats_;
}

} // namespace embed::ota
