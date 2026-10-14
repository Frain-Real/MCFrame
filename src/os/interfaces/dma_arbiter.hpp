// dma_arbiter.hpp
#pragma once
// ---------------------------------------------------------------------------
// АРБИТР КАНАЛОВ DMA С ОЧЕРЕДЬЮ РАБОТ.
//
// ЗАЧЕМ
// На STM32F1 запрос DMA жёстко привязан к каналу (remap запросов не
// существует), поэтому два интерфейса могут делить один канал: USART1_TX и
// I2C2_TX — это оба DMA1 Channel4, USART1_RX и I2C2_RX — оба Channel5.
// DmaManager в такой ситуации честно отвечает Busy: второй интерфейс НЕ МОЖЕТ
// начать обмен прямо сейчас. Арбитр превращает этот отказ в ожидание: работа
// встаёт в очередь и выполняется, как только канал освободится.
//
// ЧТО ГАРАНТИРУЕТ
//   * канал занят ровно одной работой: захват идёт через DmaManager, поэтому
//     «проигравший» физически не выставит DMAT/DMAR/DMAEN;
//   * работы с РАЗНЫМИ каналами идут параллельно — очередь не глобальная,
//     а по каналам (глобальная очередь затормозила бы, например, поток АЦП);
//   * на одном интерфейсе (драйвере) одновременно идёт не более одной работы:
//     у драйвера один объект завершения и один активный обмен;
//   * все каналы работы захватываются РАЗОМ: частичный захват позволил бы
//     двум работам с парой каналов встать взаимной блокировкой (A держит Ch6,
//     ждёт Ch7; B держит Ch7, ждёт Ch6);
//   * порядок FIFO: кто раньше попросил — тот раньше и поедет.
//
// ЧЕГО НЕ ДЕЛАЕТ
//   * не делит один канал между двумя перифериями «по времени внутри одной
//     операции» — это невозможно аппаратно; работы на общем канале строго
//     последовательны;
//   * не приоритизирует работы (нужен приоритет — добавьте поле в DmaJob и
//     сравнение в pickNextLocked());
//   * не защищает от кода, который лезет к DMA мимо DmaManager;
//   * не ждёт освобождения интерфейса, занятого обменом ВНЕ арбитра: такая
//     работа завершится статусом, который вернул драйвер (обычно Busy).
//
// ТИПИЧНОЕ ИСПОЛЬЗОВАНИЕ
//   Порт (линейно, как обычный write/read — сам встанет в очередь):
//       embed::async::UartPort<UartDriver1> uart;
//       uart.writeQueued("payload", 7u, 100u);
//
//   Напрямую (асинхронно):
//       DmaJob job{};
//       job.tx    = UartDriver1::kTxRequest;
//       job.owner = &UartDriver1::instance();
//       job.start = &my_start;
//       job.ctx   = &my_ctx;
//       DmaTicket t;
//       if (DmaArbiter::instance().submit(job, t) == Status::Ok) { ... }
//
// ВАЖНО ПРО ВРЕМЯ ЖИЗНИ: ctx (и буферы внутри него) обязан жить до
// завершения работы. Линейные обёртки порта ждут завершения внутри вызова,
// поэтому держат параметры на своём стеке — там это безопасно.
// ---------------------------------------------------------------------------
#include "../common.hpp"
#include "../config/dma_config.hpp"
#include "dma_manager.hpp"
#include "../async/async_completion.hpp"

namespace embed {

/// Запуск работы: арбитр уже удерживает каналы, драйвер должен начать обмен.
/// Объект завершения выдаёт арбитр — тот же, который он потом и освободит.
using DmaStartFn = Status (*)(void* ctx, async::completion& token) noexcept;

/// Снятие работы: обычно driver.abort(). Нужно, если работу отменили или
/// истёк её таймаут, пока она была в работе.
using DmaCancelFn = void (*)(void* ctx) noexcept;

/// Описание работы для арбитра.
///
/// Работа — это ОДИН обмен (передача, приём или writeRead). Каналы берутся из
/// таблицы RM0008 (config::mappings), поэтому работы, которым нужен один и тот
/// же канал, арбитр выполняет по очереди.
struct DmaJob {
    /// Канал передачи: None, если передача не нужна.
    config::DmaRequest tx = config::DmaRequest::None;
    /// Канал приёма: None, если приём не нужен.
    ///
    /// Оба поля сразу нужны операции «запись адреса регистра + чтение» (I2C
    /// writeRead): у неё разные каналы на фазах, но занимать их надо разом.
    config::DmaRequest rx = config::DmaRequest::None;

    /// Интерфейс-владелец (обычно &Driver::instance()): на нём одновременно
    /// идёт не более одной работы.
    void*       owner  = nullptr;
    DmaStartFn  start  = nullptr;
    DmaCancelFn cancel = nullptr;
    /// Параметры работы; живут, пока работа не завершится.
    void*       ctx    = nullptr;
    /// Имя для диагностики (необязательно).
    const char* name   = nullptr;
};

/// Билет на принятую работу: слот очереди + его поколение.
///
/// Поколение (seq) не даёт использовать билет после того, как слот освободили
/// и отдали другой работе.
struct DmaTicket {
    static constexpr uint16_t kInvalidSlot = 0xFFFFu;

    uint16_t slot = kInvalidSlot;
    uint16_t seq  = 0u;

    constexpr bool valid() const noexcept { return slot != kInvalidSlot; }
};

/// Арбитр каналов DMA. Единственный экземпляр, вся память статическая.
class DmaArbiter {
public:
    /// Состояние работы.
    enum class State : uint8_t {
        Free = 0,   ///< слот свободен
        Queued,     ///< принята, ждёт канал
        Running,    ///< обмен идёт
        Done,       ///< завершена, результат в status()/transferred()
    };

    /// Диагностика.
    struct Stats {
        uint32_t submitted = 0u;   ///< всего принято работ
        uint32_t started   = 0u;   ///< из них дошло до обмена
        uint32_t completed = 0u;   ///< завершилось успешно
        uint32_t failed    = 0u;   ///< завершилось ошибкой
        uint32_t cancelled = 0u;   ///< снято до завершения
        uint32_t overflow  = 0u;   ///< отказов из-за полной очереди
        uint32_t max_wait_ms = 0u; ///< худшее ожидание канала, мс
        uint16_t queued  = 0u;     ///< сейчас в очереди
        uint16_t running = 0u;     ///< сейчас в работе
    };

    static DmaArbiter& instance() noexcept;

    // ─────────────────────────── Приём работ ───────────────────────────

    /// Принять работу асинхронно. Можно вызывать и из прерывания.
    ///
    /// Ok        — принята: либо уже запущена, либо стоит в очереди;
    /// NoMemory  — очередь заполнена (см. config::DMA_ARBITER_JOBS);
    /// InvalidArg — работа описана неверно (нет start/owner/каналов).
    Status submit(const DmaJob& job, DmaTicket& out) noexcept;

    /// Принять работу и дождаться её завершения (линейно).
    ///
    /// timeout_ms отсчитывается от приёма работы, то есть включает и ожидание
    /// в очереди. 0 — ждать без ограничения.
    ///
    /// ТОЛЬКО из задачи (или до старта планировщика): вызов ждёт завершения,
    /// а из обработчика прерывания ждать нельзя — для ISR есть submit().
    Status run(const DmaJob& job, uint32_t timeout_ms, Size& transferred) noexcept;

    /// Дождаться завершения принятой работы (без ограничения по времени).
    /// Нужен, если работа принята submit(); для работ с таймаутом не нужен.
    void waitDone(DmaTicket t) noexcept;

    // ─────────────────────────── Результат ───────────────────────────

    bool   ready(DmaTicket t) const noexcept;
    State  state(DmaTicket t) const noexcept;
    Status status(DmaTicket t) const noexcept;
    Size   transferred(DmaTicket t) const noexcept;

    /// Снять работу: из очереди — сразу, в работе — через cancel() драйвера.
    Status cancel(DmaTicket t) noexcept;

    /// Освободить слот. Обязательно после submit()+ожидания, иначе очередь
    /// заполнится навсегда. Для run() вызывается автоматически.
    ///
    /// Работа, которая ещё стоит в очереди, при этом снимается; работа в
    /// процессе НЕ освобождается (её нужно сначала cancel() и дождаться).
    void release(DmaTicket t) noexcept;

    // ─────────────────────────── Диагностика ───────────────────────────

    Stats stats() const noexcept;
    /// Сколько работ ждёт канал прямо сейчас.
    Size  queued() const noexcept;
    /// Сколько байт занимает арбитр (для отчёта о RAM).
    static constexpr Size static_bytes() noexcept {
        return sizeof(DmaArbiter);
    }

private:
    /// Слот очереди. Объект завершения лежит ЗДЕСЬ, а не у вызывающего:
    /// он живёт столько же, сколько слот, поэтому «повиснуть» на стеке
    /// вызывающего не может.
    struct Slot {
        State              state = State::Free;
        DmaJob             job{};
        async::completion  done{};
        DmaChannel         tx_ch{};
        DmaChannel         rx_ch{};
        Status             result = Status::Busy;
        Size               transferred = 0u;
        uint32_t           order = 0u;      ///< порядок приёма (FIFO)
        uint32_t           queued_ms = 0u;  ///< когда приняли (для замера ожидания)
        uint32_t           wait_ms = 0u;    ///< сколько ждал канал
        bool               started = false;
        uint16_t           seq = 0u;        ///< поколение слота
    };

    DmaArbiter() noexcept = default;

    DmaArbiter(const DmaArbiter&)            = delete;
    DmaArbiter& operator=(const DmaArbiter&) = delete;

    Status submitLocked(const DmaJob& job, uint32_t timeout_ms, DmaTicket& out) noexcept;

    /// Раздать каналы: запустить все работы, которым сейчас есть и канал,
    /// и свободный интерфейс. Вызывается при приёме работы и при завершении.
    void dispatchLocked() noexcept;
    bool startNextLocked() noexcept;
    bool startSlotLocked(Slot& s) noexcept;

    bool channelsFree(const DmaJob& job) const noexcept;
    bool acquireChannels(Slot& s) noexcept;
    void releaseChannels(Slot& s) noexcept;
    bool ownerBusy(const void* owner) const noexcept;

    /// Завершение работы (колбэк объекта завершения слота).
    static void onDone(const async::Completion& result, void* arg) noexcept;
    void settle(Slot& s, Status st, Size n) noexcept;

    Slot*       find(DmaTicket t)       noexcept;
    const Slot* find(DmaTicket t) const noexcept;

    Slot     slots_[config::DMA_ARBITER_JOBS]{};
    uint32_t order_        = 0u;
    bool     dispatching_  = false;
    bool     need_dispatch_ = false;
    Stats    stats_{};
};

} // namespace embed
