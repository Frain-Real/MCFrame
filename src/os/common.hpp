// common.hpp
#pragma once

#include <cstdint>
#include "iplatform.hpp"   // даёт Size/Address/CoreId/... и platform::ops()

namespace embed {

    // ------------------------------------------------------------------------
    // Критическая секция.
    //
    // Тонкие обёртки над platform::ops(). Платформа ОБЯЗАНА сохранять и
    // восстанавливать маску прерываний, поэтому Lock реентерабелен:
    // вложенный Lock не включает прерывания раньше внешнего.
    // ------------------------------------------------------------------------
    [[nodiscard]] inline uint32_t platform_critical_enter() noexcept {
        return platform::ops().critical_enter();
    }
    inline void platform_critical_exit(uint32_t state) noexcept {
        platform::ops().critical_exit(state);
    }

    /// RAII-блокировка критической секции.
    struct Lock
    {
        uint32_t state_;
        Lock() noexcept  : state_(platform::ops().critical_enter()) {}
        ~Lock() noexcept { platform::ops().critical_exit(state_); }

        Lock(const Lock&)            = delete;
        Lock& operator=(const Lock&) = delete;
    };

    /// Досрочно покинуть критическую секцию (нужно перед блокировкой задачи,
    /// чтобы не уснуть с выключенными прерываниями). Повторный вызов
    /// в деструкторе становится безопасным no-op.
    inline void unlock_early(Lock& l) noexcept {
        platform::ops().critical_exit(l.state_);
        l.state_ = 0u;
    }

    // ------------------------------------------------------------------------
    // Общие типы
    // ------------------------------------------------------------------------
    enum class Status : int
    {
        Ok = 0,
        InvalidArg,
        NoMemory,
        Busy,
        Timeout,
        NotFound,
        AlreadyExists,
        PermissionDenied,
        NotSupported,
        HardwareError,
        Unavailable,
        Fault,
        Aborted,
        TimeNotSynced,
        Overflow,
        Corruption
    };

    using Handle        = unsigned int;
    using ThreadId      = unsigned short;
    using ProcessId     = unsigned short;
    using IrqNumber     = unsigned short;
    using Milliseconds  = unsigned int;
    using Microseconds  = unsigned int;

    /// Сентинел «ждать бесконечно». Отдельный тип не даёт перепутать его
    /// с настоящим таймаутом (в отличие от прежнего UINT32_MAX).
    using Timeout = uint64_t;
    constexpr Timeout WaitForever = ~Timeout{0};

    /// Сентинел «ближайшего срока нет» для запросов вида
    /// «через сколько миллисекунд системе точно понадобится ядро».
    ///
    /// Значение — максимальное uint32_t, поэтому min() с ним работает как
    /// «нет ограничения», а арифметика по нему НЕ ведётся: сравнивать и
    /// подставлять в min, не складывать.
    inline constexpr uint32_t kNoDeadline = 0xFFFFFFFFu;

    using task_func = void (*)(void* ctx);

    enum class Access : unsigned char
    {
        None = 0,
        Read,
        Write,
        ReadWrite,
        Execute
    };

    namespace dma {
        enum class Direction : uint8_t {
            MemToMem,
            MemToPeriph,
            PeriphToMem,
            PeriphToPeriph
        };
        enum class TransferSize : uint8_t {
            Byte,       // 8 bit
            HalfWord,   // 16 bit
            Word        // 32 bit
        };

        enum class Priority : uint8_t {
            Low = 0,
            Medium,
            High,
            VeryHigh
        };

        enum class Trigger : uint8_t {
            Software,
            Hardware
        };

        /// Режим передачи. «Инкремент адреса» задаётся отдельными флагами
        /// BufferConfig::increment (PINC/MINC), здесь — только форма
        /// цикла передачи. Значения Single/Incrementing/Circular были
        /// синонимами и включали MINC дважды.
        enum class BurstMode : uint8_t {
            Single,
            Circular
        };

        struct BufferConfig {
            void*  buffer;
            Size   bytes;
            bool   increment;
        };
    }
}
