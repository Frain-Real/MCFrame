// dma_manager.hpp
#pragma once
#include "../common.hpp"
#include "../config/dma_config.hpp"

namespace embed {

// ─────────────── Платформенный слой (реализуется в platforms/<target>/) ──────
uint8_t platform_dma_max_channels() noexcept;

/// Контекст, который ядро передаёт колбэку канала DMA.
///
/// ВАЖНО про соглашение: колбэк получает указатель НА ЭТУ структуру, а не
/// значение, переданное в DmaChannel::setCallback(). Сам пользовательский
/// указатель лежит в поле user. Раньше тип контекста объявлялся локально
/// в двух местах (platform.cpp и dma_manager.cpp) и драйвер не мог его
/// назвать: UART-драйвер трактовал аргумент как собственный this и читал
/// поля по мусорному адресу. Теперь тип один на всех.
struct DmaCallbackContext {
    uint8_t channel;   ///< глобальный id канала
    Status  result;    ///< Ok либо HardwareError
    void*   user;      ///< то, что передали в DmaChannel::setCallback()
};

/// Глобальный id канала -> (контроллер, номер канала). Живёт в платформе,
/// чтобы ядро не знало про раскладку DMA1/DMA2.
void global_id_to_hw(uint8_t global_id, uint8_t& ctrl, uint8_t& ch) noexcept;

Status platform_dma_configure_mem_to_periph(
    uint8_t global_id,
    config::DmaRequest request,
    const dma::BufferConfig& src,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::Trigger trig,
    dma::BurstMode burst
) noexcept;

Status platform_dma_configure_periph_to_mem(
    uint8_t global_id,
    config::DmaRequest request,
    const dma::BufferConfig& dst,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::Trigger trig,
    dma::BurstMode burst
) noexcept;

Status platform_dma_configure_mem_to_mem(
    uint8_t global_id,
    const dma::BufferConfig& src,
    const dma::BufferConfig& dst,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::BurstMode burst
) noexcept;

Status platform_dma_start(uint8_t global_id) noexcept;
Status platform_dma_stop(uint8_t global_id) noexcept;
bool   platform_dma_is_complete(uint8_t global_id) noexcept;
/// Возвращает ОСТАТОК в элементах (CNDTR). Прежнее имя bytes_transferred
/// вводило в заблуждение: возвращался не объём переданного.
Size   platform_dma_bytes_remaining(uint8_t global_id) noexcept;
Status platform_dma_set_callback(uint8_t global_id, Callback_h cb, void* arg) noexcept;
Status platform_dma_clear_flags(uint8_t global_id) noexcept;

void platform_dma_irq_handler(uint8_t global_id) noexcept;

class DmaManager;

// ─────────────────────────────── DmaChannel ─────────────────────────────────
// ВАЖНО: это ХЭНДЛ (4 байта), а не владелец состояния. Прежняя версия
// была копией объекта менеджера: setCallback() писал в копию клиента,
// а менеджер читал своё поле, поэтому колбэк не вызывался НИКОГДА,
// а release() не сбрасывал состояние менеджера.
class DmaChannel {
public:
    static constexpr uint8_t kInvalidId = 0xFFu;

    DmaChannel() noexcept = default;

    bool valid() const noexcept { return id_ != kInvalidId; }
    uint8_t id() const noexcept { return id_; }
    const char* owner() const noexcept;

    Status configureMemToPeriph(
        config::DmaRequest request,
        const dma::BufferConfig& src,
        dma::TransferSize unitSize = dma::TransferSize::Byte,
        dma::Priority prio = dma::Priority::Medium,
        dma::Trigger trig = dma::Trigger::Hardware,
        dma::BurstMode burst = dma::BurstMode::Single
    ) noexcept;

    Status configurePeriphToMem(
        config::DmaRequest request,
        const dma::BufferConfig& dst,
        dma::TransferSize unitSize = dma::TransferSize::Byte,
        dma::Priority prio = dma::Priority::Medium,
        dma::Trigger trig = dma::Trigger::Hardware,
        dma::BurstMode burst = dma::BurstMode::Single
    ) noexcept;

    Status configureMemToMem(
        const dma::BufferConfig& src,
        const dma::BufferConfig& dst,
        dma::TransferSize unitSize = dma::TransferSize::Byte,
        dma::Priority prio = dma::Priority::Medium,
        dma::BurstMode burst = dma::BurstMode::Single
    ) noexcept;

    /// Сменить буфер, сохранив прежние параметры передачи.
    Status reconfigureBuffer(void* new_buffer, Size new_bytes) noexcept;

    Status start() noexcept;
    Status stop() noexcept;

    bool isComplete() const noexcept;
    Size bytesRemaining() const noexcept;

    Status setCallback(Callback_h cb, void* arg) noexcept;

private:
    friend class DmaManager;
    explicit DmaChannel(uint8_t id) noexcept : id_(id) {}

    uint8_t id_ = kInvalidId;
};

// ─────────────────────────────── DmaManager ─────────────────────────────────
class DmaManager {
public:
    static DmaManager& instance() noexcept;

    Status init() noexcept;
    bool   is_initialized() const noexcept { return initialized_; }

    /// Захватить канал под запрос. hint_tx выбирает направление,
    /// если для запроса есть и TX, и RX (например SPI/USART).
    ///
    /// ПОВТОРНЫЙ ЗАХВАТ ТОГО ЖЕ ЗАПРОСА не конфликтует сам с собой: канал уже
    /// занят этим запросом, поэтому возвращается ОН ЖЕ, а счётчик ссылок
    /// растёт. Так работает связка «арбитр каналов (dma_arbiter.hpp) →
    /// драйвер»: арбитр удерживает канал на время работы целиком, а драйвер
    /// берёт вторую ссылку на него при старте обмена.
    ///
    /// Освобождать обязан каждый, кто захватил: канал гасится, когда уходит
    /// последняя ссылка. Запрос ДРУГОЙ периферии на том же канале по-прежнему
    /// получает Status::Busy — это аппаратное ограничение F1 (запрос DMA
    /// жёстко привязан к каналу), а не программное.
    Status acquire(config::DmaRequest request,
                   DmaChannel& outChannel,
                   bool hint_tx = true) noexcept;

    Status release(DmaChannel& channel) noexcept;

    /// Свободен ли канал под запрос ДЛЯ НОВОГО ВЛАДЕЛЬЦА: есть ли канал,
    /// который не захвачен НИКЕМ.
    ///
    /// Это строже, чем «acquire() сейчас сработает»: повторный захват того же
    /// запроса (арбитр → драйвер) здесь свободой не считается, потому что
    /// канал уже занят работой. Именно эту проверку делает арбитр
    /// (dma_arbiter.hpp), когда решает, можно ли запускать работу, — и по всем
    /// её каналам сразу, чтобы две работы с парой каналов не встали взаимной
    /// блокировкой.
    bool channelAvailable(config::DmaRequest request) const noexcept;

    /// Сколько ссылок выдано на канал (0 — свободен). Диагностика и тесты.
    uint8_t channelHolders(uint8_t channel_id) const noexcept;

    /// Вызывается платформенным обработчиком прерывания.
    void onTransferComplete(uint8_t channel_id, Status result) noexcept;

    // Доступ к состоянию — только для внутренних нужд ядра (ADC).
    config::DmaRequest request(uint8_t channel_id) const noexcept;
    const dma::BufferConfig* buffer(uint8_t channel_id) const noexcept;
    dma::TransferSize unitSize(uint8_t channel_id) const noexcept;
    dma::Priority     priority(uint8_t channel_id) const noexcept;
    dma::Trigger      trigger(uint8_t channel_id) const noexcept;
    dma::BurstMode    burst(uint8_t channel_id) const noexcept;
    dma::Direction    direction(uint8_t channel_id) const noexcept;

private:
    friend class DmaChannel;   // configure() — внутренняя точка настройки

    DmaManager() noexcept = default;

    /// Максимум каналов, который вообще может быть у платформы.
    static constexpr uint8_t kMaxChannels =
        (config::TOTAL_CHANNELS > 0) ? config::TOTAL_CHANNELS : 1;

    struct ChannelState {
        bool              occupied    = false;
        bool              owned       = false;
        bool              configured  = false;
        /// Сколько ссылок на канал выдано: арбитр + драйвер = 2.
        uint8_t           holders     = 0u;
        config::DmaRequest request    = config::DmaRequest::None;
        dma::Direction    direction   = dma::Direction::MemToMem;
        dma::TransferSize unit_size   = dma::TransferSize::Byte;
        dma::Priority     prio        = dma::Priority::Medium;
        dma::Trigger      trig        = dma::Trigger::Hardware;
        dma::BurstMode    burst       = dma::BurstMode::Single;
        dma::BufferConfig buf{};
        Callback_h        cb          = nullptr;
        void*             cb_arg      = nullptr;
    };

    /// Единая точка настройки. request == None означает «сохранить прежний
    /// запрос» (используется при смене буфера).
    Status configure(uint8_t id, config::DmaRequest request,
                     const dma::BufferConfig& buf, bool tx, bool mem_to_mem,
                     dma::TransferSize unit_size, dma::Priority prio,
                     dma::Trigger trig, dma::BurstMode burst) noexcept;

    ChannelState states_[kMaxChannels];
    bool         initialized_ = false;
};

} // namespace embed
