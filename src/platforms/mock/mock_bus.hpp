// platforms/mock/mock_bus.hpp
#pragma once
// ---------------------------------------------------------------------------
// Эмуляция аппаратных шин для mock-сборки (тесты и отладка на ПК).
//
// ЗАЧЕМ: чтобы UART/I2C-драйверы, их автоматы и связка с async IO
// проверялись без железа. Эмулируется именно ПОВЕДЕНИЕ шины:
//   * у UART есть приёмный и передающий регистры, флаги готовности,
//     ошибки (переполнение, кадр) и задержка передачи;
//   * у I2C есть адресация, NACK от отсутствующего устройства, ответ
//     зарегистрированных устройств и ошибки шины.
//
// Стратегия (Polling/Async/Dma) влияет только на то, КТО завершает
// операцию, а не на результат — это и проверяют тесты.
// ---------------------------------------------------------------------------
#include "../../os/common.hpp"

#include <cstring>

namespace embed::mock {

// ────────────────────────────── UART ────────────────────────────────────────

/// Модель UART: буферы, флаги, задержка, инъекция ошибок.
struct UartBus {
    static constexpr Size kFifo = 256u;

    // ── Приём ──
    uint8_t rx[kFifo]{};
    Size    rx_len  = 0u;
    Size    rx_read = 0u;

    // ── Передача ──
    uint8_t tx[kFifo]{};
    Size    tx_len = 0u;

    // ── Поведение ──
    /// Сколько «операций железа» нужно на передачу одного байта.
    /// 0 — мгновенно. Больше нуля — проверяет, что драйвер ждёт,
    /// а не предполагает готовность.
    uint32_t tx_ticks_per_byte = 0u;
    uint32_t tx_elapsed = 0u;
    Size     tx_shifting = 0u;      // сколько байт «в сдвиговом регистре»

    /// Следующая операция завершится ошибкой (переполнение/кадр).
    bool fail_next_tx = false;
    bool fail_next_rx = false;

    /// Линия свободна: принятый пакет закончился.
    ///
    /// Нужно, чтобы read() возвращался, когда пришли данные и линия
    /// замолчала, а не ждал заполнения всего буфера. Так ведёт себя
    /// настоящий UART (по простому признаку простоя линии), и именно
    /// поэтому read(buf, 64) на 4 принятых байтах возвращает 4, а не
    /// висит до таймаута.
    bool rx_idle = true;

    void reset() {
        rx_len = rx_read = 0u;
        tx_len = 0u;
        tx_ticks_per_byte = 0u;
        tx_elapsed = 0u;
        tx_shifting = 0u;
        fail_next_tx = false;
        fail_next_rx = false;
        rx_idle = true;
    }

    /// Положить данные «в приёмник». Считается, что пакет завершён,
    /// поэтому линия помечается свободной.
    void feed(const void* data, Size len) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (Size i = 0u; i < len && rx_len < kFifo; ++i) {
            rx[rx_len++] = p[i];
        }
        rx_idle = true;
    }

    void feed(const char* text) {
        Size n = 0u;
        while (text[n] != '\0') ++n;
        feed(text, n);
    }

    Size rx_available() const noexcept { return rx_len - rx_read; }

    /// Забрать один байт из приёмника.
    bool popByte(uint8_t& out) {
        if (rx_available() == 0u) return false;
        out = rx[rx_read++];
        return true;
    }

    /// «Принять» байт от передатчика.
    void pushTx(uint8_t b) {
        if (tx_len < kFifo) tx[tx_len++] = b;
    }

    /// Продвинуть «передачу» на один шаг. Возвращает число переданных байт.
    Size tick() {
        if (tx_ticks_per_byte == 0u) return 0u;      // мгновенная передача
        if (++tx_elapsed < tx_ticks_per_byte) return 0u;
        tx_elapsed = 0u;
        return 1u;
    }
};

inline UartBus g_uart_bus{};

// ─────────────────────────────── I2C ────────────────────────────────────────

/// Виртуальное устройство на шине I2C.
struct I2cDevice {
    using Handler = bool (*)(void* ctx, bool read,
                             const uint8_t* tx, Size tx_len,
                             uint8_t* rx, Size rx_len, Size& done) noexcept;

    uint8_t address = 0u;          // 7 бит
    Handler handler = nullptr;
    void*   ctx     = nullptr;
    bool    present = true;        // false — устройство «не отвечает» (NACK)
};

/// Модель шины I2C: адресация, устройства, ошибки.
struct I2cBus {
    static constexpr unsigned kMaxDevices = 8u;

    I2cDevice devices[kMaxDevices]{};
    unsigned  device_count = 0u;

    /// Регистры эмулируемого устройства: адрес регистра -> значение.
    /// Используется обработчиком register_device().
    static constexpr Size kRegs = 32u;
    uint8_t regs[kRegs]{};

    /// Следующая операция завершится ошибкой шины (BERR/ARLO).
    bool fail_next = false;
    /// Шина «залипла»: любая операция возвращает HardwareError,
    /// пока не вызовут recover().
    bool bus_stuck = false;

    void reset() {
        for (auto& d : devices) d = I2cDevice{};
        device_count = 0u;
        for (auto& r : regs) r = 0u;
        fail_next = false;
        bus_stuck = false;
    }

    /// Простейшее устройство «регистровая модель»:
    /// запись двух байт = (адрес регистра, значение), чтение — по
    /// последнему записанному адресу. Этого достаточно для проверки
    /// writeRead() и readRegister().
    static bool register_model_handler(void* ctx, bool read,
                                       const uint8_t* tx, Size tx_len,
                                       uint8_t* rx, Size rx_len,
                                       Size& done) noexcept {
        auto* bus = static_cast<I2cBus*>(ctx);
        if (bus == nullptr) return false;

        if (!read) {
            if (tx_len >= 2u) bus->regs[tx[0] % kRegs] = tx[1];
            else if (tx_len == 1u) bus->last_reg = tx[0];
            done = tx_len;
            return true;
        }
        for (Size i = 0u; i < rx_len; ++i) {
            rx[i] = bus->regs[(bus->last_reg + i) % kRegs];
        }
        done = rx_len;
        return true;
    }

    uint8_t last_reg = 0u;

    /// Зарегистрировать устройство с регистровой моделью.
    bool register_device(uint8_t address) {
        if (device_count >= kMaxDevices) return false;
        if (find(address) != nullptr) return false;

        devices[device_count].address = address & 0x7Fu;
        devices[device_count].handler = &register_model_handler;
        devices[device_count].ctx     = this;
        devices[device_count].present = true;
        ++device_count;
        return true;
    }

    /// Зарегистрировать своё устройство.
    bool register_device(uint8_t address, I2cDevice::Handler h, void* ctx) {
        if (device_count >= kMaxDevices) return false;
        devices[device_count].address = address & 0x7Fu;
        devices[device_count].handler = h;
        devices[device_count].ctx     = ctx;
        devices[device_count].present = true;
        ++device_count;
        return true;
    }

    I2cDevice* find(uint8_t address) {
        for (unsigned i = 0u; i < device_count; ++i) {
            if (devices[i].address == (address & 0x7Fu)) return &devices[i];
        }
        return nullptr;
    }

    /// Выполнить обмен так, как это сделала бы шина.
    Status transfer(uint8_t address, bool read,
                    const uint8_t* tx, Size tx_len,
                    uint8_t* rx, Size rx_len, Size& done) {
        done = 0u;
        if (bus_stuck) return Status::HardwareError;
        if (fail_next) { fail_next = false; return Status::HardwareError; }

        I2cDevice* dev = find(address);
        if (dev == nullptr || !dev->present) return Status::NotFound;   // NACK
        if (dev->handler == nullptr) return Status::HardwareError;
        if (!dev->handler(dev->ctx, read, tx, tx_len, rx, rx_len, done)) {
            return Status::HardwareError;
        }
        return Status::Ok;
    }
};

inline I2cBus g_i2c_bus{};

} // namespace embed::mock
