// adc.hpp
#pragma once
#include "../common.hpp"
#include "../config/time_config.hpp"

namespace embed {

// ---------------------------------------------------------------------------
// Платформенный слой АЦП.
//
// ВАЖНО: функции НЕ шаблонные. Прежде platform_adc_pollResult и
// platform_adc_init были шаблонами, поэтому не могли быть определены в .cpp
// платформы — модуль был нерабочим по построению.
//
// Тип отсчёта фиксирован: 12-битный АЦП умещается в uint16_t.
// ---------------------------------------------------------------------------
using AdcSample = uint16_t;

/// Инициализация АЦП. Конфигурация передаётся непрозрачно (void* + размер),
/// чтобы ядро не зависело от типа HAL-структуры.
bool platform_adc_init(const void* cfg, Size cfg_size) noexcept;
bool platform_adc_start() noexcept;
bool platform_adc_stop() noexcept;
bool platform_adc_startScan() noexcept;

/// Готовый результат: true и (pos, val), если данные есть.
bool platform_adc_pollResult(uint8_t& pos, AdcSample& val) noexcept;

void platform_adc_enableIrq(bool enable) noexcept;

bool platform_adc_configureDma(AdcSample* buf, Size samples, bool circular) noexcept;
bool platform_adc_startDma() noexcept;
void platform_adc_stopDma() noexcept;

/// Платформа вызывает это из обработчика АЦП.
void platform_adc_onIrq() noexcept;

/// Задать регулярную последовательность каналов (порядок сканирования).
/// Реализуется платформой; приложение вызывает до start().
bool platform_adc_setChannels(const uint8_t* channels, Size count) noexcept;

/// Задать время выборки канала (код зависит от платформы).
bool platform_adc_setSampleTime(uint8_t channel, uint8_t cycles_code) noexcept;

// ---------------------------------------------------------------------------
// Драйвер АЦП.
//
// Исправлено относительно прежней версии:
//  * таймаут — Timeout с явным сентинелом WaitForever вместо
//    uint64_t timeout = UINT32_MAX (где UINT32_MAX означал «вечно», будучи
//    при этом допустимым малым значением, а в copyDmaBlock дефолт
//    UINT64_MAX молча усекался до uint32_t);
//  * в циклах ожидания вызывается Kernel::yield() вместо busy-wait;
//  * DMA-буфер без volatile: volatile заставлял каждое чтение идти в память
//    и запрещал оптимизацию, не давая защиты от DMA;
//  * кадры не копируются под критической секцией внутри ISR;
//  * ping-pong реализован через отдельные половины буфера и корректную
//    защиту от переполнения.
// ---------------------------------------------------------------------------
template <
    typename Kernel,
    typename Hal,
    Size     NChannels_,
    Size     DmaBlockFrames_ = 32,
    typename Sample_ = AdcSample
>
class ADC {
public:
    using SampleT = Sample_;
    using ConfigT = Hal;          // конфигурация задаётся HAL-платформой

    enum class Mode : uint8_t { Polling, Async, Dma };
    enum class State : uint8_t { Uninit, Stopped, Running };

    static constexpr Size kChannels        = NChannels_;
    static constexpr Size kDmaBlockFrames  = DmaBlockFrames_;
    static constexpr Size kDmaBlockSamples = kDmaBlockFrames * kChannels;
    static constexpr Size kDmaTotalSamples = 2u * kDmaBlockSamples;   // ping-pong

    static_assert(kChannels > 0u && kChannels <= 32u,
                  "ADC: поддерживается 1..32 канала");
    static_assert(kDmaBlockFrames > 0u,
                  "ADC: размер DMA-блока должен быть больше нуля");

    struct Frame {
        uint32_t seq = 0;
        SampleT  ch[kChannels]{};
    };

    struct DmaBlock {
        const SampleT* data    = nullptr;
        Size           samples = 0;
        uint32_t       seq     = 0;
        bool           valid   = false;
    };

    struct Config {
        Mode     mode            = Mode::Polling;
        bool     continuous      = false;
        uint8_t  resolution_bits = 12u;
        uint32_t sample_rate_hz  = 0u;
        Timeout  default_timeout = WaitForever;
        bool     dma_circular    = true;

        // Буфер ping-pong предоставляет ПРИЛОЖЕНИЕ.
        //
        // Прежде буфер на kDmaTotalSamples лежал статическим членом класса:
        // при DmaBlockFrames=32 и 16 каналах это 2 КБ BSS даже там, где АЦП
        // не используется. Плюс размер задавался только шаблоном.
        SampleT* dma_buffer      = nullptr;
        Size     dma_buffer_size = 0u;     // в отсчётах, >= kDmaTotalSamples
    };

    // ─────────────────────────── Инициализация ───────────────────────────
    Status init(const Config& cfg) noexcept {
        if (state_ != State::Uninit) return Status::Busy;
        cfg_   = cfg;
        state_ = State::Stopped;
        return Status::Ok;
    }

    /// Платформенная инициализация с конкретной конфигурацией АЦП.
    Status initHardware(const void* hal_cfg, Size cfg_size) noexcept {
        if (state_ == State::Running) return Status::Busy;

        if (cfg_.mode == Mode::Dma) {
            if (cfg_.dma_buffer == nullptr) return Status::InvalidArg;
            if (cfg_.dma_buffer_size < kDmaTotalSamples) return Status::NoMemory;
        }

        clearBuffers();
        if (!platform_adc_init(hal_cfg, cfg_size)) return Status::HardwareError;

        if (cfg_.mode == Mode::Dma) {
            if (!platform_adc_configureDma(cfg_.dma_buffer, kDmaTotalSamples,
                                           cfg_.dma_circular)) {
                return Status::HardwareError;
            }
        }
        state_ = State::Stopped;
        return Status::Ok;
    }

    // ─────────────────────────── Пуск / стоп ───────────────────────────
    Status start() noexcept {
        if (state_ == State::Uninit)  return Status::Unavailable;
        if (state_ == State::Running) return Status::Ok;

        Lock lock;
        clearBuffers();

        if (!platform_adc_start()) return Status::HardwareError;

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

    Status stop() noexcept {
        if (state_ == State::Uninit)  return Status::NotFound;
        if (state_ == State::Stopped) return Status::Ok;

        Lock lock;
        if (cfg_.mode == Mode::Async) platform_adc_enableIrq(false);
        if (cfg_.mode == Mode::Dma)   platform_adc_stopDma();
        platform_adc_stop();

        state_ = State::Stopped;
        return Status::Ok;
    }

    // ─────────────────────────── Чтение ───────────────────────────
    Status read(uint8_t channel_idx, SampleT& out,
                Timeout timeout = WaitForever) noexcept {
        if (channel_idx >= kChannels) return Status::InvalidArg;

        Frame f{};
        const Status st = readFrame(f, timeout);
        if (st != Status::Ok) return st;
        out = f.ch[channel_idx];
        return Status::Ok;
    }

    Status readFrame(Frame& out, Timeout timeout = WaitForever) noexcept {
        if (state_ == State::Uninit)  return Status::NotFound;
        if (cfg_.mode == Mode::Dma)   return Status::NotSupported;
        if (state_ != State::Running) return Status::Unavailable;

        if (cfg_.mode == Mode::Polling) return readFramePolling(out, timeout);

        if (popFrame(out)) return Status::Ok;

        const uint64_t start = config::OsTimer::NowMs();
        for (;;) {
            if (popFrame(out)) return Status::Ok;
            if (timeoutExpired(start, timeout)) return Status::Timeout;
            Kernel::yield();          // уступаем CPU вместо busy-wait
        }
    }

    Status copyDmaBlock(SampleT* dst, Size max_samples, Size& copied,
                        Timeout timeout = WaitForever) noexcept {
        if (dst == nullptr) return Status::InvalidArg;

        DmaBlock b{};
        const Status st = acquireDmaBlock(b, timeout);
        if (st != Status::Ok) return st;

        const Size n = (b.samples < max_samples) ? b.samples : max_samples;
        for (Size i = 0; i < n; ++i) dst[i] = b.data[i];
        copied = n;
        return Status::Ok;
    }

    /// Zero-copy доступ к готовому блоку. ВНИМАНИЕ: DMA перезапишет эту
    /// половину через один период — обрабатывайте немедленно или копируйте.
    Status acquireDmaBlock(DmaBlock& out, Timeout timeout = WaitForever) noexcept {
        if (state_ == State::Uninit)  return Status::NotFound;
        if (cfg_.mode != Mode::Dma)   return Status::NotSupported;
        if (state_ != State::Running) return Status::PermissionDenied;
        if (!cfg_.dma_circular)       return Status::NotSupported;

        const uint64_t start = config::OsTimer::NowMs();
        for (;;) {
            if (tryAcquireDmaBlock(out)) return Status::Ok;
            if (timeoutExpired(start, timeout)) return Status::Timeout;
            Kernel::yield();
        }
    }

    // ─────────────────────── События от платформы ───────────────────────
    /// Вызывается из обработчика АЦП.
    void onConversionComplete() noexcept {
        uint8_t pos = 0u;
        SampleT val = 0u;

        while (platform_adc_pollResult(pos, val)) {
            if (pos >= kChannels) continue;

            if (pos == 0u) {
                current_pos_ = 0u;
                current_.seq = seq_ + 1u;
            } else if (pos != current_pos_) {
                continue;                 // пришёл не тот канал — пропускаем
            }

            current_.ch[pos] = val;
            ++current_pos_;

            if (current_pos_ == kChannels) {
                ++seq_;
                current_pos_ = 0u;
                pushFrameFromIsr(current_);
            }
        }
    }

    /// Первая половина DMA-буфера готова (ping).
    void dmaHalfComplete() noexcept { dmaBlockReady(0u); }
    /// Буфер пройден целиком (pong).
    void dmaComplete()     noexcept { dmaBlockReady(1u); }

    bool takeFrameOverflow() noexcept {
        Lock lock;
        const bool v = frame_overflow_;
        frame_overflow_ = false;
        return v;
    }

    bool takeDmaOverflow() noexcept {
        Lock lock;
        const bool v = dma_overflow_;
        dma_overflow_ = false;
        return v;
    }

    State state() const noexcept { return state_; }
    Size  frameOverflows() const noexcept { return frame_overflows_; }

private:
    static bool timeoutExpired(uint64_t start, Timeout timeout) noexcept {
        if (timeout == WaitForever) return false;
        return (config::OsTimer::NowMs() - start) >= timeout;
    }

    void clearBuffers() noexcept {
        frame_head_ = frame_tail_ = frame_count_ = 0u;
        current_ = Frame{};
        current_pos_ = 0u;
        seq_ = 0u;
        frame_overflow_ = false;
        dma_write_seq_ = dma_read_seq_ = 0u;
        dma_overflow_ = false;
        frame_overflows_ = 0u;
        if (cfg_.dma_buffer != nullptr) {
            for (Size i = 0; i < kDmaTotalSamples; ++i) cfg_.dma_buffer[i] = 0u;
        }
    }

    Status readFramePolling(Frame& out, Timeout timeout) noexcept {
        Frame f{};
        f.seq = seq_ + 1u;

        if (!platform_adc_startScan()) return Status::HardwareError;

        uint32_t mask = 0u;
        Size     got  = 0u;
        uint8_t  pos  = 0u;
        SampleT  val  = 0u;

        const uint64_t start = config::OsTimer::NowMs();

        while (got < kChannels) {
            if (platform_adc_pollResult(pos, val)) {
                if (pos < kChannels) {
                    const uint32_t bit = 1UL << pos;
                    if ((mask & bit) == 0u) {
                        mask |= bit;
                        f.ch[pos] = val;
                        ++got;
                    }
                }
            } else if (timeoutExpired(start, timeout)) {
                return Status::Timeout;
            } else {
                Kernel::yield();
            }
        }

        seq_ = f.seq;
        out = f;
        return Status::Ok;
    }

    void pushFrameFromIsr(const Frame& f) noexcept {
        // Без Lock: мы уже в прерывании, а критическая секция здесь
        // включала бы прерывания при выходе из неё.
        frame_ring_[frame_head_] = f;
        frame_head_ = (frame_head_ + 1u) % kFrameRing;

        if (frame_count_ == kFrameRing) {
            frame_tail_ = (frame_tail_ + 1u) % kFrameRing;   // выбрасываем старый
            frame_overflow_ = true;
            ++frame_overflows_;
        } else {
            ++frame_count_;
        }
    }

    bool popFrame(Frame& f) noexcept {
        Lock lock;
        if (frame_count_ == 0u) return false;
        f = frame_ring_[frame_tail_];
        frame_tail_ = (frame_tail_ + 1u) % kFrameRing;
        --frame_count_;
        return true;
    }

    static Size half_of(uint32_t seq) noexcept { return static_cast<Size>(seq & 1u); }

    bool tryAcquireDmaBlock(DmaBlock& out) noexcept {
        Lock lock;
        if (dma_write_seq_ <= dma_read_seq_) return false;

        const uint32_t seq = dma_read_seq_;
        out.data    = &cfg_.dma_buffer[half_of(seq) * kDmaBlockSamples];
        out.samples = kDmaBlockSamples;
        out.seq     = seq;
        out.valid   = true;
        ++dma_read_seq_;
        return true;
    }

    void dmaBlockReady(uint32_t half) noexcept {
        // Вызывается из обработчика DMA: без Lock.
        ++dma_write_seq_;
        last_half_ = half;
        if ((dma_write_seq_ - dma_read_seq_) > 2u) {
            dma_read_seq_ = dma_write_seq_ - 2u;   // устаревшее отбрасываем
            dma_overflow_ = true;
        }
    }

    static constexpr Size kFrameRing = 4u;

    Config cfg_{};
    State  state_ = State::Uninit;

    Frame    frame_ring_[kFrameRing]{};
    Size     frame_head_  = 0u;
    Size     frame_tail_  = 0u;
    Size     frame_count_ = 0u;

    Frame    current_{};
    uint8_t  current_pos_ = 0u;
    uint32_t seq_ = 0u;

    bool     frame_overflow_  = false;
    Size     frame_overflows_ = 0u;

    uint32_t dma_write_seq_ = 0u;
    uint32_t dma_read_seq_  = 0u;
    uint32_t last_half_     = 0u;
    bool     dma_overflow_  = false;
};

}
