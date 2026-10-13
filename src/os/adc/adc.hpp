#pragma once
#include "../common.hpp"
#include "../config/time_config.hpp"
#include "../core/primitives/semaphore.hpp"

namespace embed {

    template <typename ConfigT>
    bool platform_adc_init(const ConfigT& cfg) noexcept;

    bool platform_adc_start() noexcept;
    bool platform_adc_stop() noexcept;
    bool platform_adc_startScan() noexcept;

    template <typename SampleT>
    bool platform_adc_pollResult(uint8_t& pos, SampleT& val) noexcept;
    
    void platform_adc_enableIrq(bool enable) noexcept;

    template <typename SampleT>
    bool platform_adc_configureDma(volatile SampleT* buf, Size samples, bool circular) noexcept;

    bool platform_adc_startDma() noexcept;
    void platform_adc_stopDma() noexcept;

template <typename Kernel, typename Hal, Size NChannels_, Size DmaBlockFrames_ = 32, typename Sample_ = uint16_t>
class ADC {
public:
    enum class Mode : uint8_t {
        Polling,
        Async,
        Dma
    };

    struct ChannelCfg {
        uint8_t id = 0;           // аппаратный канал
        uint8_t sample_time = 0;  // время выборки, единицы задаёт HAL
        uint8_t gain = 0;         // если есть аппаратный усилитель
        bool enabled = true;      // HAL может использовать для валидации
    };

    struct Config_ {
        using SampleT = Sample_;
        Mode mode = Mode::Polling;
        
        // Для HAL: непрерывное преобразование или одиночный запуск.
        bool continuous = false;

        uint8_t resolution_bits = 12;
        uint32_t sample_rate_hz = 0;

        // Используется как fallback-таймаут в прикладных вызовах.
        uint64_t timeout_ms = 10;

        // Логические каналы в порядке сканирования.
        // NChannels_ — это число каналов, которые реально используются.
        ChannelCfg channels[NChannels_];

        struct DmaCfg {
            uint8_t stream = 0;
            uint8_t request = 0;
            uint8_t priority = 0;
            bool circular = true;
        } dma{};
    };

public:

    using ConfigT = Config_;
    using SampleT = Sample_;
    static constexpr Size kChannels = NChannels_;
    static constexpr Size kDmaBlockFrames = DmaBlockFrames_;
    static constexpr Size kDmaBlockSamples = kDmaBlockFrames * kChannels;
    static constexpr Size kDmaTotalSamples = 2u * kDmaBlockSamples;
    static constexpr Size kFrameRing = 4u;

    static_assert(kChannels > 0u && kChannels <= 32u,
                  "ADC: supported 1..32 channels");
    static_assert(kDmaBlockFrames > 0u,
                  "ADC: DMA block frames must be > 0");

    struct Frame {
        uint32_t seq = 0;
        SampleT ch[kChannels]{};
    };

    struct DmaBlock {
        volatile SampleT* data = {};
        Size samples = 0;
        uint32_t seq = 0;
    };

    Status init(const ConfigT& cfg)
    {
        if (state_ != State::Uninit) {
            return Status::Busy;
        }

        cfg_ = cfg;

        Kernel::semInit(frame_sem_, 0);
        Kernel::semInit(dma_sem_, 0);

        for (Size i = 0u; i < kDmaTotalSamples; ++i) {
            dma_buf_[i] = 0u;
        }

        if (!platform_adc_init(cfg_)) {
            return Status::HardwareError;
        }

        if (cfg_.mode == Mode::Dma) {
            if (!platform_adc_configureDma(dma_buf_, kDmaTotalSamples, cfg_.dma.circular)) {
                return Status::HardwareError;
            }
        }

        state_ = State::Stopped;
        return Status::Ok;
    }

    Status start()
    {
        if (state_ == State::Uninit) {
            return Status::Unavailable;
        }

        if (state_ == State::Running) {
            return Status::Ok;
        }

        Lock lock;

        frame_head_ = 0u;
        frame_tail_ = 0u;
        frame_count_ = 0u;

        current_ = Frame{};
        current_pos_ = 0u;
        seq_ = 0u;

        dma_write_seq_ = 0u;
        dma_read_seq_ = 0u;

        frame_overflow_ = false;
        dma_overflow_ = false;

        if (!platform_adc_start()) {
            return Status::HardwareError;
        }

        if (cfg_.mode == Mode::Dma) {
            if (!platform_adc_startDma()) {
                platform_adc_stop();
                return Status::HardwareError;
            }
        } else if (cfg_.mode == Mode::Async) {
            platform_adc_enableIrq(true);
        }

        state_ = State::Running;
        return Status::Ok;
    }

    Status stop()
    {
        if (state_ == State::Uninit) {
            return Status::NotFound;
        }

        if (state_ == State::Stopped) {
            return Status::Ok;
        }

        Lock lock;

        if (cfg_.mode == Mode::Async) {
            platform_adc_enableIrq(false);
        }

        if (cfg_.mode == Mode::Dma) {
            platform_adc_stopDma();
        }

        platform_adc_stop();

        state_ = State::Stopped;

        return Status::Ok;
    }

    // Удобно для одиночного чтения одного канала в polling/IRQ режимах.
    Status read(uint8_t channel_idx, SampleT& out, uint64_t timeout_ms = UINT32_MAX)
    {
        if (channel_idx >= kChannels) {
            return Status::InvalidArg;
        }

        Frame f{};
        Status e = readFrame(f, timeout_ms);
        if (e != Status::Ok) {
            return e;
        }

        out = f.ch[channel_idx];
        return Status::Ok;
    }

    // В DMA режиме лучше использовать copyDmaBlock()/acquireDmaBlock().
    Status readFrame(Frame& out, uint64_t timeout_ms = UINT32_MAX)
    {
        if (state_ == State::Uninit) {
            return Status::NotFound;
        }

        if (cfg_.mode == Mode::Dma) {
            return Status::NotSupported;
        }

        if (state_ != State::Running) {
            return Status::Unavailable;
        }

        if (cfg_.mode == Mode::Polling) {
            Frame f{};
            f.seq = seq_ + 1u;

            uint32_t mask = 0u;
            Size got = 0u;
            uint8_t pos = 0u;
            SampleT val = 0u;

            const uint64_t start = config::OsTimer::NowMs();

            if (!platform_adc_startScan()) {
                return Status::HardwareError;
            }

            while (got < kChannels) {
                if (platform_adc_pollResult(pos, val)) {
                    if ((pos < kChannels) && ((mask & (1u << pos)) == 0u)) {
                        mask |= (1u << pos);
                        f.ch[pos] = val;
                        ++got;
                    }
                } else if (timeoutExpired(start, timeout_ms)) {
                    return Status::Timeout;
                }
            }

            seq_ = f.seq;
            out = f;
            return Status::Ok;
        }

        // IRQ mode.
        // Сначала пробуем забрать уже готовный кадр.
        if (popFrame(out)) {
            return Status::Ok;
        }

        const uint64_t start = config::OsTimer::NowMs();

        while (true) {
            if (frame_sem_.try_wait()) {
                // Последний шанс, если семафор был, но кадр уже забрали.
                if (popFrame(out)) {
                    return Status::Ok;
                }
                return Status::Timeout;
            }

            if (popFrame(out)) {
                return Status::Ok;
            }

            if (timeoutExpired(start, timeout_ms)) {
                return Status::Timeout;
            }
        }
    }

    // Безопасное копирование готового DMA-блока.
    // Рекомендуется использовать вместо прямого доступа к DMA-буферу.
    Status copyDmaBlock(SampleT* dst,
                     Size max_samples,
                     Size& copied,
                     uint32_t timeout_ms = UINT32_MAX)
    {
        if (!dst) {
            return Status::InvalidArg;
        }

        DmaBlock b{};
        Status e = acquireDmaBlock(b, timeout_ms);
        if (e != Status::Ok) {
            return e;
        }

        Size n = (b.samples < max_samples) ? b.samples : max_samples;

        for (Size i = 0u; i < n; ++i) {
            dst[i] = b.data[i];
        }

        copied = n;
        return Status::Ok;
    }

    // Zero-copy доступ к готовому DMA-блоку.
    //
    // Важно: блок находится в DMA-памяти и будет перезаписан через 2 периода.
    // Если нужна долговременная обработка — копируйте через copyDmaBlock().
    Status acquireDmaBlock(DmaBlock& out, uint64_t timeout_ms = UINT32_MAX)
    {
        if (state_ == State::Uninit) {
            return Status::NotFound;
        }

        if (cfg_.mode != Mode::Dma) {
            return Status::NotSupported;
        }

        if (state_ != State::Running) {
            return Status::PermissionDenied;
        }

        const uint64_t start = config::OsTimer::NowMs();

        while (true) {
            if (tryAcquireDmaBlock(out)) {
                return Status::Ok;
            }

            if (timeoutExpired(start, timeout_ms)) {
                return Status::Timeout;
            }

            if (dma_sem_.try_wait()) {
                if (tryAcquireDmaBlock(out)) {
                    return Status::Ok;
                }
                return Status::Timeout;
            }
        }
    }

    // Вызывается из ISR АЦП в режиме прерываний.
    // Например:
    //
    // extern "C" void ADC1_IRQHandler()
    // {
    //     adc1.irqConversionComplete();
    // }
    void irqConversionComplete()
    {
        uint8_t pos = 0u;
        SampleT val = 0u;

        while (platform_adc_pollResult(pos, val)) {
            if (pos >= kChannels) {
                continue;
            }

            if (pos == 0u) {
                current_pos_ = 0u;
                current_.seq = seq_ + 1u;
            } else if (pos != current_pos_) {
                continue;
            }

            current_.ch[pos] = val;
            ++current_pos_;

            if (current_pos_ == kChannels) {
                ++seq_;

                Frame f = current_;
                current_pos_ = 0u;

                pushFrameFromIsr(f);
                frame_sem_.try_wait();
            }
        }
    }

    void dmaHalfComplete()
    {
        dmaBlockReady();
    }

    void dmaComplete()
    {
        dmaBlockReady();
    }

    bool takeFrameOverflow()
    {
        Lock lock;
        bool v = frame_overflow_;
        frame_overflow_ = false;
        return v;
    }

    bool takeDmaOverflow()
    {
        Lock lock;
        bool v = dma_overflow_;
        dma_overflow_ = false;
        return v;
    }

private:
    enum class State : uint8_t {
        Uninit,
        Stopped,
        Running
    };

    bool timeoutExpired(uint64_t start, uint64_t timeout_ms) const
    {
        if (timeout_ms == UINT32_MAX) {
            return false;
        }

        return (config::OsTimer::NowMs() - start) >= timeout_ms;
    }

    bool pushFrameFromIsr(const Frame& f)
    {
        Lock lock;

        frame_ring_[frame_head_] = f;
        frame_head_ = (frame_head_ + 1u) % kFrameRing;

        if (frame_count_ == kFrameRing) {
            // Переполнение: выбрасываем самый старый кадр.
            frame_tail_ = (frame_tail_ + 1u) % kFrameRing;
            frame_overflow_ = true;
        } else {
            ++frame_count_;
        }

        return true;
    }

    bool popFrame(Frame& f)
    {
        Lock lock;

        if (frame_count_ == 0u) {
            return false;
        }

        f = frame_ring_[frame_tail_];
        frame_tail_ = (frame_tail_ + 1u) % kFrameRing;
        --frame_count_;

        return true;
    }

    bool tryAcquireDmaBlock(DmaBlock& out)
    {
        Lock lock;

        if (dma_write_seq_ > dma_read_seq_) {
            uint32_t seq = dma_read_seq_;
            Size half = static_cast<Size>(seq & 1u);

            out.data = &dma_buf_[half * kDmaBlockSamples];
            out.samples = kDmaBlockSamples;
            out.seq = seq;

            ++dma_read_seq_;
            return true;
        }

        return false;
    }

    void dmaBlockReady()
    {
        Lock lock;

        // Если приложение не успело забрать уже 2 блока,
        // один старый блок придётся отбросить, чтобы сохранить актуальность.
        if ((dma_write_seq_ - dma_read_seq_) >= 2u) {
            dma_read_seq_ = dma_write_seq_ - 1u;
            dma_overflow_ = true;
        }

        ++dma_write_seq_;

        dma_sem_.try_wait();
    }

    ConfigT cfg_{};
    State state_ = State::Uninit;

    semaphore<sem_type::binary, 1> frame_sem_;
    semaphore<sem_type::binary, 1> dma_sem_;

    Frame frame_ring_[kFrameRing]{};
    Size frame_head_ = 0u;
    Size frame_tail_ = 0u;
    Size frame_count_ = 0u;

    Frame current_{};
    uint8_t current_pos_ = 0u;
    uint32_t seq_ = 0u;

    bool frame_overflow_ = false;

    alignas(32) volatile SampleT dma_buf_[kDmaTotalSamples];

    uint32_t dma_write_seq_ = 0u;
    uint32_t dma_read_seq_ = 0u;

    bool dma_overflow_ = false;
};

}