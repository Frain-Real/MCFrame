#pragma once
#include "../common.hpp"
#include "../config/dma_config.hpp"

namespace embed {

uint8_t platform_dma_max_channels() noexcept;

Status platform_dma_configure_mem_to_periph(
    uint8_t global_id,
    const char* device_name,
    const dma::BufferConfig& src,
    dma::TransferSize unitSize,
    dma::Priority prio,
    dma::Trigger trig,
    dma::BurstMode burst
) noexcept;

Status platform_dma_configure_periph_to_mem(
    uint8_t global_id,
    const char* device_name,
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
bool            platform_dma_is_complete(uint8_t global_id) noexcept;
Size   platform_dma_bytes_transferred(uint8_t global_id) noexcept;
Status platform_dma_set_callback(uint8_t global_id, Callback_h cb, void* arg) noexcept;
Status platform_dma_clear_flags(uint8_t global_id) noexcept;

void platform_dma_irq_handler(uint8_t global_id) noexcept;


class DmaManager;

class DmaChannel {
public:
    DmaChannel() = default;

    Status configureMemToPeriph(
        const char* device_name,
        const dma::BufferConfig& src,
        dma::TransferSize unitSize = dma::TransferSize::Byte,
        dma::Priority prio = dma::Priority::Medium,
        dma::Trigger trig = dma::Trigger::Hardware,
        dma::BurstMode burst = dma::BurstMode::Incrementing
    ) noexcept;

    Status configurePeriphToMem(
        const char* device_name,
        const dma::BufferConfig& dst,
        dma::TransferSize unitSize = dma::TransferSize::Byte,
        dma::Priority prio = dma::Priority::Medium,
        dma::Trigger trig = dma::Trigger::Hardware,
        dma::BurstMode burst = dma::BurstMode::Incrementing
    ) noexcept;

    Status configureMemToMem(
        const dma::BufferConfig& src,
        const dma::BufferConfig& dst,
        dma::TransferSize unitSize = dma::TransferSize::Byte,
        dma::Priority prio = dma::Priority::Medium,
        dma::BurstMode burst = dma::BurstMode::Incrementing
    ) noexcept;

    Status reconfigureBuffer(void* new_buffer, Size new_bytes) noexcept;

    Status start() noexcept;
    Status stop() noexcept;

    bool   isComplete() const noexcept;
    Size   bytesTransferred() const noexcept;

    Status setCallback(Callback_h cb, void* arg) noexcept;

    uint8_t id() const noexcept { return id_; }
    const char* owner() const noexcept { return owner_; }

private:
    friend class DmaManager;

    uint8_t     id_         = 0xFF;
    const char* owner_      = nullptr;
    bool        owned_      = false;
    bool        configured_ = false;
    dma::Direction   direction_  = dma::Direction::MemToMem;
    const char* device_     = nullptr;
    
    Callback_h  callback_   = nullptr;
    void*       cb_arg_     = nullptr;
};

class DmaManager {
public:
    static DmaManager& instance() noexcept {
        static DmaManager mgr;
        return mgr;
    }

    Status init() noexcept;

    Status acquire(const char* deviceName,
                   DmaChannel& outChannel,
                   bool hint_tx = true) noexcept;

    Status release(DmaChannel& channel) noexcept;

    void onTransferComplete(uint8_t channel_id, Status result) noexcept;

private:
    DmaManager() = default;

    DmaChannel channels_[config::TOTAL_CHANNELS];
    bool       channel_occupied_[config::TOTAL_CHANNELS];
    const char* channel_owner_[config::TOTAL_CHANNELS];
    
    bool initialized_ = false;
};

} // namespace os::dma