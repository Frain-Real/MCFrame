// dma_arbiter.cpp
// ---------------------------------------------------------------------------
// Арбитр каналов DMA: очередь работ и раздача каналов (см. dma_arbiter.hpp).
//
// КЛЮЧЕВЫЕ РЕШЕНИЯ
//  * состояние очереди меняют и задача (submit), и обработчик прерывания
//    (завершение работы), поэтому все переходы идут под критической секцией;
//  * запуск следующей работы может произойти ПРЯМО ВНУТРИ job.start() —
//    например I2C обнаружил NACK и завершил обмен синхронно. Отсюда флаг
//    dispatching_: вложенный вызов не запускает работы рекурсивно, а лишь
//    просит внешний цикл пройти очередь ещё раз;
//  * объект завершения принадлежит СЛОТУ арбитра, а не вызывающему: работа
//    может ждать канал сколь угодно долго, и указатель не должен «повиснуть»
//    на стеке того, кто её заказал.
// ---------------------------------------------------------------------------
#include "dma_arbiter.hpp"
#include "../async/async_kernel.hpp"   // kernel::has_time/time_ms — для замеров

namespace embed {

DmaArbiter& DmaArbiter::instance() noexcept {
    // Статический POD без динамической инициализации — как у DmaManager:
    // Meyers-singleton требует __cxa_guard_*, которых нет при -nostdlib.
    static DmaArbiter arbiter;
    return arbiter;
}

// ──────────────────────────────── Приём работ ──────────────────────────────

Status DmaArbiter::submit(const DmaJob& job, DmaTicket& out) noexcept {
    return submitLocked(job, /*timeout_ms*/ 0u, out);
}

Status DmaArbiter::run(const DmaJob& job, uint32_t timeout_ms, Size& transferred) noexcept {
    transferred = 0u;

    DmaTicket t;
    const Status st = submitLocked(job, timeout_ms, t);
    if (st != Status::Ok) return st;

    // Ждём завершения (или таймаута — его выставит объект завершения).
    slots_[t.slot].done.wait();

    const Status result = slots_[t.slot].result;
    transferred = slots_[t.slot].transferred;
    release(t);
    return result;
}

Status DmaArbiter::submitLocked(const DmaJob& job, uint32_t timeout_ms,
                                DmaTicket& out) noexcept {
    out = DmaTicket{};

    if (job.start == nullptr || job.owner == nullptr) return Status::InvalidArg;
    if (job.tx == config::DmaRequest::None && job.rx == config::DmaRequest::None) {
        return Status::InvalidArg;
    }

    DmaManager& mgr = DmaManager::instance();
    if (!mgr.is_initialized() && mgr.init() != Status::Ok) return Status::Fault;

    Lock lock;

    Slot* slot = nullptr;
    for (auto& s : slots_) {
        if (s.state == State::Free) { slot = &s; break; }
    }
    if (slot == nullptr) {
        ++stats_.overflow;
        return Status::NoMemory;
    }

    slot->job         = job;
    slot->result      = Status::Busy;
    slot->transferred = 0u;
    slot->started     = false;
    slot->tx_ch       = DmaChannel{};
    slot->rx_ch       = DmaChannel{};
    slot->wait_ms     = 0u;
    slot->queued_ms   = async::kernel::has_time()
                      ? static_cast<uint32_t>(async::kernel::time_ms()) : 0u;
    slot->order       = ++order_;
    slot->state       = State::Queued;

    // Таймаут считается от приёма работы: он включает и ожидание в очереди —
    // иначе «застрявшая» за занятым каналом работа ждала бы вечно.
    slot->done.reset(timeout_ms == 0u ? async::WaitPolicy::Forever
                                      : async::WaitPolicy::Timeout,
                     timeout_ms, &onDone, slot);
    slot->done.begin();

    ++stats_.submitted;

    out.slot = static_cast<uint16_t>(slot - slots_);
    out.seq  = slot->seq;

    dispatchLocked();
    return Status::Ok;
}

void DmaArbiter::waitDone(DmaTicket t) noexcept {
    Lock lock;
    Slot* s = find(t);
    if (s == nullptr) return;
    if (s->state == State::Done) return;

    // Ждать нужно БЕЗ удержания критической секции, иначе обработчик DMA
    // не сможет завершить работу и мы получим взаимную блокировку.
    unlock_early(lock);
    s->done.wait();
}

// ─────────────────────────────── Результат ─────────────────────────────────

bool DmaArbiter::ready(DmaTicket t) const noexcept {
    const Slot* s = find(t);
    return s != nullptr && s->state == State::Done;
}

DmaArbiter::State DmaArbiter::state(DmaTicket t) const noexcept {
    const Slot* s = find(t);
    return (s != nullptr) ? s->state : State::Free;
}

Status DmaArbiter::status(DmaTicket t) const noexcept {
    const Slot* s = find(t);
    return (s != nullptr) ? s->result : Status::InvalidArg;
}

Size DmaArbiter::transferred(DmaTicket t) const noexcept {
    const Slot* s = find(t);
    return (s != nullptr) ? s->transferred : 0u;
}

Status DmaArbiter::cancel(DmaTicket t) noexcept {
    Lock lock;

    Slot* s = find(t);
    if (s == nullptr) return Status::InvalidArg;

    if (s->state == State::Queued) {
        // Ещё не начинали: каналов нет, снимаем из очереди.
        s->result = Status::Aborted;
        s->state  = State::Done;
        ++stats_.cancelled;
        need_dispatch_ = true;
        dispatchLocked();
        return Status::Ok;
    }

    if (s->state == State::Running) {
        // Работа идёт: снимать обмен должен драйвер — он снимет биты DMA
        // и вызовет завершение, которое освободит каналы в settle().
        if (s->job.cancel != nullptr) s->job.cancel(s->job.ctx);

        if (s->state == State::Running) {
            // Драйвер не завершил работу сам. Оставлять слот занятым нельзя:
            // отпускаем каналы и закрываем работу, иначе очередь встанет
            // (счётчик отмен увеличит settle()).
            settle(*s, Status::Aborted, s->transferred);
        }
        return Status::Ok;
    }

    return Status::Ok;      // уже завершена
}

void DmaArbiter::release(DmaTicket t) noexcept {
    Lock lock;

    Slot* s = find(t);
    if (s == nullptr) return;

    if (s->state == State::Running) {
        // Работу нельзя «забыть» на середине: сначала cancel() и дождаться.
        return;
    }

    if (s->state == State::Queued) {
        // Ещё не начинали: каналов работа не держит, снимаем из очереди.
        ++stats_.cancelled;
    }

    s->job         = DmaJob{};
    s->result      = Status::Busy;
    s->transferred = 0u;
    s->started     = false;
    s->wait_ms     = 0u;
    ++s->seq;                      // поколение: старый билет больше не действителен
    s->state       = State::Free;
}

// ─────────────────────────── Раздача каналов ───────────────────────────────

void DmaArbiter::dispatchLocked() noexcept {
    if (dispatching_) {
        // Мы внутри запуска работы: следующий проход сделает внешний цикл.
        need_dispatch_ = true;
        return;
    }

    dispatching_ = true;
    do {
        need_dispatch_ = false;
        while (startNextLocked()) { /* запускаем, пока есть готовые */ }
    } while (need_dispatch_);
    dispatching_ = false;
}

bool DmaArbiter::channelsFree(const DmaJob& job) const noexcept {
    const DmaManager& mgr = DmaManager::instance();

    if (job.tx != config::DmaRequest::None && !mgr.channelAvailable(job.tx)) return false;
    if (job.rx != config::DmaRequest::None && !mgr.channelAvailable(job.rx)) return false;
    return true;
}

bool DmaArbiter::ownerBusy(const void* owner) const noexcept {
    for (const auto& s : slots_) {
        if (s.state == State::Running && s.job.owner == owner) return true;
    }
    return false;
}

bool DmaArbiter::startNextLocked() noexcept {
    // Берём САМУЮ РАННЮЮ по порядку приёма работу из числа готовых к запуску.
    // Работа, которой канал занят, очередь не блокирует: у остальных каналы
    // свободны и они должны ехать параллельно.
    Slot* best = nullptr;
    for (auto& s : slots_) {
        if (s.state != State::Queued) continue;
        if (ownerBusy(s.job.owner)) continue;
        if (!channelsFree(s.job)) continue;
        if (best == nullptr || s.order < best->order) best = &s;
    }
    if (best == nullptr) return false;
    return startSlotLocked(*best);
}

bool DmaArbiter::acquireChannels(Slot& s) noexcept {
    DmaManager& mgr = DmaManager::instance();

    if (s.job.tx != config::DmaRequest::None) {
        if (mgr.acquire(s.job.tx, s.tx_ch) != Status::Ok) return false;
    }
    if (s.job.rx != config::DmaRequest::None) {
        if (mgr.acquire(s.job.rx, s.rx_ch) != Status::Ok) {
            // Второй канал не дали — отпускаем первый, работа остаётся в
            // очереди целой (никакого частичного владения).
            if (s.tx_ch.valid()) (void)mgr.release(s.tx_ch);
            s.tx_ch = DmaChannel{};
            return false;
        }
    }
    return true;
}

void DmaArbiter::releaseChannels(Slot& s) noexcept {
    DmaManager& mgr = DmaManager::instance();

    if (s.tx_ch.valid()) { (void)mgr.release(s.tx_ch); s.tx_ch = DmaChannel{}; }
    if (s.rx_ch.valid()) { (void)mgr.release(s.rx_ch); s.rx_ch = DmaChannel{}; }
}

bool DmaArbiter::startSlotLocked(Slot& s) noexcept {
    if (!acquireChannels(s)) return false;

    s.state   = State::Running;
    s.started = true;
    ++stats_.started;

    // Сколько работа ждала канал — диагностика «кто кого задерживает».
    if (async::kernel::has_time()) {
        s.wait_ms = static_cast<uint32_t>(async::kernel::time_ms()) - s.queued_ms;
        if (s.wait_ms > stats_.max_wait_ms) stats_.max_wait_ms = s.wait_ms;
    }

    const Status st = s.job.start(s.job.ctx, s.done);

    // Работа могла завершиться ПРЯМО ВНУТРИ start() (например I2C увидел
    // NACK и выставил завершение синхронно) — тогда её уже закрыл settle().
    if (s.state != State::Running) return true;

    if (st != Status::Ok) {
        releaseChannels(s);
        ++stats_.failed;
        s.result = st;
        s.state  = State::Done;
        return true;
    }
    return true;
}

// ────────────────────────────── Завершение ─────────────────────────────────

void DmaArbiter::onDone(const async::Completion& result, void* arg) noexcept {
    if (arg == nullptr) return;
    auto* s = static_cast<Slot*>(arg);
    instance().settle(*s, result.status, result.transferred);
}

void DmaArbiter::settle(Slot& s, Status st, Size n) noexcept {
    Lock lock;

    if (s.state == State::Done || s.state == State::Free) return;

    s.result      = st;
    s.transferred = n;

    if (s.started) {
        // Таймаут срабатывает на стороне арбитра: обмен ещё идёт, и снять его
        // обязан драйвер (снять биты DMA и закрыть операцию).
        //
        // Aborted сюда приходит ОТ драйвера (его abort()), поэтому повторно
        // звать cancel() нельзя — это был бы второй abort подряд.
        if (st == Status::Timeout && s.job.cancel != nullptr) {
            s.job.cancel(s.job.ctx);
        }
        releaseChannels(s);
    }

    if (st == Status::Ok)      ++stats_.completed;
    else if (st != Status::Aborted && st != Status::Timeout) ++stats_.failed;
    if (st == Status::Aborted || st == Status::Timeout) ++stats_.cancelled;

    s.state = State::Done;

    need_dispatch_ = true;
    dispatchLocked();
}

// ─────────────────────────── Диагностика и поиск ───────────────────────────

DmaArbiter::Stats DmaArbiter::stats() const noexcept {
    Stats out = stats_;
    out.queued  = 0u;
    out.running = 0u;
    for (const auto& s : slots_) {
        if (s.state == State::Queued)  ++out.queued;
        if (s.state == State::Running) ++out.running;
    }
    return out;
}

Size DmaArbiter::queued() const noexcept {
    Size n = 0u;
    for (const auto& s : slots_) {
        if (s.state == State::Queued) ++n;
    }
    return n;
}

DmaArbiter::Slot* DmaArbiter::find(DmaTicket t) noexcept {
    if (!t.valid() || t.slot >= config::DMA_ARBITER_JOBS) return nullptr;
    Slot& s = slots_[t.slot];
    if (s.state == State::Free || s.seq != t.seq) return nullptr;
    return &s;
}

const DmaArbiter::Slot* DmaArbiter::find(DmaTicket t) const noexcept {
    if (!t.valid() || t.slot >= config::DMA_ARBITER_JOBS) return nullptr;
    const Slot& s = slots_[t.slot];
    if (s.state == State::Free || s.seq != t.seq) return nullptr;
    return &s;
}

} // namespace embed
