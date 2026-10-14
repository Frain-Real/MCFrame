// os/boot/boot.cpp
// ---------------------------------------------------------------------------
// Политика загрузчика: выбор слота, проверка образа, окно ожидания хоста.
//
// ПОРЯДОК РАЗБОРА
//   1. init: раскладка и носитель;
//   2. evaluate: метки -> откат -> выбор слота -> проверка образа;
//   3. step: полный цикл (решить, подержать окно, сказать вызывающему, что
//      делать);
//   4. validate: вектор + CRC32/SHA-256;
//   5. работа приложения в слоте (fill_ota_config/confirm_self).
//
// ЧТО ЗДЕСЬ ГЛАВНОЕ
// Ни одно решение не принимается «по метке». Метка говорит лишь, что образ
// когда-то был принят и проверен; перед переходом загрузчик проверяет его
// СНОВА (вектор сброса и, по настройке, CRC32/SHA-256 по носителю). Причина
// простая: между «проверено» и «загружаем» может пройти сколько угодно времени
// и сколько угодно сбоев питания, а прыжок в испорченный образ — это отказ без
// диагностики и без возможности обновиться.
// ---------------------------------------------------------------------------

#include "boot.hpp"

#include "../config/time_config.hpp"
#include "../crypto/sha2.hpp"      // проверка образа по SHA-256, если он записан

namespace embed::boot {

namespace {

/// Как часто пересматривать решение, пока мы остаёмся в загрузчике.
/// Чаще — лишняя работа с носителем, реже — задержка реакции на только что
/// принятый образ.
constexpr uint32_t kReevaluateMs = 500u;

uint32_t flash_base() noexcept {
    const platform::FlashOps* f = platform::ops().flash;
    return (f != nullptr && f->base != nullptr) ? f->base() : 0u;
}

uint64_t now_ms() noexcept {
    return config::OsTimer::TimeAvailable() ? config::OsTimer::NowMs() : 0u;
}

/// Ранг состояния слота для выбора кандидата.
///
/// Активированный слот — это ЯВНАЯ просьба хоста загрузить именно его, поэтому
/// он важнее всего. Дальше — подтверждённый (Running: он уже доказывал, что
/// работает) и проверенный (Verified). Всё остальное грузить нечего.
int state_rank(ota::proto::State st) noexcept {
    switch (st) {
        case ota::proto::State::Activated: return 3;
        case ota::proto::State::Running:   return 2;
        case ota::proto::State::Verified:  return 1;
        default:                           return 0;
    }
}

} // namespace

// ────────────────────────────── Инициализация ──────────────────────────────

Status Loader::init(const Config& cfg) noexcept {
    if (inited_) {
        // Повторная инициализация допустима: загрузчик может захотеть
        // перечитать раскладку (например после смены настроек).
        inited_ = false;
    }
    if (cfg.storage == nullptr) return Status::InvalidArg;
    if (!cfg.layout.valid()) return Status::InvalidArg;
    if (platform::ops().boot == nullptr) return Status::NotSupported;

    cfg_ = cfg;

    // Раскладка проверяется ещё и по носителю: адреса обязаны быть кратны
    // странице, а слоты — влезать в его ёмкость. Ошибка здесь означала бы
    // стирание не той страницы, а это уже необратимо.
    const ota::LayoutCheck lc = ota::check_layout(*cfg_.storage, cfg_.layout.meta_offset,
                                                 cfg_.layout.slot_offset, cfg_.layout.slot_size);
    if (!lc.ok) return lc.status;
    layout_check_ = lc;

    const Status st = cfg_.storage->begin();
    if (st != Status::Ok && st != Status::NotSupported) return st;

    max_attempts_ = (cfg_.max_boot_attempts == 0u) ? 1u : cfg_.max_boot_attempts;

    window_open_    = (cfg_.window_ms != 0u);
    window_start_ms_= now_ms();
    decided_        = false;
    last_reason_    = Reason::NoImage;
    inited_         = true;
    return Status::Ok;
}

// ──────────────────────────── Чтение меток ─────────────────────────────────

Status Loader::read_record(uint8_t slot, ota::SlotRecord& out) noexcept {
    if (slot > 1u) return Status::InvalidArg;
    // Записи состояния читает общий SlotStore — тот же, что в сервисе OTA.
    // Дублировать формат метаданных здесь значило бы однажды разойтись с ним, а
    // цена расхождения — загрузка не того слота.
    if (!store_.initialized()) {
        const Status st = store_.init(cfg_.storage, cfg_.layout.meta_offset);
        if (st != Status::Ok) return st;
    }
    return store_.load(slot, out);
}

Status Loader::write_record(uint8_t slot, const ota::SlotRecord& rec) noexcept {
    if (slot > 1u) return Status::InvalidArg;
    if (!store_.initialized()) {
        const Status st = store_.init(cfg_.storage, cfg_.layout.meta_offset);
        if (st != Status::Ok) return st;
    }
    return store_.store(slot, rec);
}

// ─────────────────────────────── Решение ───────────────────────────────────

uint8_t Loader::pick_candidate() noexcept {
    // Выбор слота — ОБЩЕЕ правило (см. ota_storage.hpp): активированный слот
    // важнее подтверждённого, подтверждённый — проверенного, а при равных
    // состояниях предпочтительнее тот, из которого работаем сейчас.
    ota::SlotRecord rec{};
    if (!store_.initialized()) {
        if (store_.init(cfg_.storage, cfg_.layout.meta_offset) != Status::Ok) {
            return Layout::kNoSlot;
        }
    }
    const uint8_t slot = store_.pick(current_slot(), rec);
    return slot;
}

Status Loader::evaluate(Decision& out) noexcept {
    out = Decision{};
    if (!inited_) return Status::Unavailable;
    ++stats_.evaluations;

    if (!cfg_.layout.valid()) {
        out.action = Action::Stay;
        out.reason = Reason::BadLayout;
        last_reason_ = out.reason;
        return Status::InvalidArg;
    }

    // ── Пробные загрузки и откат ──
    //
    // Считает их ЗАГРУЗЧИК, а не сервис OTA: сервис в этот образ может быть
    // вовсе не вкомпонован (на 128 КБ Flash он туда не влезает вместе с двумя
    // слотами), а правило обязано работать всегда. Правило при этом ОДНО на
    // оба образа — record_take_trial()/record_mark_*() из ota_storage.hpp.
    uint8_t slot = pick_candidate();
    if (slot == Layout::kNoSlot) {
        // ── Образ без метки: первая прошивка или заливка по SWD ──
        //
        // Целостность проверить нечем (ожидаемой CRC32 нет), поэтому
        // проверяется только правдоподобие вектора, а попытки считаются в
        // регистрах, переживающих сброс. Иначе устройство, у которого нет
        // метаданных, не загрузило бы НИЧЕГО и осталось бы в загрузчике
        // навсегда (а если приём выполняет приложение — то и обновить его было
        // бы нельзя).
        if (cfg_.allow_unrecorded_image) {
            for (uint8_t s = 0u; s < 2u; ++s) {
                if (cfg_.layout.slot_size[s] == 0u) continue;
                if (!validate_plausible(s)) continue;

                const uint32_t attempts = note_attempts(s) + 1u;
                note(s, attempts, 0xFFu);       // 0xFF = «метки о слоте нет»
                if (cfg_.max_boot_attempts != 0u && attempts > cfg_.max_boot_attempts) {
                    // Образ «похож», но столько раз не подтвердил себя, что
                    // доверять ему больше нельзя. Заводим запись Failed —
                    // теперь слот выключен из загрузки до новой прошивки.
                    (void)store_.invalidate(s, 5u);
                    continue;
                }

                out.slot       = s;
                out.image_size = 0u;            // размер неизвестен: метки нет
                out.attempts   = attempts;
                out.trial      = true;
                out.action     = Action::Load;
                out.reason     = Reason::FactoryImage;
                if (cfg_.layout.slot_address(s, out.address)) {
                    out.address += flash_base();
                }
                last_reason_ = out.reason;
                ++stats_.factory;
                ++stats_.trials;
                decided_ = true;
                last_decision_ = out;
                last_eval_ms_ = now_ms();
                return Status::Ok;
            }
        }

        out.action = Action::Stay;
        out.reason = Reason::NoImage;
        last_reason_ = out.reason;
        ++stats_.stays;
        return Status::Ok;
    }

    ota::SlotRecord rec{};
    if (read_record(slot, rec) != Status::Ok) {
        out.action = Action::Stay;
        out.reason = Reason::NoImage;
        last_reason_ = out.reason;
        ++stats_.stays;
        return Status::Ok;
    }

    if (static_cast<ota::proto::State>(rec.state) == ota::proto::State::Activated) {
        const bool exhausted = ota::record_take_trial(rec, max_attempts_);
        (void)write_record(slot, rec);
        note(slot, rec.boot_attempts, rec.state);

        if (exhausted) {
            // Разрешение на попытки исчерпано: возвращаемся к прежнему слоту.
            const Status rb = rollback_internal(slot);
            ++stats_.rollbacks;
            if (rb != Status::Ok) {
                out.action = Action::Stay;
                out.reason = Reason::NoRollback;
                last_reason_ = out.reason;
                ++stats_.stays;
                return Status::Ok;
            }
            slot = pick_candidate();
            if (slot == Layout::kNoSlot || read_record(slot, rec) != Status::Ok) {
                out.action = Action::Stay;
                out.reason = Reason::NoImage;
                last_reason_ = out.reason;
                ++stats_.stays;
                return Status::Ok;
            }
        }
    }

    if (rec.image_size == 0u || rec.image_size > cfg_.layout.slot_size[slot]) {
        // Метка есть, а образа по ней нет: так бывает после сбоя питания между
        // записью слота и записью метки. Слот помечается негодным, иначе
        // загрузчик возвращался бы к нему на каждой загрузке.
        ++stats_.bad_vectors;
        (void)store_.invalidate(slot, 1u);
        out.action = Action::Stay;
        out.reason = Reason::BadVector;
        last_reason_ = out.reason;
        ++stats_.stays;
        return Status::Ok;
    }

    // ── Проверка образа ПЕРЕД переходом ──
    const uint8_t* sha = nullptr;
    if (rec.hash_kind == static_cast<uint8_t>(ota::proto::HashKind::Sha256)) {
        sha = rec.sha256;
    }
    const Status vst = validate(slot, rec.image_size, rec.image_crc32, sha);
    if (vst != Status::Ok) {
        // Образ, не прошедший проверку, помечается негодным: иначе загрузчик
        // пытался бы грузить его каждую загрузку, а счётчик пробных загрузок
        // этого не поймал бы (он про пробную ЗАГРУЗКУ, а не про порчу образа).
        (void)store_.invalidate(slot, (vst == Status::NotFound) ? 1u : 2u);
        out.action = Action::Stay;
        out.reason = (vst == Status::NotFound) ? Reason::BadVector : Reason::BadImage;
        last_reason_ = out.reason;
        ++stats_.stays;
        return Status::Ok;
    }

    out.slot       = slot;
    out.image_size = rec.image_size;
    out.attempts   = rec.boot_attempts;
    out.trial      = (static_cast<ota::proto::State>(rec.state) == ota::proto::State::Activated);
    out.action     = Action::Load;
    out.reason     = out.trial ? Reason::TrialBoot : Reason::ImageOk;
    if (cfg_.layout.slot_address(slot, out.address)) {
        out.address += flash_base();
    }
    last_reason_ = out.reason;
    if (out.trial) ++stats_.trials;
    decided_ = true;
    last_decision_ = out;
    last_eval_ms_ = now_ms();

    // Отметка «что пытались грузить» переживает сброс: если прошивка упадёт
    // сразу после перехода, после перезагрузки будет видно, кого винить.
    note(slot, out.attempts, static_cast<uint32_t>(rec.state));
    return Status::Ok;
}

Status Loader::rollback_internal(uint8_t failed_slot) noexcept {
    // Слот, исчерпавший попытки, помечается негодным: возвращаться к нему
    // нельзя, иначе откат вернул бы именно ту прошивку, которая не даёт
    // устройству стартовать.
    ota::SlotRecord failed{};
    if (read_record(failed_slot, failed) == Status::Ok) {
        ota::record_mark_failed(failed, 3u);
        (void)write_record(failed_slot, failed);
    }

    // Кандидат на откат — другой слот, если он годен. Подтверждённый (Running)
    // лучше проверенного: он уже доказывал работоспособность.
    const uint8_t other = (failed_slot == 0u) ? 1u : 0u;
    ota::SlotRecord rec{};
    if (read_record(other, rec) != Status::Ok) return Status::NotFound;
    if (!ota::record_loadable(rec)) return Status::NotFound;

    // Подтверждённый слот (Running) уже доказывал работоспособность: он
    // возвращается в состояние «проверен», и его загрузка НЕ считается пробной —
    // иначе после отката счётчик попыток начал бы считать заново и устройство
    // могло бы «пинговать» между двумя слотами. Непроверенный слот (Verified)
    // наоборот активируется как пробный: если и он не поднимется, загрузчик
    // вернётся сюда же и в конце концов останется в загрузчике (а не будет
    // перезагружаться вечно).
    if (static_cast<ota::proto::State>(rec.state) == ota::proto::State::Running) {
        ota::record_mark_verified(rec);
    } else {
        ota::record_mark_activated(rec);
    }
    const Status st = write_record(other, rec);
    if (st != Status::Ok) return st;
    note(other, 0u, rec.state);
    return Status::Ok;
}

// ─────────────────────────────── step ──────────────────────────────────────

Status Loader::step(Decision& out, uint32_t budget_ms) noexcept {
    if (!inited_) return Status::Unavailable;

    // Пока хост не работает с нами, решение пересматривается: принятый и
    // активированный образ должен загрузиться без «подойди и нажми reset».
    const bool host_busy = (cfg_.host_active != nullptr) &&
                           cfg_.host_active(cfg_.host_ctx);
    const uint64_t now = now_ms();

    // Решение пересматривается, когда:
    //   * его ещё нет;
    //   * СЕССИЯ ХОСТА ЗАКОНЧИЛАСЬ — именно так принятый и активированный
    //     образ попадает в загрузку без «нажми reset» (и без этого загрузчик с
    //     приёмом OTA остался бы в цикле приёма навсегда);
    //   * прошло время с прошлого решения, и системное время идёт.
    // Последнее условие требует РАБОТАЮЩЕГО таймера: образ, у которого время не
    // поднято (например загрузчик без приёма OTA), не должен пересматривать
    // решение по «нулевому» времени — он пересматривает его по событиям.
    const bool session_ended = host_was_busy_ && !host_busy;
    host_was_busy_ = host_busy;
    const bool timed_reeval = !host_busy && config::OsTimer::TimeAvailable() &&
                              ((now - last_eval_ms_) >= kReevaluateMs);
    const bool stale = !decided_ || session_ended || timed_reeval;

    if (stale) {
        const Status st = evaluate(out);
        if (st != Status::Ok && st != Status::InvalidArg) return st;
    } else {
        out = last_decision_;
    }

    if (host_busy) {
        // Хост уже передаёт образ: уходить в приложение сейчас — значит
        // потерять его сессию. Остаёмся и обслуживаем приём.
        out.action = Action::Stay;
        out.reason = Reason::HostSession;
        last_reason_ = out.reason;
        if (!session_counted_) {
            ++stats_.sessions;
            session_counted_ = true;
        }
        return Status::Ok;
    }
    session_counted_ = false;

    if (out.action == Action::Load && window_open_) {
        const Status wst = wait_for_host(budget_ms);
        if (wst == Status::Busy) {
            out.action = Action::Stay;
            out.reason = Reason::HostSession;
            last_reason_ = out.reason;
            return Status::Ok;
        }
        if (wst == Status::Ok) {
            // Окно истекло, хост не пришёл: грузим то, что есть.
            out.reason = Reason::WindowExpired;
            last_reason_ = out.reason;
        } else {
            // Окно ещё открыто: решение — ЖДАТЬ, а не «грузить». Иначе
            // вызывающий ушёл бы в приложение на первом же шаге, и окно не
            // имело бы смысла вовсе.
            out.action = Action::Wait;
        }
    }
    return Status::Ok;
}

Status Loader::wait_for_host(uint32_t budget_ms) noexcept {
    if (!inited_) return Status::Unavailable;
    if (!window_open_) return Status::Ok;

    // Окно отсчитывается от init: это «окно после сброса», а не «сколько
    // ждать каждого хоста».
    if (cfg_.window_ms == 0u || !config::OsTimer::TimeAvailable()) {
        window_open_ = false;
        return Status::Ok;
    }

    const uint64_t spent = now_ms() - window_start_ms_;
    if (spent >= cfg_.window_ms) {
        window_open_ = false;
        return Status::Ok;
    }

    // Приём выполняет тот, кто умеет: колбэк, переданный вызывающим. Загрузчик
    // не держит второго приёмника — иначе протокол пришлось бы реализовывать
    // дважды, а «дважды» здесь означает «однажды по-разному».
    const uint32_t left = static_cast<uint32_t>(cfg_.window_ms - spent);
    const uint32_t slice = (budget_ms != 0u && budget_ms < left) ? budget_ms : left;
    if (cfg_.host_poll != nullptr) cfg_.host_poll(cfg_.host_ctx, slice);

    if (cfg_.host_active != nullptr && cfg_.host_active(cfg_.host_ctx)) {
        return Status::Busy;
    }
    if ((now_ms() - window_start_ms_) >= cfg_.window_ms) {
        window_open_ = false;
        return Status::Ok;
    }
    return Status::Timeout;
}

// ──────────────────────────── Проверка образа ──────────────────────────────

Status Loader::validate(uint8_t slot, uint32_t image_size, uint32_t image_crc32,
                        const uint8_t* sha256) noexcept {
    if (!inited_) return Status::Unavailable;
    if (slot > 1u) return Status::InvalidArg;
    if (image_size == 0u || image_size > cfg_.layout.slot_size[slot]) {
        return Status::NoMemory;
    }

    // ── 1. Вектор сброса ──
    //
    // Самая дешёвая и самая важная проверка: у стёртого участка начальный SP
    // равен 0xFFFFFFFF, у чужого образа — своим. Прыжок в такой «образ» — это
    // гарантированный отказ сразу после перехода, без единой строчки
    // диагностики.
    if (!validate_plausible(slot)) {
        ++stats_.bad_vectors;
        // NotFound именно потому, что это НЕ «испорченный образ», а «не
        // образ»: участок не начинается с вектора сброса (стёрт, чужой или
        // залит чем-то другим). Разные причины — разная диагностика.
        return Status::NotFound;
    }

    // ── 2. Целостность ──
    if (!cfg_.verify_crc) return Status::Ok;

    uint32_t crc = ota::proto::image_crc_begin();
    crypto::Sha2<crypto::Sha2Variant::Sha256> hasher{};
    const bool want_hash = cfg_.verify_hash && (sha256 != nullptr);

    uint32_t done = 0u;
    while (done < image_size) {
        uint32_t chunk = image_size - done;
        if (chunk > sizeof(verify_buf_)) chunk = sizeof(verify_buf_);
        const Status st = cfg_.storage->read(cfg_.layout.slot_offset[slot] + done,
                                            verify_buf_, chunk);
        if (st != Status::Ok) return st;
        crc = ota::proto::image_crc_update(crc, verify_buf_, chunk);
        if (want_hash) hasher.update(verify_buf_, chunk);
        done += chunk;
    }

    if (ota::proto::image_crc_finish(crc) != image_crc32) {
        ++stats_.bad_crc;
        return Status::Corruption;
    }
    if (want_hash) {
        uint8_t digest[32]{};
        hasher.finalize(digest);
        uint8_t diff = 0u;
        for (Size i = 0u; i < 32u; ++i) diff = static_cast<uint8_t>(diff | (digest[i] ^ sha256[i]));
        if (diff != 0u) {
            ++stats_.bad_crc;
            return Status::Corruption;
        }
    }
    return Status::Ok;
}

bool Loader::validate_plausible(uint8_t slot) noexcept {
    if (!inited_ || slot > 1u) return false;
    const platform::BootOps* b = platform::ops().boot;
    if (b == nullptr || b->image_plausible == nullptr) return false;
    const uint32_t addr = flash_base() + cfg_.layout.slot_offset[slot];
    return b->image_plausible(addr, cfg_.layout.slot_size[slot]);
}

// ─────────────────────────────── Переход ───────────────────────────────────

Status Loader::jump(const Decision& d) noexcept {
    if (!inited_) return Status::Unavailable;
    if (d.address == 0u) return Status::InvalidArg;

    const platform::BootOps* b = platform::ops().boot;
    if (b == nullptr || b->jump == nullptr) return Status::NotSupported;

    // Рубеж: сюда попадаем только после проверки образа в evaluate(). На МКУ
    // вызов не возвращается — дальше исполняется загруженный образ.
    if (!b->jump(d.address)) return Status::HardwareError;

    // Стенд: «прыжок» смоделирован, и мы можем проверить его параметры.
    ++stats_.jumps;
    return Status::Ok;
}

uint32_t Loader::note_attempts(uint8_t slot) noexcept {
    const platform::WatchdogOps* wd = platform::ops().watchdog;
    if (wd == nullptr || wd->note_read == nullptr) return 0u;
    if (wd->note_read(cfg_.note_base + ota::kNoteIdxMagic) != ota::kNoteMagic) return 0u;
    const uint32_t noted_slot = wd->note_read(cfg_.note_base + ota::kNoteIdxSlot);
    if (noted_slot != slot) return 0u;          // отметка про ДРУГОЙ слот
    return wd->note_read(cfg_.note_base + ota::kNoteIdxAttempts);
}

void Loader::note(uint32_t slot, uint32_t attempts, uint32_t state) noexcept {
    const platform::WatchdogOps* wd = platform::ops().watchdog;
    if (wd == nullptr || wd->note_write == nullptr) return;
    (void)wd->note_write(cfg_.note_base + ota::kNoteIdxMagic, ota::kNoteMagic);
    (void)wd->note_write(cfg_.note_base + ota::kNoteIdxSlot,
                         static_cast<uint16_t>(slot > 1u ? 0xFFu : slot));
    (void)wd->note_write(cfg_.note_base + ota::kNoteIdxAttempts,
                         static_cast<uint16_t>(attempts & 0xFFFFu));
    (void)wd->note_write(cfg_.note_base + ota::kNoteIdxState,
                         static_cast<uint16_t>(state & 0xFFFFu));
}

// ───────────────────────── Свой адрес и таблица векторов ───────────────────

// Символ приходит из линкер-скрипта (linker_sections.ld) и объявлен СЛАБЫМ:
// в сборке без линкер-скрипта (хостовые тесты) его нет, и тогда функция честно
// сообщает, что адрес неизвестен, вместо «прыжка по нулю».
extern "C" uint32_t _os_image_base[] __attribute__((weak));

uint32_t linked_image_base() noexcept {
    if (_os_image_base == nullptr) return 0u;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_os_image_base));
}

Status relocate_vectors_to_self() noexcept {
    const uint32_t base = linked_image_base();
    if (base == 0u) return Status::NotFound;

    const platform::BootOps* b = platform::ops().boot;
    if (b == nullptr || b->relocate_vectors == nullptr) return Status::NotSupported;

    // Если таблица уже здесь — не трогаем: лишняя запись в VTOR безвредна, но
    // и смысла в ней нет.
    if (b->image_base != nullptr && b->image_base() == base) return Status::Ok;

    b->relocate_vectors(base);
    return Status::Ok;
}

// ─────────────────────── Какой образ исполняется сейчас ────────────────────
//
// Эти две функции нужны и загрузчику (при выборе слота: «свой» слот при равных
// состояниях предпочтительнее), и приложению (чтобы знать, чью загрузку оно
// подтверждает). Обе опираются на VTOR платформы: приложение, собранное для
// слота A и запущенное из слота B, обязано видеть B.

uint32_t Loader::image_base() const noexcept {
    const platform::BootOps* b = platform::ops().boot;
    return (b != nullptr && b->image_base != nullptr) ? b->image_base() : 0u;
}

uint8_t Loader::current_slot() const noexcept {
    const uint32_t base = image_base();
    const uint32_t fb = flash_base();
    if (base < fb) return Layout::kNoSlot;
    return cfg_.layout.slot_of(base - fb);
}

} // namespace embed::boot
