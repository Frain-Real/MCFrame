// dma_manager.cpp
#include "dma_manager.hpp"

namespace embed {

// ─────────────────────────────── DmaManager ─────────────────────────────────

DmaManager& DmaManager::instance() noexcept {
    // Статический POD без динамической инициализации: Meyers-singleton
    // требовал __cxa_guard_*, которых нет при -nostdlib.
    static DmaManager mgr;
    return mgr;
}

Status DmaManager::init() noexcept {
    Lock lock;
    if (initialized_) return Status::Ok;

    const uint8_t max_ch = platform_dma_max_channels();
    const uint8_t limit  = (max_ch < config::TOTAL_CHANNELS) ? max_ch
                                                             : config::TOTAL_CHANNELS;

    for (uint8_t i = 0; i < config::TOTAL_CHANNELS; ++i) {
        states_[i] = ChannelState{};
    }

    // Каналы сверх аппаратного лимита помечаем занятыми, чтобы acquire()
    // никогда их не выдал.
    for (uint8_t i = limit; i < config::TOTAL_CHANNELS; ++i) {
        states_[i].occupied = true;
    }

    initialized_ = true;
    return Status::Ok;
}

Status DmaManager::acquire(config::DmaRequest request,
                           DmaChannel& outChannel,
                           bool hint_tx) noexcept {
    // Состояние каналов меняют И задача, и обработчик прерывания DMA
    // (арбитр завершает работу из ISR), поэтому доступ под критической секцией.
    Lock lock;

    if (!initialized_) return Status::InvalidArg;
    if (request == config::DmaRequest::None || request >= config::DmaRequest::Count_) {
        return Status::InvalidArg;
    }

    // Ищем запрос в таблице вместе с направлением.
    // Для TX/RX-пар (SPI1_RX/SPI1_TX, USART1_TX/...) направление зашито
    // в сам enum, поэтому hint_tx используется только для пар с общим именем.
    const config::DeviceMapping* found = nullptr;
    for (uint8_t i = 0; i < config::mappings_count; ++i) {
        if (config::mappings[i].request == request) {
            found = &config::mappings[i];
            break;
        }
    }
    (void)hint_tx;
    if (!found) return Status::NotSupported;

    for (uint8_t i = 0; i < found->channel_count; ++i) {
        const uint8_t ch_id = found->channels[i];
        if (ch_id >= config::TOTAL_CHANNELS) continue;

        ChannelState& st = states_[ch_id];

        if (st.occupied) {
            // Тот же запрос уже держит этот канал (арбитр, запускающий работу):
            // это ВТОРАЯ ССЫЛКА на тот же канал, а не конфликт двух периферий.
            if (st.owned && st.request == request && st.holders != 0u &&
                st.holders != 0xFFu) {
                ++st.holders;
                outChannel = DmaChannel(ch_id);
                return Status::Ok;
            }
            continue;   // канал занят ДРУГИМ запросом — конфликт каналов
        }

        st.occupied   = true;
        st.owned      = true;
        st.holders    = 1u;
        st.request    = request;
        st.configured = false;

        outChannel = DmaChannel(ch_id);
        return Status::Ok;
    }

    return Status::Busy;
}

Status DmaManager::release(DmaChannel& channel) noexcept {
    Lock lock;

    if (!initialized_)    return Status::InvalidArg;
    if (!channel.valid()) return Status::InvalidArg;

    const uint8_t id = channel.id();
    if (id >= config::TOTAL_CHANNELS) return Status::InvalidArg;

    ChannelState& st = states_[id];
    if (!st.owned) return Status::Unavailable;

    // Ссылок было больше одной — снимаем только свою, канал остаётся за
    // другим владельцем (арбитром). Гасим канал, лишь когда ушла последняя.
    if (st.holders > 1u) {
        --st.holders;
        channel = DmaChannel{};
        return Status::Ok;
    }

    platform_dma_stop(id);
    platform_dma_set_callback(id, nullptr, nullptr);

    // Сбрасываем ЕДИНСТВЕННОЕ состояние — то, которое читает onTransferComplete.
    st = ChannelState{};

    channel = DmaChannel{};
    return Status::Ok;
}

bool DmaManager::channelAvailable(config::DmaRequest request) const noexcept {
    if (request == config::DmaRequest::None) return false;

    for (uint8_t i = 0; i < config::mappings_count; ++i) {
        if (config::mappings[i].request != request) continue;

        for (uint8_t k = 0; k < config::mappings[i].channel_count; ++k) {
            const uint8_t ch_id = config::mappings[i].channels[k];
            if (ch_id >= config::TOTAL_CHANNELS) continue;

            // Только полностью свободный канал: занятый (даже нашим же
            // запросом) канал новой работе не отдадут.
            if (!states_[ch_id].occupied) return true;
        }
        return false;
    }
    return false;
}

uint8_t DmaManager::channelHolders(uint8_t channel_id) const noexcept {
    if (channel_id >= config::TOTAL_CHANNELS) return 0u;
    return states_[channel_id].holders;
}

Status DmaManager::configure(uint8_t id, config::DmaRequest request,
                             const dma::BufferConfig& buf, bool tx, bool mem_to_mem,
                             dma::TransferSize unit_size, dma::Priority prio,
                             dma::Trigger trig, dma::BurstMode burst) noexcept {
    if (!initialized_) return Status::InvalidArg;
    if (id >= config::TOTAL_CHANNELS) return Status::InvalidArg;

    ChannelState& st = states_[id];
    if (!st.owned) return Status::Unavailable;
    if (!buf.buffer || buf.bytes == 0u) return Status::InvalidArg;

    // None == «оставить прежний запрос» (смена буфера).
    const config::DmaRequest eff = (request == config::DmaRequest::None)
                                 ? st.request : request;

    if (mem_to_mem) {
        st.direction = dma::Direction::MemToMem;
    } else {
        if (eff == config::DmaRequest::None) return Status::InvalidArg;
        st.direction = tx ? dma::Direction::MemToPeriph : dma::Direction::PeriphToMem;
    }

    Status rc;
    if (mem_to_mem) {
        // Реальная настройка делается в DmaChannel::configureMemToMem,
        // потому что там есть оба буфера.
        rc = Status::Ok;
    } else if (tx) {
        rc = platform_dma_configure_mem_to_periph(id, eff, buf, unit_size, prio, trig, burst);
    } else {
        rc = platform_dma_configure_periph_to_mem(id, eff, buf, unit_size, prio, trig, burst);
    }

    if (rc != Status::Ok) return rc;

    st.configured = true;
    st.request    = eff;
    st.unit_size  = unit_size;
    st.prio       = prio;
    st.trig       = trig;
    st.burst      = burst;
    st.buf        = buf;
    return Status::Ok;
}

void DmaManager::onTransferComplete(uint8_t channel_id, Status result) noexcept {
    if (channel_id >= config::TOTAL_CHANNELS) return;

    ChannelState& st = states_[channel_id];
    if (!st.cb) return;

    // Контекст на стеке: колбэк обязан скопировать данные, если сохраняет.
    // Тип общий с платформенным обработчиком (см. dma_manager.hpp).
    DmaCallbackContext ctx{ channel_id, result, st.cb_arg };

    st.cb(&ctx);
}

const dma::BufferConfig* DmaManager::buffer(uint8_t id) const noexcept {
    return (id < config::TOTAL_CHANNELS) ? &states_[id].buf : nullptr;
}
config::DmaRequest DmaManager::request(uint8_t id) const noexcept {
    return (id < config::TOTAL_CHANNELS) ? states_[id].request : config::DmaRequest::None;
}
dma::TransferSize DmaManager::unitSize(uint8_t id) const noexcept {
    return (id < config::TOTAL_CHANNELS) ? states_[id].unit_size : dma::TransferSize::Byte;
}
dma::Priority DmaManager::priority(uint8_t id) const noexcept {
    return (id < config::TOTAL_CHANNELS) ? states_[id].prio : dma::Priority::Medium;
}
dma::Trigger DmaManager::trigger(uint8_t id) const noexcept {
    return (id < config::TOTAL_CHANNELS) ? states_[id].trig : dma::Trigger::Hardware;
}
dma::BurstMode DmaManager::burst(uint8_t id) const noexcept {
    return (id < config::TOTAL_CHANNELS) ? states_[id].burst : dma::BurstMode::Single;
}
dma::Direction DmaManager::direction(uint8_t id) const noexcept {
    return (id < config::TOTAL_CHANNELS) ? states_[id].direction : dma::Direction::MemToMem;
}

// ─────────────────────────────── DmaChannel ─────────────────────────────────

const char* DmaChannel::owner() const noexcept {
    if (!valid()) return nullptr;
    return config::request_name(DmaManager::instance().request(id_));
}

Status DmaChannel::configureMemToPeriph(
    config::DmaRequest request,
    const dma::BufferConfig& src,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::Trigger trig,
    dma::BurstMode burst
) noexcept {
    if (!valid()) return Status::Unavailable;
    return DmaManager::instance().configure(id_, request, src,
                                            /*tx*/ true, /*mem_to_mem*/ false,
                                            unitSize, prio, trig, burst);
}

Status DmaChannel::configurePeriphToMem(
    config::DmaRequest request,
    const dma::BufferConfig& dst,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::Trigger trig,
    dma::BurstMode burst
) noexcept {
    if (!valid()) return Status::Unavailable;
    return DmaManager::instance().configure(id_, request, dst,
                                            /*tx*/ false, /*mem_to_mem*/ false,
                                            unitSize, prio, trig, burst);
}

Status DmaChannel::configureMemToMem(
    const dma::BufferConfig& src,
    const dma::BufferConfig& dst,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::BurstMode burst
) noexcept {
    if (!valid()) return Status::Unavailable;
    if (!src.buffer || !dst.buffer || src.bytes == 0u) return Status::InvalidArg;

    DmaManager& mgr = DmaManager::instance();

    const Status rc = platform_dma_configure_mem_to_mem(id_, src, dst, unitSize, prio, burst);
    if (rc != Status::Ok) return rc;

    // Фиксируем состояние в ЕДИНСТВЕННОМ месте — менеджере.
    // Смена буфера потом использует dst как актуальный буфер канала.
    if (mgr.configure(id_, config::DmaRequest::None, dst, true, /*mem_to_mem*/ true,
                      unitSize, prio, dma::Trigger::Software, burst) != Status::Ok) {
        return Status::Unavailable;
    }
    return Status::Ok;
}

Status DmaChannel::reconfigureBuffer(void* new_buffer, Size new_bytes) noexcept {
    if (!valid()) return Status::Unavailable;
    if (!new_buffer || new_bytes == 0u) return Status::InvalidArg;

    DmaManager& mgr = DmaManager::instance();
    const dma::BufferConfig* old = mgr.buffer(id_);
    if (!old) return Status::Unavailable;

    // Сохраняем ВСЕ прежние параметры вместо жёстко зашитых Byte/Medium/
    // Hardware/Incrementing, как было раньше.
    const dma::BufferConfig cfg{ new_buffer, new_bytes, old->increment };
    const dma::TransferSize unit = mgr.unitSize(id_);
    const dma::Priority     prio = mgr.priority(id_);
    const dma::Trigger      trig = mgr.trigger(id_);
    const dma::BurstMode    burst = mgr.burst(id_);

    switch (mgr.direction(id_)) {
        case dma::Direction::MemToPeriph:
            return DmaManager::instance().configure(
                id_, config::DmaRequest::None, cfg, true, false, unit, prio, trig, burst);
        case dma::Direction::PeriphToMem:
            return DmaManager::instance().configure(
                id_, config::DmaRequest::None, cfg, false, false, unit, prio, trig, burst);
        default:
            return Status::NotSupported;
    }
}

Status DmaChannel::start() noexcept {
    if (!valid()) return Status::Unavailable;
    platform_dma_clear_flags(id_);
    return platform_dma_start(id_);
}

Status DmaChannel::stop() noexcept {
    if (!valid()) return Status::Unavailable;
    return platform_dma_stop(id_);
}

bool DmaChannel::isComplete() const noexcept {
    return valid() ? platform_dma_is_complete(id_) : false;
}

Size DmaChannel::bytesRemaining() const noexcept {
    return valid() ? platform_dma_bytes_remaining(id_) : 0u;
}

Status DmaChannel::setCallback(Callback_h cb, void* arg) noexcept {
    if (!valid()) return Status::Unavailable;
    return platform_dma_set_callback(id_, cb, arg);
}

} // namespace embed
