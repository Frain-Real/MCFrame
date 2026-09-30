#include "dma_manager.hpp"
#include <cstring>

namespace embed {

Status DmaManager::init() noexcept {
    if (initialized_) return Status::Ok;

    for (uint8_t i = 0; i < config::TOTAL_CHANNELS; ++i) {
        channels_[i].id_         = i;
        channels_[i].owner_      = nullptr;
        channels_[i].owned_      = false;
        channels_[i].configured_ = false;
        
        channel_occupied_[i] = false;
        channel_owner_[i]    = nullptr;
    }
    initialized_ = true;
    return Status::Ok;
}

Status DmaManager::acquire(const char* deviceName,
                           DmaChannel& outChannel,
                           bool hint_tx) noexcept {
    if (!initialized_) return Status::InvalidArg;
    if (!deviceName)   return Status::InvalidArg;

    // Ищем маппинг для устройства+направления
    const config::DeviceMapping* found_mapping = nullptr;
    
    for (uint8_t i = 0; i < config::mappings_count; ++i) {
        const auto& m = config::mappings[i];
        if (std::strcmp(m.device, deviceName) == 0 && m.is_tx == hint_tx) {
            found_mapping = &m;
            break;
        }
    }

    if (!found_mapping) {
        return Status::NotSupported;
    }

    // Перебираем возможные каналы и ищем свободный
    for (uint8_t i = 0; i < found_mapping->channel_count; ++i) {
        uint8_t ch_id = found_mapping->channels[i];
        
        if (ch_id >= config::TOTAL_CHANNELS) {
            continue;
        }
        
        if (!channel_occupied_[ch_id]) {
            channel_occupied_[ch_id] = true;
            channel_owner_[ch_id]    = deviceName;
            
            auto& ch = channels_[ch_id];
            ch.owned_ = true;
            ch.owner_ = deviceName;
            outChannel = ch;
            
            return Status::Ok;
        }
    }

    return Status::Busy;
}

Status DmaManager::release(DmaChannel& channel) noexcept {
    if (!initialized_)           return Status::InvalidArg;
    if (!channel.owned_)         return Status::Unavailable;
    if (channel.id_ == 0xFF)     return Status::InvalidArg;

    channel.stop();
    
    channel_occupied_[channel.id_] = false;
    channel_owner_[channel.id_]    = nullptr;
    
    channel.owned_      = false;
    channel.configured_ = false;
    channel.owner_      = nullptr;
    
    return Status::Ok;
}

void DmaManager::onTransferComplete(uint8_t channel_id, Status result) noexcept {
    if (channel_id >= config::TOTAL_CHANNELS) return;

    auto& ch = channels_[channel_id];
    if (ch.callback_) {
        struct CbContext {
            Status  result;
            void*   arg;
        } ctx{ result, ch.cb_arg_ };

        ch.callback_(&ctx);
    }
}

// ---------- DmaChannel ----------

Status DmaChannel::configureMemToPeriph(
    const char* device_name,
    const dma::BufferConfig& src,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::Trigger trig,
    dma::BurstMode burst
) noexcept {
    if (!owned_) return Status::Unavailable;
    if (!device_name || !src.buffer || src.bytes == 0) return Status::InvalidArg;

    auto st = platform_dma_configure_mem_to_periph(
        id_, device_name, src, unitSize, prio, trig, burst
    );
    if (st != Status::Ok) return st;

    configured_ = true;
    direction_ = dma::Direction::MemToPeriph;
    device_ = device_name;
    return Status::Ok;
}

Status DmaChannel::configurePeriphToMem(
    const char* device_name,
    const dma::BufferConfig& dst,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::Trigger trig,
    dma::BurstMode burst
) noexcept {
    if (!owned_) return Status::Unavailable;
    if (!device_name || !dst.buffer || dst.bytes == 0) return Status::InvalidArg;

    auto st = platform_dma_configure_periph_to_mem(
        id_, device_name, dst, unitSize, prio, trig, burst
    );
    if (st != Status::Ok) return st;

    configured_ = true;
    direction_ = dma::Direction::PeriphToMem;
    device_ = device_name;
    return Status::Ok;
}

Status DmaChannel::configureMemToMem(
    const dma::BufferConfig& src,
    const dma::BufferConfig& dst,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::BurstMode burst
) noexcept {
    if (!owned_) return Status::Unavailable;
    if (!src.buffer || !dst.buffer || src.bytes == 0) return Status::InvalidArg;

    auto st = platform_dma_configure_mem_to_mem(
        id_, src, dst, unitSize, prio, burst
    );
    if (st != Status::Ok) return st;

    configured_ = true;
    direction_ = dma::Direction::MemToMem;
    device_ = nullptr;
    return Status::Ok;
}

Status DmaChannel::reconfigureBuffer(void* new_buffer, Size new_bytes) noexcept {
    if (!owned_ || !configured_) return Status::Unavailable;
    if (!new_buffer || new_bytes == 0) return Status::InvalidArg;

    dma::BufferConfig cfg{new_buffer, new_bytes, true};

    switch (direction_) {
        case dma::Direction::MemToPeriph:
            return platform_dma_configure_mem_to_periph(
                id_, device_, cfg, dma::TransferSize::Byte, dma::Priority::Medium,
                dma::Trigger::Hardware, dma::BurstMode::Incrementing
            );
        case dma::Direction::PeriphToMem:
            return platform_dma_configure_periph_to_mem(
                id_, device_, cfg, dma::TransferSize::Byte, dma::Priority::Medium,
                dma::Trigger::Hardware, dma::BurstMode::Incrementing
            );
        default:
            return Status::NotSupported;
    }
}

Status DmaChannel::start() noexcept {
    if (!configured_) return Status::Unavailable;
    platform_dma_clear_flags(id_);
    return platform_dma_start(id_);
}

Status DmaChannel::stop() noexcept {
    return platform_dma_stop(id_);
}

bool DmaChannel::isComplete() const noexcept {
    return platform_dma_is_complete(id_);
}

Size DmaChannel::bytesTransferred() const noexcept {
    return platform_dma_bytes_transferred(id_);
}

Status DmaChannel::setCallback(Callback_h cb, void* arg) noexcept {
    callback_ = cb;
    cb_arg_ = arg;
    return platform_dma_set_callback(id_, cb, arg);
}

} // namespace os::dma