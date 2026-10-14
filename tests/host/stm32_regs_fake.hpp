// tests/host/stm32_regs_fake.hpp
#pragma once
// ---------------------------------------------------------------------------
// ПРОГРАММНАЯ МОДЕЛЬ РЕГИСТРОВ STM32F103 ДЛЯ ТЕСТОВ HAL-ЛОГИКИ НА ПК.
//
// ЗАЧЕМ
// Автоматы STM32-драйверов (uart_stm32.hpp, i2c_stm32.hpp) обращаются к MMIO
// по абсолютным адресам, поэтому на ПК их «как есть» не запустить. Здесь
// битовые маски, имена регистров и номера прерываний берутся из настоящей
// CMSIS (stm32f10x.h + core_cm3.h), а указатели USART1/I2C1/RCC/NVIC
// подменяются на память процесса с хуками чтения/записи регистров.
//
// Подмена включается ТОЛЬКО флагом MCFRAME_STM32_REGS_FAKE, который читает
// platforms/stm32/stm32_regs.hpp. В прошивке этого файла нет вовсе, а в
// автоматах драйверов нет ни одного ветвления «для теста»: тест проверяет
// ровно тот код, который уходит на МКУ.
//
// ЧТО МОДЕЛИРУЕТСЯ
//   * USART1: флаги SR (TXE/TC/RXNE/IDLE/ORE/FE/NE), DR с побочными
//     эффектами, двухтактный передатчик (DR -> сдвиговый регистр -> TC),
//     приёмник с очередью, признак простоя линии (IDLE);
//   * I2C1: SR1/SR2/DR с побочными эффектами, состояния шины
//     (START/SB/ADDR/TXE/BTF/RXNE/AF/STOPF), модель ведомого с регистрами
//     (как в mock_bus.hpp) и ошибки шины (BERR);
//   * NVIC: ISER/ICER, поэтому «прерывание» в тесте возникает ровно тогда,
//     когда оно возникло бы на железе: флаг разрешён в CR и разрешён в NVIC;
//   * RCC: регистры включения тактирования.
//   * DMA1: модель контроллера — tests/host/fake_dma.cpp (реализует
//     контракт embed::platform_dma_*, см. os/interfaces/dma_manager.hpp).
//
// ЧЕГО ЗДЕСЬ НЕТ
// Это НЕ эталон RM0008: модель воспроизводит только то поведение регистров,
// на которое опираются драйверы. Она проверяет логику драйвера (кто кого
// будит, какие биты включаются, когда приходит завершение), но не заменяет
// проверку на реальном железе.
// ---------------------------------------------------------------------------
#include "stm32f10x.h"     // настоящие маски и номера IRQ
#include "core_cm3.h"

#include "os/common.hpp"   // Size

namespace embed::test {

// ─────────────────────── Регистр с побочными эффектами ──────────────────────
//
// Модель регистра: чтение и запись уходят в модель периферии. Без этого
// нельзя проверить главное свойство драйвера — что он снимает флаги именно
// так, как это делает железо (например, что ORE снимается чтением DR).
template <typename Owner>
class Reg {
public:
    void bind(Owner& owner, uint8_t index) noexcept {
        owner_ = &owner;
        index_ = index;
    }

    Reg& operator=(uint32_t value) noexcept { return assign(value); }

    /// |= и &= работают как на железе: чтение, затем запись.
    /// Шаблон нужен потому, что маски в CMSIS имеют тип uint16_t, а
    /// выражения вида ~(A | B) дают int.
    template <typename T>
    Reg& operator|=(T value) noexcept {
        return assign(read() | static_cast<uint32_t>(value));
    }

    template <typename T>
    Reg& operator&=(T value) noexcept {
        return assign(read() & static_cast<uint32_t>(value));
    }

    operator uint32_t() const noexcept { return read(); }

private:
    uint32_t read() const noexcept {
        return (owner_ != nullptr) ? owner_->onRegRead(index_) : 0u;
    }

    Reg& assign(uint32_t value) noexcept {
        if (owner_ != nullptr) owner_->onRegWrite(index_, value);
        return *this;
    }

    Owner*  owner_ = nullptr;
    uint8_t index_ = 0u;
};

// ─────────────────────────────────── NVIC ───────────────────────────────────
class NvicModel {
public:
    /// Запись в ISER включает прерывание, запись в ICER — выключает.
    class WrReg {
    public:
        void bind(NvicModel& owner, uint8_t index, bool set) noexcept {
            owner_ = &owner;
            index_ = index;
            set_   = set;
        }
        uint32_t operator=(uint32_t value) noexcept {
            if (owner_ != nullptr) owner_->write(index_, value, set_);
            return value;
        }

    private:
        NvicModel* owner_ = nullptr;
        uint8_t    index_ = 0u;
        bool       set_   = true;
    };

    struct Regs {
        WrReg ISER[8];
        WrReg ICER[8];
    } regs;

    NvicModel() noexcept {
        for (uint8_t i = 0u; i < 8u; ++i) {
            regs.ISER[i].bind(*this, i, true);
            regs.ICER[i].bind(*this, i, false);
        }
    }

    /// Разрешено ли прерывание в NVIC — то, что драйвер обязан выставить
    /// для стратегий Async/Dma и снять для Polling.
    bool enabled(uint32_t irqn) const noexcept {
        return (iser_[irqn >> 5u] & (1UL << (irqn & 0x1Fu))) != 0u;
    }

    void reset() noexcept {
        for (auto& r : iser_) r = 0u;
    }

private:
    void write(uint8_t index, uint32_t value, bool set) noexcept {
        if (index >= 8u) return;
        if (set) iser_[index] |= value;
        else     iser_[index] &= ~value;
    }

    uint32_t iser_[8]{};
};

// ─────────────────────────────────── RCC ────────────────────────────────────
struct RccModel {
    struct Regs {
        volatile uint32_t CR, CFGR, CIR, APB2RSTR, APB1RSTR;
        volatile uint32_t AHBENR, APB2ENR, APB1ENR, BDCR, CSR;
    } regs;
};

// ────────────────────────────────── USART1 ──────────────────────────────────
class UsartModel {
public:
    enum : uint8_t {
        kSr = 0u, kDr, kBrr, kCr1, kCr2, kCr3, kGtpr, kRegCount
    };

    struct Regs {
        Reg<UsartModel> SR, DR, BRR, CR1, CR2, CR3, GTPR;
    } regs;

    static constexpr Size kFifo = 64u;

    UsartModel() noexcept { reset(); }

    void reset() noexcept;

    // ── Регистровые хуки (их вызывают прокси-регистры) ──
    uint32_t onRegRead(uint8_t index) noexcept;
    void     onRegWrite(uint8_t index, uint32_t value) noexcept;

    // ───────────────────────────── «Железо» ─────────────────────────────

    /// Байт пришёл в приёмник. Если DR занят — это переполнение (ORE):
    /// ровно так же теряется байт и на реальном USART.
    void feedByte(uint8_t b) noexcept;
    void feed(const void* data, Size len) noexcept;

    /// Линия замолчала после приёма (IDLE) — конец пачки.
    void leaveIdle() noexcept;

    /// Один шаг передатчика: DR -> сдвиговый регистр -> линия.
    /// Вызывается тестом явно, чтобы момент завершения передачи (TC) можно
    /// было проверить, а не предполагать.
    void tick() noexcept;

    /// Аппаратная ошибка приёма (ORE/FE/NE) — для проверки разбора ошибок.
    void injectRxError(uint32_t sr_flags) noexcept {
        sr_ |= sr_flags;
    }

    // ──────────────────────────── Наблюдение ────────────────────────────
    Size txLen() const noexcept { return tx_len_; }
    const uint8_t* txBytes() const noexcept { return tx_; }
    bool irqPending() const noexcept;
    bool txReady() const noexcept { return (sr_ & USART_SR_TXE) != 0u; }
    bool rxReady() const noexcept { return (sr_ & USART_SR_RXNE) != 0u; }
    /// Разрешены ли DMA-запросы передатчика/приёмника (DMAT/DMAR в CR3) —
    /// без них DMA-переноса не будет.
    bool dmaTxEnabled() const noexcept { return (val_[kCr3] & USART_CR3_DMAT) != 0u; }
    bool dmaRxEnabled() const noexcept { return (val_[kCr3] & USART_CR3_DMAR) != 0u; }
    /// Был ли потерян байт из-за переполнения приёмника (диагностика тестов).
    bool overflowed() const noexcept { return overflow_; }

    // ──────────────────────── Сторона DMA (CPAR) ────────────────────────
    /// DMA положил байт в DR. false — периферия не готова (TXE снят).
    bool dmaPush(uint8_t b) noexcept;
    /// DMA забрал байт из DR. false — данных нет (RXNE снят).
    bool dmaPop(uint8_t& b) noexcept;

    /// Продвигать «железо» при каждом чтении SR.
    ///
    /// Нужно ровно для одного сценария: порт в режиме Polling опрашивает
    /// регистр в цикле ожидания, и время в этот момент «идёт» именно там.
    /// В режимах Async/Dma тест продвигает модель сам (tick()), чтобы
    /// проверять порядок событий, а не полагаться на побочный эффект.
    bool auto_tick = false;

private:
    void deliverRx() noexcept;

    uint32_t sr_  = 0u;
    uint32_t val_[kRegCount]{};

    uint8_t rx_[kFifo]{};
    Size    rx_len_ = 0u;
    Size    rx_pos_ = 0u;

    uint8_t tx_[kFifo]{};
    Size    tx_len_ = 0u;

    uint8_t tx_hold_  = 0u;      // байт в DR
    uint8_t tx_shift_ = 0u;      // байт в сдвиговом регистре
    bool    hold_valid_  = false;
    bool    shift_valid_ = false;
    bool    overflow_    = false;   // для диагностики тестов
};

// ─────────────────────────────────── I2C1 ───────────────────────────────────
class I2cModel {
public:
    enum : uint8_t {
        kCr1 = 0u, kCr2, kOar1, kOar2, kDr, kSr1, kSr2, kCcr, kTrise, kRegCount
    };

    struct Regs {
        Reg<I2cModel> CR1, CR2, OAR1, OAR2, DR, SR1, SR2, CCR, TRISE;
    } regs;

    static constexpr unsigned kMaxDevices = 4u;
    static constexpr Size     kRegs       = 32u;

    I2cModel() noexcept { reset(); }

    void reset() noexcept;

    // ── Регистровые хуки ──
    uint32_t onRegRead(uint8_t index) noexcept;
    void     onRegWrite(uint8_t index, uint32_t value) noexcept;

    // ───────────────────────────── «Железо» ─────────────────────────────

    /// Устройство на шине (регистровая модель: запись байта адреса регистра,
    /// затем байта значения; чтение — с последнего адреса).
    bool registerDevice(uint8_t address) noexcept;

    /// Шина «залипла»: START не отрабатывает, вместо SB — ошибка шины.
    void setBusStuck(bool stuck) noexcept { bus_stuck_ = stuck; }

    /// Ошибка шины (BERR) в следующей операции.
    void injectBusError() noexcept { sr1_ |= I2C_SR1_BERR; }

    /// Один шаг обмена: продвигает байт через DR <-> сдвиговый регистр.
    void tick() noexcept;

    // ──────────────────────────── Наблюдение ────────────────────────────
    bool irqEvPending() const noexcept;
    bool irqErPending() const noexcept;
    bool txReady() const noexcept { return (sr1_ & I2C_SR1_TXE) != 0u; }
    bool rxReady() const noexcept { return (sr1_ & I2C_SR1_RXNE) != 0u; }
    bool dmaEnabled() const noexcept {
        return (val_[kCr2] & I2C_CR2_DMAEN) != 0u;
    }
    uint8_t deviceReg(unsigned index) const noexcept {
        return (index < kRegs) ? dev_regs_[index] : 0u;
    }
    void setDeviceReg(unsigned index, uint8_t value) noexcept {
        if (index < kRegs) dev_regs_[index] = value;
    }
    uint8_t lastReg() const noexcept { return last_reg_; }
    /// Задать «последний адрес регистра» — как это делает предыдущая запись.
    void setLastReg(uint8_t reg) noexcept { last_reg_ = static_cast<uint8_t>(reg % kRegs); }
    /// Сколько STOP было сгенерировано (проверка завершения обмена).
    unsigned stopCount() const noexcept { return stop_count_; }

    // ──────────────────────── Сторона DMA (CPAR) ────────────────────────
    bool dmaPush(uint8_t b) noexcept;
    bool dmaPop(uint8_t& b) noexcept;

private:
    enum class Bus : uint8_t {
        Idle = 0,
        Started,    // START выставлен, ждём байт адреса
        AddrTx,     // адрес принят, направление — запись
        AddrRx,     // адрес принят, направление — чтение
        Tx,         // идут данные от ведущего
        Rx,         // идут данные к ведущему
        RxTail,     // STOP при приёме: последний байт ещё можно прочитать
    };

    bool slavePresent(uint8_t address) const noexcept;
    void serveRxByte() noexcept;
    void applySlaveWrites() noexcept;
    void finishStop() noexcept;

    uint32_t val_[kRegCount]{};
    uint32_t sr1_ = 0u;
    uint32_t sr2_ = 0u;
    bool     sr1_read_ = false;

    Bus bus_ = Bus::Idle;
    bool bus_stuck_ = false;

    struct Device {
        uint8_t address = 0u;
        bool    present = false;
    };
    Device  devices_[kMaxDevices]{};

    uint8_t dev_regs_[kRegs]{};
    uint8_t last_reg_ = 0u;

    uint8_t tx_buf_[16]{};      // что ведомый принял в текущей транзакции
    Size    tx_buf_len_ = 0u;

    uint8_t rx_byte_ = 0u;
    Size    rx_index_ = 0u;

    bool     hold_  = false;    // байт в DR
    bool     shift_ = false;    // байт в сдвиговом регистре
    unsigned stop_count_ = 0u;
};

// ─────────────────────────── Глобальные модели ──────────────────────────────
extern UsartModel g_usart1;
extern I2cModel   g_i2c1;
extern RccModel   g_rcc;
extern NvicModel  g_nvic;

/// Полный сброс «железа» между тестами.
void fakeReset() noexcept;

} // namespace embed::test

// ─────────────── Подмена указателей на периферию (CMSIS -> модель) ──────────
#undef USART1
#undef I2C1
#undef RCC
#undef NVIC

#define USART1 (&::embed::test::g_usart1.regs)
#define I2C1   (&::embed::test::g_i2c1.regs)
#define RCC    (&::embed::test::g_rcc.regs)
#define NVIC   (&::embed::test::g_nvic.regs)
