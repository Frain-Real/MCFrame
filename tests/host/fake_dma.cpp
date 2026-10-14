// tests/host/fake_dma.cpp
// ---------------------------------------------------------------------------
// Модель DMA1: реализация контракта embed::platform_dma_* для тестов на ПК
// (см. fake_dma.hpp). Семантика повторяет platforms/stm32/platform.cpp:
// каналы DMA1 Ch1..Ch7, глобальный id = индекс канала, CPAR/CMAR/CNDTR,
// флаги TC/TE, колбэк с DmaCallbackContext.
//
// Ключевое отличие от реального контроллера: перенос делает модель, и только
// тогда, когда периферия готова (TXE для передачи, RXNE для приёма). Поэтому
// «мгновенного» DMA не бывает: если драйвер не выставил DMAT/DMAR/DMAEN или
// неверно настроил канал, перенос просто не идёт — тест это увидит.
// ---------------------------------------------------------------------------
#include "fake_dma.hpp"
#include "stm32_regs_fake.hpp"

#include "os/config/dma_config.hpp"
#include "os/interfaces/dma_manager.hpp"

namespace embed {
namespace {

using embed::test::g_i2c1;
using embed::test::g_usart1;
using embed::test::I2cModel;
using embed::test::UsartModel;

// ─────────────────────────── Маршрут «запрос -> периферия» ──────────────────
//
// На реальном МКУ CPAR — адрес регистра DR. Здесь адрес тоже вычисляется
// (в модели регистров он есть), но перенос идёт через хуки: так модель видит
// каждый байт и может сказать, готова ли периферия.
struct Route {
    config::DmaRequest request;
    uint32_t           addr;     // «CPAR»: адрес DR в модели регистров
    void*              ctx;
    bool (*gate)(void*);         // разрешены ли DMA-запросы периферии
    bool (*push)(void*, uint8_t);
    bool (*pop)(void*, uint8_t&);
};

bool uartTxGate(void* ctx) noexcept {
    return static_cast<UsartModel*>(ctx)->dmaTxEnabled();
}
bool uartRxGate(void* ctx) noexcept {
    return static_cast<UsartModel*>(ctx)->dmaRxEnabled();
}
bool i2cGate(void* ctx) noexcept {
    return static_cast<I2cModel*>(ctx)->dmaEnabled();
}
bool uartPush(void* ctx, uint8_t b) noexcept {
    return static_cast<UsartModel*>(ctx)->dmaPush(b);
}
bool uartPop(void* ctx, uint8_t& b) noexcept {
    return static_cast<UsartModel*>(ctx)->dmaPop(b);
}
bool i2cPush(void* ctx, uint8_t b) noexcept {
    return static_cast<I2cModel*>(ctx)->dmaPush(b);
}
bool i2cPop(void* ctx, uint8_t& b) noexcept {
    return static_cast<I2cModel*>(ctx)->dmaPop(b);
}

#define DR_ADDR(model) \
    (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&(model).regs.DR)))

const Route kRoutes[] = {
    { config::DmaRequest::Usart1Tx, DR_ADDR(g_usart1), &g_usart1, &uartTxGate, &uartPush, nullptr   },
    { config::DmaRequest::Usart1Rx, DR_ADDR(g_usart1), &g_usart1, &uartRxGate, nullptr,   &uartPop  },
    { config::DmaRequest::I2c1Tx,   DR_ADDR(g_i2c1),   &g_i2c1,   &i2cGate,    &i2cPush,   nullptr   },
    { config::DmaRequest::I2c1Rx,   DR_ADDR(g_i2c1),   &g_i2c1,   &i2cGate,    nullptr,   &i2cPop   },
};

#undef DR_ADDR

const Route* findRoute(config::DmaRequest request) noexcept {
    for (const auto& r : kRoutes) {
        if (r.request == request) return &r;
    }
    return nullptr;
}

// ─────────────────────────────── Состояние канала ───────────────────────────

struct Channel {
    bool         enabled       = false;
    bool         mem_to_periph = true;
    bool         mem_inc       = true;
    uint32_t     cpar          = 0u;
    uint8_t*     mem           = nullptr;
    Size         cndtr         = 0u;   // остаток
    Size         total         = 0u;   // сколько было всего
    bool         error         = false;   // следующий перенос завершится TE
    bool         tc            = false;
    bool         te            = false;
    Callback_h   cb            = nullptr;
    void*        arg           = nullptr;
    const Route* route         = nullptr;
};

Channel g_channels[config::TOTAL_CHANNELS]{};

bool validId(uint8_t global_id) noexcept {
    return global_id < config::TOTAL_CHANNELS;
}

void clearFlags(Channel& ch) noexcept {
    ch.tc = false;
    ch.te = false;
}

} // namespace

// ─────────────────────────── Публичный контракт ─────────────────────────────

void global_id_to_hw(uint8_t global_id, uint8_t& ctrl, uint8_t& ch) noexcept {
    // Для F103RB контроллер всегда DMA1 (как в platform.cpp).
    ctrl = 1u + (global_id / config::DMA1_CHANNELS);
    ch   = 1u + (global_id % config::DMA1_CHANNELS);
}

uint8_t platform_dma_max_channels() noexcept {
    return config::TOTAL_CHANNELS;
}

Status platform_dma_configure_mem_to_periph(
    uint8_t global_id,
    config::DmaRequest request,
    const dma::BufferConfig& src,
    dma::TransferSize unitSize,
    dma::Priority /*prio*/,
    dma::Trigger /*trig*/,
    dma::BurstMode /*burst*/
) noexcept {
    if (!validId(global_id)) return Status::Unavailable;
    if (!src.buffer || src.bytes == 0u) return Status::InvalidArg;
    if (unitSize != dma::TransferSize::Byte) return Status::NotSupported;

    const Route* r = findRoute(request);
    if (r == nullptr || r->push == nullptr) return Status::NotSupported;

    Channel& ch = g_channels[global_id];
    clearFlags(ch);
    ch.route         = r;
    ch.cpar          = r->addr;
    ch.mem           = static_cast<uint8_t*>(src.buffer);
    ch.mem_inc       = src.increment;
    ch.mem_to_periph = true;
    ch.total         = src.bytes;
    ch.cndtr         = src.bytes;
    ch.enabled       = false;
    return Status::Ok;
}

Status platform_dma_configure_periph_to_mem(
    uint8_t global_id,
    config::DmaRequest request,
    const dma::BufferConfig& dst,
    dma::TransferSize unitSize,
    dma::Priority /*prio*/,
    dma::Trigger /*trig*/,
    dma::BurstMode /*burst*/
) noexcept {
    if (!validId(global_id)) return Status::Unavailable;
    if (!dst.buffer || dst.bytes == 0u) return Status::InvalidArg;
    if (unitSize != dma::TransferSize::Byte) return Status::NotSupported;

    const Route* r = findRoute(request);
    if (r == nullptr || r->pop == nullptr) return Status::NotSupported;

    Channel& ch = g_channels[global_id];
    clearFlags(ch);
    ch.route         = r;
    ch.cpar          = r->addr;
    ch.mem           = static_cast<uint8_t*>(dst.buffer);
    ch.mem_inc       = dst.increment;
    ch.mem_to_periph = false;
    ch.total         = dst.bytes;
    ch.cndtr         = dst.bytes;
    ch.enabled       = false;
    return Status::Ok;
}

Status platform_dma_configure_mem_to_mem(
    uint8_t /*global_id*/,
    const dma::BufferConfig& /*src*/,
    const dma::BufferConfig& /*dst*/,
    dma::TransferSize /*unitSize*/,
    dma::Priority /*prio*/,
    dma::BurstMode /*burst*/
) noexcept {
    // Драйверы UART/I2C память-в-память не используют: моделировать нечего,
    // а «молча сделать вид» здесь означало бы скрыть ошибку вызова.
    return Status::NotSupported;
}

Status platform_dma_start(uint8_t global_id) noexcept {
    if (!validId(global_id)) return Status::Unavailable;

    Channel& ch = g_channels[global_id];
    if (ch.route == nullptr) return Status::Unavailable;
    if (ch.enabled)         return Status::Busy;
    if (ch.cndtr == 0u)     return Status::InvalidArg;

    clearFlags(ch);
    ch.enabled = true;
    return Status::Ok;
}

Status platform_dma_stop(uint8_t global_id) noexcept {
    if (!validId(global_id)) return Status::Unavailable;

    Channel& ch = g_channels[global_id];
    ch.enabled = false;
    clearFlags(ch);
    return Status::Ok;
}

bool platform_dma_is_complete(uint8_t global_id) noexcept {
    return validId(global_id) ? g_channels[global_id].tc : false;
}

Size platform_dma_bytes_remaining(uint8_t global_id) noexcept {
    return validId(global_id) ? g_channels[global_id].cndtr : 0u;
}

Status platform_dma_set_callback(uint8_t global_id, Callback_h cb, void* arg) noexcept {
    if (!validId(global_id)) return Status::Unavailable;
    g_channels[global_id].cb  = cb;
    g_channels[global_id].arg = arg;
    return Status::Ok;
}

Status platform_dma_clear_flags(uint8_t global_id) noexcept {
    if (!validId(global_id)) return Status::Unavailable;
    clearFlags(g_channels[global_id]);
    return Status::Ok;
}

void platform_dma_irq_handler(uint8_t global_id) noexcept {
    if (!validId(global_id)) return;

    Channel& ch = g_channels[global_id];
    if (!ch.tc && !ch.te) return;

    const Status result = ch.te ? Status::HardwareError : Status::Ok;
    clearFlags(ch);

    Callback_h cb  = ch.cb;
    void*      arg = ch.arg;
    if (cb == nullptr) return;

    // Тот же контракт, что и в platform.cpp: драйвер читает свой указатель
    // из поля user, а не из самого аргумента.
    DmaCallbackContext ctx{ global_id, result, arg };
    cb(&ctx);
}

// ─────────────────────────── Тестовый интерфейс ─────────────────────────────

namespace test {

Size fakeDmaService(uint8_t global_id, Size max_steps) {
    if (!validId(global_id)) return 0u;

    Channel& ch = g_channels[global_id];
    if (!ch.enabled || ch.route == nullptr || ch.cndtr == 0u) return 0u;

    Size moved = 0u;
    for (Size step = 0u; step < max_steps && ch.cndtr > 0u; ++step) {
        // Периферия должна разрешить DMA-запросы (DMAT/DMAR у USART,
        // DMAEN у I2C). Без этого переноса не будет — как на железе.
        if (ch.route->gate != nullptr && !ch.route->gate(ch.route->ctx)) break;

        const Size index = ch.mem_inc ? (ch.total - ch.cndtr) : 0u;

        if (ch.mem_to_periph) {
            if (ch.route->push == nullptr) break;
            if (!ch.route->push(ch.route->ctx, ch.mem[index])) break;
        } else {
            if (ch.route->pop == nullptr) break;
            uint8_t b = 0u;
            if (!ch.route->pop(ch.route->ctx, b)) break;
            ch.mem[index] = b;
        }

        --ch.cndtr;
        ++moved;
    }

    if (ch.cndtr == 0u) {
        ch.enabled = false;
        if (ch.error) { ch.te = true; ch.error = false; }
        else          { ch.tc = true; }
    }
    return moved;
}

uint32_t fakeDmaServiceAll(Size max_steps) {
    uint32_t done = 0u;
    for (uint8_t id = 0u; id < config::TOTAL_CHANNELS; ++id) {
        (void)fakeDmaService(id, max_steps);
        if (g_channels[id].tc || g_channels[id].te) done |= (1u << id);
    }
    return done;
}

void fakeDmaIrq(uint8_t global_id) {
    platform_dma_irq_handler(global_id);
}

bool fakeDmaComplete(uint8_t global_id) {
    if (!validId(global_id)) return false;
    return g_channels[global_id].tc || g_channels[global_id].te;
}

bool fakeDmaBusy(uint8_t global_id) {
    return validId(global_id) ? g_channels[global_id].enabled : false;
}

Size fakeDmaRemaining(uint8_t global_id) {
    return platform_dma_bytes_remaining(global_id);
}

Size fakeDmaTotal(uint8_t global_id) {
    return validId(global_id) ? g_channels[global_id].total : 0u;
}

Size fakeDmaTransferred(uint8_t global_id) {
    if (!validId(global_id)) return 0u;
    const Channel& ch = g_channels[global_id];
    return (ch.total >= ch.cndtr) ? (ch.total - ch.cndtr) : 0u;
}

void fakeDmaSetError(uint8_t global_id) {
    if (!validId(global_id)) return;
    g_channels[global_id].error = true;
}

void fakeDmaReset() {
    for (auto& ch : g_channels) ch = Channel{};
}

} // namespace test
} // namespace embed
