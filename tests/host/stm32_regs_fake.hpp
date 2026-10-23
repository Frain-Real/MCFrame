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
//
// Регистры тактирования. CR/CFGR/BDCR/CSR — с побочными эффектами: модель
// выставляет биты готовности (HSIRDY/HSERDY/PLLRDY/LSERDY/LSIRDY) и
// переключает SWS вслед за SW. Без этого нельзя проверить ни смену профиля
// частоты, ни RTC — код обязан ЖДАТЬ готовности, а не предполагать её.
class RccModel {
public:
    enum : uint8_t {
        kCr = 0u, kCfgr, kCir, kApb2Rstr, kApb1Rstr,
        kAhbenr, kApb2enr, kApb1enr, kBdcr, kCsr, kRegCount
    };

    struct Regs {
        Reg<RccModel> CR, CFGR, CIR, APB2RSTR, APB1RSTR;
        Reg<RccModel> AHBENR, APB2ENR, APB1ENR, BDCR, CSR;
    } regs;

    RccModel() noexcept {
        regs.CR.bind(*this, kCr);
        regs.CFGR.bind(*this, kCfgr);
        regs.CIR.bind(*this, kCir);
        regs.APB2RSTR.bind(*this, kApb2Rstr);
        regs.APB1RSTR.bind(*this, kApb1Rstr);
        regs.AHBENR.bind(*this, kAhbenr);
        regs.APB2ENR.bind(*this, kApb2enr);
        regs.APB1ENR.bind(*this, kApb1enr);
        regs.BDCR.bind(*this, kBdcr);
        regs.CSR.bind(*this, kCsr);
    }

    uint32_t onRegRead(uint8_t index) noexcept;
    void     onRegWrite(uint8_t index, uint32_t value) noexcept;

    void reset() noexcept;

    /// Частота SYSCLK, как её видит SystemCoreClockUpdate().
    uint32_t sysclk_hz() const noexcept;
    /// Источник SYSCLK выбран (SWS) — проверка смены профиля.
    uint32_t sws() const noexcept;
    /// Выбран ли источником RTC кварц LSE.
    bool lse_selected() const noexcept;
    bool lsi_enabled() const noexcept;

private:
    uint32_t val_[kRegCount]{};
};

// ───────────────────────── PWR: режимы энергосбережения ─────────────────────
class PwrModel {
public:
    enum : uint8_t { kCr = 0u, kCsr, kRegCount };

    struct Regs {
        Reg<PwrModel> CR, CSR;
    } regs;

    PwrModel() noexcept {
        regs.CR.bind(*this, kCr);
        regs.CSR.bind(*this, kCsr);
    }

    uint32_t onRegRead(uint8_t index) noexcept {
        return (index < kRegCount) ? val_[index] : 0u;
    }
    void onRegWrite(uint8_t index, uint32_t value) noexcept {
        if (index < kRegCount) val_[index] = value;
    }

    void reset() noexcept { for (auto& v : val_) v = 0u; }

    uint32_t cr() const noexcept { return val_[kCr]; }
    bool low_power_regulator() const noexcept { return (val_[kCr] & PWR_CR_LPDS) != 0u; }
    bool power_down_deepsleep() const noexcept { return (val_[kCr] & PWR_CR_PDDS) != 0u; }

private:
    uint32_t val_[kRegCount]{};
};

// ──────────────────────────── FLASH: wait states ────────────────────────────
struct FlashModel {
    struct Regs {
        volatile uint32_t ACR, KEYR, OPTKEYR, SR, CR, AR;
    } regs;

    void reset() noexcept { regs.ACR = 0u; }
    /// Число wait states, выставленное в LATENCY.
    uint32_t latency() const noexcept { return regs.ACR & FLASH_ACR_LATENCY; }
};

// ──────────────────────────────────── RTC ───────────────────────────────────
//
// Ключевое свойство модели: регистры PRL/CNT/ALR пишутся ТОЛЬКО в режиме
// конфигурации (бит CNF в CRL) — как на железе. Если код забудет выставить
// CNF, будильник не будет запрограммирован, и тест это увидит.
class RtcModel {
public:
    enum : uint8_t {
        kCrl = 0u, kCrh, kPrlh, kPrll, kDivh, kDivl,
        kCnth, kCntl, kAlrh, kAlrl, kRegCount
    };

    struct Regs {
        Reg<RtcModel> CRL, CRH, PRLH, PRLL, DIVH, DIVL, CNTH, CNTL, ALRH, ALRL;
    } regs;

    RtcModel() noexcept {
        regs.CRL.bind(*this, kCrl);
        regs.CRH.bind(*this, kCrh);
        regs.PRLH.bind(*this, kPrlh);
        regs.PRLL.bind(*this, kPrll);
        regs.DIVH.bind(*this, kDivh);
        regs.DIVL.bind(*this, kDivl);
        regs.CNTH.bind(*this, kCnth);
        regs.CNTL.bind(*this, kCntl);
        regs.ALRH.bind(*this, kAlrh);
        regs.ALRL.bind(*this, kAlrl);
    }

    uint32_t onRegRead(uint8_t index) noexcept;
    void     onRegWrite(uint8_t index, uint32_t value) noexcept;

    void reset() noexcept;

    // ── «Железо» ──
    /// Счётчик RTC дошёл до будильника. Если будильник не запрограммирован,
    /// пробуждения не будет: ровно так выглядит конфигурация без источника.
    bool fireAlarm() noexcept;
    /// Продвинуть счётчик (тест управляет временем сам).
    void advanceSeconds(uint32_t seconds) noexcept { counter_ += seconds; }

    // ── Наблюдение ──
    uint32_t counter() const noexcept { return counter_; }
    uint32_t alarm() const noexcept { return alarm_; }
    bool     alarm_programmed() const noexcept { return alarm_programmed_; }
    bool     alarm_fired() const noexcept { return alarm_fired_; }
    bool     config_mode() const noexcept { return config_mode_; }
    uint32_t prescaler() const noexcept { return prescaler_; }
    bool     prescaler_set() const noexcept { return prescaler_set_; }
    bool     alarm_irq_enabled() const noexcept { return (val_[kCrh] & RTC_CRH_ALRIE) != 0u; }

private:
    uint32_t val_[kRegCount]{};
    uint32_t counter_ = 0u;
    uint32_t alarm_ = 0u;
    uint32_t prescaler_ = 0u;
    bool     prescaler_set_ = false;
    bool     alarm_programmed_ = false;
    bool     alarm_fired_ = false;
    bool     config_mode_ = false;
};

// ────────────────────────────── EXTI (линия 17) ─────────────────────────────
class ExtiModel {
public:
    enum : uint8_t { kImr = 0u, kEmr, kRtsr, kFtsr, kSwier, kPr, kRegCount };

    struct Regs {
        Reg<ExtiModel> IMR, EMR, RTSR, FTSR, SWIER, PR;
    } regs;

    ExtiModel() noexcept {
        regs.IMR.bind(*this, kImr);
        regs.EMR.bind(*this, kEmr);
        regs.RTSR.bind(*this, kRtsr);
        regs.FTSR.bind(*this, kFtsr);
        regs.SWIER.bind(*this, kSwier);
        regs.PR.bind(*this, kPr);
    }

    uint32_t onRegRead(uint8_t index) noexcept;
    void     onRegWrite(uint8_t index, uint32_t value) noexcept;

    void reset() noexcept { for (auto& v : val_) v = 0u; pending_ = 0u; }

    /// Линия разрешена как источник пробуждения из STOP.
    bool masked(uint32_t line) const noexcept { return (val_[kImr] & (1u << line)) != 0u; }
    /// Флаг запроса линии выставлен (запись 1 в PR его снимает).
    bool pending(uint32_t line) const noexcept { return (pending_ & (1u << line)) != 0u; }

    void raise(uint32_t line) noexcept { pending_ |= (1u << line); }

private:
    uint32_t val_[kRegCount]{};
    uint32_t pending_ = 0u;
};


// ─────────────────────── IWDG: независимый сторожевой таймер ────────────────
//
// Модель воспроизводит ДВА свойства, на которых чаще всего ошибаются:
//   * PR/RLR пишутся только после ключа 0x5555 в KR (иначе запись теряется,
//     как на железе) — забытый ключ виден сразу;
//   * флаги PVU/RVU в SR снимаются НЕ мгновенно: первое чтение SR всё ещё
//     показывает обновление, поэтому код обязан ЖДАТЬ, а не считать запись
//     применённой. Без этого код «проходил» бы и с отсутствующим ожиданием.
// Счётчик таймера модель не крутит: срабатывание — забота теста.
class IwdgModel {
public:
    enum : uint8_t { kKr = 0u, kPr, kRlr, kSr, kRegCount };

    struct Regs {
        Reg<IwdgModel> KR, PR, RLR, SR;
    } regs;

    IwdgModel() noexcept {
        regs.KR.bind(*this, kKr);
        regs.PR.bind(*this, kPr);
        regs.RLR.bind(*this, kRlr);
        regs.SR.bind(*this, kSr);
        reset();
    }

    uint32_t onRegRead(uint8_t index) noexcept;
    void     onRegWrite(uint8_t index, uint32_t value) noexcept;

    void reset() noexcept;

    // ── Наблюдение ──
    bool     started() const noexcept { return started_; }
    bool     write_access() const noexcept { return write_access_; }
    uint32_t prescaler() const noexcept { return pr_; }
    uint32_t reload() const noexcept { return rlr_; }
    uint32_t start_keys() const noexcept { return start_keys_; }
    uint32_t reload_keys() const noexcept { return reload_keys_; }
    uint32_t sr_polls() const noexcept { return sr_polls_; }

    /// Таймаут, который даёт запрограммированная пара (PR, RLR) при частоте
    /// LSI hz. = (4 × 2^PR × RLR) / f
    uint32_t timeout_ms_at(uint32_t lsi_hz) const noexcept;

private:
    bool     started_      = false;
    bool     write_access_ = false;
    bool     pending_      = false;   ///< PVU/RVU: запись ещё «применяется»
    uint8_t  sr_reads_     = 0u;
    uint32_t pr_  = 0u;
    uint32_t rlr_ = 0u;
    uint32_t start_keys_  = 0u;
    uint32_t reload_keys_ = 0u;
    uint32_t sr_polls_    = 0u;
};

// ───────────────────── BKP: регистры, переживающие сброс ────────────────────
//
// Запись разрешена только при DBP=1 в PWR_CR — как на железе: без этого
// «посмертная отметка» молча терялась бы, и после сброса причину было бы
// не узнать.
class BkpModel {
public:
    enum : uint8_t { kDr1 = 0u, kDrCount = 10u };

    struct Regs {
        Reg<BkpModel> DR1, DR2, DR3, DR4, DR5;
        Reg<BkpModel> DR6, DR7, DR8, DR9, DR10;
    } regs;

    BkpModel() noexcept {
        regs.DR1.bind(*this, 0u);
        regs.DR2.bind(*this, 1u);
        regs.DR3.bind(*this, 2u);
        regs.DR4.bind(*this, 3u);
        regs.DR5.bind(*this, 4u);
        regs.DR6.bind(*this, 5u);
        regs.DR7.bind(*this, 6u);
        regs.DR8.bind(*this, 7u);
        regs.DR9.bind(*this, 8u);
        regs.DR10.bind(*this, 9u);
    }

    uint32_t onRegRead(uint8_t index) noexcept {
        return (index < kDrCount) ? val_[index] : 0u;
    }
    void onRegWrite(uint8_t index, uint32_t value) noexcept {
        if (index >= kDrCount) return;
        // Без DBP (доступ к резервной области разрешён) запись игнорируется —
        // ровно так ведёт себя железо.
        if (!write_allowed()) return;
        val_[index] = value & 0xFFFFu;
    }

    /// Установлено ли разрешение записи в резервную область.
    static bool write_allowed() noexcept;

    void reset() noexcept { for (auto& v : val_) v = 0u; }
    uint16_t value(uint32_t index) const noexcept {
        return (index < kDrCount) ? static_cast<uint16_t>(val_[index]) : 0u;
    }

private:
    uint32_t val_[kDrCount]{};
};

// ──────────────────── Ядро: SCB, SysTick и инструкции WFI ───────────────────
struct ScbModel {
    struct Regs {
        volatile uint32_t CPUID, ICSR, VTOR, AIRCR, SCR, CCR, SHPR[12], SHCSR, CFSR;
    } regs;

    void reset() noexcept { regs.SCR = 0u; }
    bool sleepdeep() const noexcept {
        return (regs.SCR & SCB_SCR_SLEEPDEEP_Msk) != 0u;
    }
};

struct SysTickModel {
    struct Regs {
        volatile uint32_t CTRL, LOAD, VAL, CALIB;
    } regs;

    void reset() noexcept { regs.CTRL = 0u; regs.LOAD = 0u; regs.VAL = 0u; }
    bool running() const noexcept { return (regs.CTRL & SysTick_CTRL_ENABLE_Msk) != 0u; }
};

/// Модель «инструкций ядра»: считает входы в сон, чтобы тест проверял не
/// намерение, а факт остановки ядра.
struct CoreModel {
    uint32_t wfi_calls      = 0u;   ///< любые входы в сон
    uint32_t deep_wfi_calls = 0u;   ///< входы в сон при выставленном SLEEPDEEP
    uint32_t dsb_calls      = 0u;
    /// Ядро вошло в STOP без вооружённого источника пробуждения — то есть
    /// на железе оно не проснулось бы никогда. Тест обязан это заметить.
    bool     stuck_in_stop  = false;
    /// Шёл ли системный тик В МОМЕНТ остановки ядра. Для STOP обязан быть
    /// false: иначе первое же прерывание тика вывело бы ядро из сна через
    /// миллисекунды, и глубокого сна не получилось бы.
    bool     systick_running_at_deep_wfi = false;
    /// Разрешено ли в NVIC прерывание будильника RTC в момент остановки ядра:
    /// без этого STOP нечем разбудить (линия EXTI сама по себе ядро не
    /// выводит — нужно разрешённое прерывание).
    bool     alarm_irq_enabled_at_wfi = false;

    void reset() noexcept {
        wfi_calls = deep_wfi_calls = dsb_calls = 0u;
        stuck_in_stop = false;
        systick_running_at_deep_wfi = false;
        alarm_irq_enabled_at_wfi = false;
    }
};

// ─────────────────────────── Глобальные модели ──────────────────────────────
// Объявлены ПОСЛЕ всех моделей: USART1/I2C1 объявлены ниже.

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
extern UsartModel g_usart2;
extern UsartModel g_usart3;
extern I2cModel   g_i2c1;
extern RccModel   g_rcc;
extern NvicModel  g_nvic;
extern PwrModel   g_pwr;
extern IwdgModel  g_iwdg;
extern BkpModel   g_bkp;
extern FlashModel g_flash;
extern RtcModel   g_rtc;
extern ExtiModel  g_exti;
extern ScbModel   g_scb;
extern SysTickModel g_systick;
extern CoreModel  g_core;

/// Заглушки инструкций ядра: на хосте ARM-ассемблер недоступен, поэтому
/// __WFI/__DSB заменяются вызовами модели (в core_cm3.h это static inline
/// функции, а макрос, объявленный ниже, перекрывает их вызовы).
void fakeWfi() noexcept;
void fakeDsb() noexcept;
void fakeIsb() noexcept;

/// Полный сброс «железа» между тестами.
void fakeReset() noexcept;

/// Сброс МКУ «как на железе»: периферия и ядро — в исходное состояние, а
/// РЕЗЕРВНАЯ ОБЛАСТЬ (BKP) СОХРАНЯЕТСЯ, и в RCC_CSR появляется флаг POR.
///
/// Именно так проверяется посмертная отметка: она обязана пережить
/// перезагрузку — в этом весь её смысл, и обычный fakeReset() (который чистит
/// модели ради изоляции тестов) для такой проверки не годится.
void fakeResetKeepBackup() noexcept;

} // namespace embed::test

// ─────────────── Подмена указателей на периферию (CMSIS -> модель) ──────────
#undef USART1
#undef USART2
#undef USART3
#undef I2C1
#undef RCC
#undef GPIOA
#undef SPI1
#undef TIM2

// Phase 3: простые модели GPIO/SPI/TIM. Они не воспроизводят обмен, но
// позволяют проверить раскладку конфигурационных регистров драйверов.
static ::GPIO_TypeDef g_gpio_a{};
static ::SPI_TypeDef  g_spi1{};
static ::TIM_TypeDef  g_tim2{};

#define GPIOA (&::g_gpio_a)
#define SPI1  (&::g_spi1)
#define TIM2  (&::g_tim2)
#undef NVIC
#undef PWR
#undef RTC
#undef EXTI
#undef FLASH
#undef SCB
#undef SysTick
#undef IWDG
#undef BKP

#define USART1 (&::embed::test::g_usart1.regs)
#define USART2 (&::embed::test::g_usart2.regs)
#define USART3 (&::embed::test::g_usart3.regs)
#define I2C1   (&::embed::test::g_i2c1.regs)
#define RCC    (&::embed::test::g_rcc.regs)
#define NVIC   (&::embed::test::g_nvic.regs)
#define PWR    (&::embed::test::g_pwr.regs)
#define RTC    (&::embed::test::g_rtc.regs)
#define EXTI   (&::embed::test::g_exti.regs)
#define FLASH  (&::embed::test::g_flash.regs)
#define SCB    (&::embed::test::g_scb.regs)
#define SysTick (&::embed::test::g_systick.regs)
#define IWDG   (&::embed::test::g_iwdg.regs)
#define BKP    (&::embed::test::g_bkp.regs)

// Инструкции ядра: на хосте нет ARM-ассемблера. Подменяются вызовами модели,
// поэтому тест видит ФАКТ остановки ядра, а не «код дошёл до этой строки».
// Макросы объявлены ПОСЛЕ включения core_cm3.h: так они перекрывают вызовы
// static inline функций __WFI()/__DSB()/__ISB() из CMSIS.
#define __WFI() ::embed::test::fakeWfi()
#define __DSB() ::embed::test::fakeDsb()
#define __ISB() ::embed::test::fakeIsb()
