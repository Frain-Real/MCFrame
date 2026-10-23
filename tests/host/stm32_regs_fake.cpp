// tests/host/stm32_regs_fake.cpp
// ---------------------------------------------------------------------------
// Реализация программной модели регистров STM32F103 (см. stm32_regs_fake.hpp).
// ---------------------------------------------------------------------------
#include "stm32_regs_fake.hpp"

namespace embed::test {

UsartModel   g_usart1{};
UsartModel   g_usart2{};
UsartModel   g_usart3{};
I2cModel     g_i2c1{};
RccModel     g_rcc{};
NvicModel    g_nvic{};
PwrModel     g_pwr{};
IwdgModel    g_iwdg{};
BkpModel     g_bkp{};
FlashModel   g_flash{};
RtcModel     g_rtc{};
ExtiModel    g_exti{};
ScbModel     g_scb{};
SysTickModel g_systick{};
CoreModel    g_core{};

void fakeReset() noexcept {
    g_usart1.reset();
    g_usart2.reset();
    g_usart3.reset();
    g_i2c1.reset();
    g_nvic.reset();
    g_rcc.reset();
    g_pwr.reset();
    g_iwdg.reset();
    g_bkp.reset();
    g_flash.reset();
    g_rtc.reset();
    g_exti.reset();
    g_scb.reset();
    g_systick.reset();
    g_core.reset();
}

void fakeResetKeepBackup() noexcept {
    // Копия fakeReset() БЕЗ g_bkp.reset(): резервные регистры переживают сброс,
    // иначе проверка посмертной отметки ничего не значила бы.
    g_usart1.reset();
    g_usart2.reset();
    g_usart3.reset();
    g_i2c1.reset();
    g_nvic.reset();
    g_rcc.reset();
    g_pwr.reset();
    g_iwdg.reset();
    g_flash.reset();
    g_rtc.reset();
    g_exti.reset();
    g_scb.reset();
    g_systick.reset();
    g_core.reset();
    // Сброс по включению питания: причина, которую увидит следующая загрузка.
    g_rcc.regs.CSR = RCC_CSR_PORRSTF;
}

// ══════════════════════════════════ ЯДРО ════════════════════════════════════

void fakeDsb() noexcept { ++g_core.dsb_calls; }
void fakeIsb() noexcept {}

void fakeWfi() noexcept {
    ++g_core.wfi_calls;
    if (!g_scb.sleepdeep()) return;          // обычный WFI: ждём прерывание

    ++g_core.deep_wfi_calls;
    g_core.systick_running_at_deep_wfi = g_systick.running();
    g_core.alarm_irq_enabled_at_wfi =
        g_nvic.enabled(static_cast<uint32_t>(RTCAlarm_IRQn));

    // STOP: ядро выходит только по вооружённому источнику. Им может быть
    // только будильник RTC на линии EXTI17 — если он не запрограммирован,
    // выхода не будет: ядро «остаётся» в STOP, и тест обязан это увидеть,
    // а не получить молчаливый успех.
    constexpr uint32_t kRtcAlarmLine = 17u;
    if (!g_rtc.alarm_programmed() || !g_exti.masked(kRtcAlarmLine)) {
        g_core.stuck_in_stop = true;
        return;
    }
    if (!g_rtc.fireAlarm()) {
        g_core.stuck_in_stop = true;
        return;
    }
    g_exti.raise(kRtcAlarmLine);
}

// ══════════════════════════════════ IWDG ════════════════════════════════════

void IwdgModel::reset() noexcept {
    started_ = false;
    write_access_ = false;
    pending_ = false;
    sr_reads_ = 0u;
    pr_ = 0u;
    rlr_ = 0u;
    start_keys_ = 0u;
    reload_keys_ = 0u;
    sr_polls_ = 0u;
}

uint32_t IwdgModel::onRegRead(uint8_t index) noexcept {
    if (index != kSr) return 0u;      // KR/PR/RLR на чтение отдают 0

    ++sr_polls_;
    if (!pending_) return 0u;

    // Запись «применяется» не мгновенно: на первом чтении флаги ещё стоят.
    // Код обязан ждать их снятия — модель это проверяет.
    if (++sr_reads_ >= 2u) {
        pending_   = false;
        sr_reads_  = 0u;
        return 0u;
    }
    return IWDG_SR_PVU | IWDG_SR_RVU;
}

void IwdgModel::onRegWrite(uint8_t index, uint32_t value) noexcept {
    switch (index) {
        case kKr:
            switch (value) {
                case 0x0000CCCCu: started_ = true;  ++start_keys_;  break;
                case 0x00005555u: write_access_ = true;            break;
                case 0x0000AAAAu: ++reload_keys_;                  break;
                default: break;
            }
            return;

        case kPr:
            // Без ключа 0x5555 запись теряется — как на железе.
            if (!write_access_) return;
            pr_  = value & IWDG_PR_PR;
            pending_ = true;
            sr_reads_ = 0u;
            return;

        case kRlr:
            if (!write_access_) return;
            rlr_ = value & 0x0FFFu;
            pending_ = true;
            sr_reads_ = 0u;
            return;

        default:
            return;
    }
}

uint32_t IwdgModel::timeout_ms_at(uint32_t lsi_hz) const noexcept {
    if (lsi_hz == 0u) return 0u;
    const uint64_t divider = 4ull << (pr_ & 7u);          // 4 × 2^PR
    return static_cast<uint32_t>((divider * rlr_ * 1000ull) / lsi_hz);
}

// ═══════════════════════════════════ BKP ════════════════════════════════════

bool BkpModel::write_allowed() noexcept {
    return (g_pwr.cr() & PWR_CR_DBP) != 0u;
}

// ══════════════════════════════════ RCC ═════════════════════════════════════

uint32_t RccModel::onRegRead(uint8_t index) noexcept {
    return (index < kRegCount) ? val_[index] : 0u;
}

void RccModel::onRegWrite(uint8_t index, uint32_t value) noexcept {
    if (index >= kRegCount) return;
    val_[index] = value;

    switch (index) {
        case kCr:
            // Готовность источников тактирования. HSIRDY/LSERDY на железе
            // появляются не мгновенно, но код обязан ЖДАТЬ их — проверяем
            // именно факт ожидания, а не задержку.
            if ((value & RCC_CR_HSION) != 0u) val_[kCr] |= RCC_CR_HSIRDY;
            else                              val_[kCr] &= ~RCC_CR_HSIRDY;

            if ((value & RCC_CR_HSEON) != 0u) val_[kCr] |= RCC_CR_HSERDY;
            else                              val_[kCr] &= ~RCC_CR_HSERDY;

            // PLL готов только при работающем HSE (так собрана плата: 8 МГц).
            if ((value & RCC_CR_PLLON) != 0u && (val_[kCr] & RCC_CR_HSERDY) != 0u) {
                val_[kCr] |= RCC_CR_PLLRDY;
            } else {
                val_[kCr] &= ~RCC_CR_PLLRDY;
            }
            break;

        case kCfgr: {
            // SWS повторяет SW: код, переключивший источник, обязан дождаться
            // этого бита, а не считать переключение мгновенным.
            const uint32_t sw = value & RCC_CFGR_SW;
            if (sw == RCC_CFGR_SW_HSI) {
                val_[kCfgr] = (val_[kCfgr] & ~RCC_CFGR_SWS) | RCC_CFGR_SWS_HSI;
            } else if (sw == RCC_CFGR_SW_HSE) {
                val_[kCfgr] = (val_[kCfgr] & ~RCC_CFGR_SWS) | RCC_CFGR_SWS_HSE;
            } else {
                // PLL: пока он не готов, SYSCLK остаётся прежним (HSI).
                if ((val_[kCr] & RCC_CR_PLLRDY) != 0u) {
                    val_[kCfgr] = (val_[kCfgr] & ~RCC_CFGR_SWS) | RCC_CFGR_SWS_PLL;
                }
            }
            break;
        }

        case kBdcr:
            if ((value & RCC_BDCR_LSEON) != 0u) val_[kBdcr] |= RCC_BDCR_LSERDY;
            else                                val_[kBdcr] &= ~RCC_BDCR_LSERDY;
            break;

        case kCsr: {
            if ((value & RCC_CSR_LSION) != 0u) val_[kCsr] |= RCC_CSR_LSIRDY;
            else                               val_[kCsr] &= ~RCC_CSR_LSIRDY;

            // RMVF: флаги причины сброса снимаются записью единицы. Без этого
            // «прочитал причину дважды» дало бы на модели другой ответ, чем на
            // плате, и проверка «читается один раз» ничего не значила бы.
            if ((value & RCC_CSR_RMVF) != 0u) {
                val_[kCsr] &= ~(RCC_CSR_PORRSTF | RCC_CSR_PINRSTF | RCC_CSR_SFTRSTF |
                                RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF | RCC_CSR_LPWRRSTF);
            }
            val_[kCsr] &= ~RCC_CSR_RMVF;   // сам бит RMVF не хранится
            break;
        }

        default:
            break;
    }
}

void RccModel::reset() noexcept {
    for (auto& v : val_) v = 0u;
    // После сброса ядро питается от HSI.
    val_[kCr] |= RCC_CR_HSION | RCC_CR_HSIRDY;
}

uint32_t RccModel::sws() const noexcept { return val_[kCfgr] & RCC_CFGR_SWS; }

uint32_t RccModel::sysclk_hz() const noexcept {
    const uint32_t src = sws();
    if (src == RCC_CFGR_SWS_PLL) return 72000000u;   // HSE 8 МГц × 9
    if (src == RCC_CFGR_SWS_HSE) return 8000000u;
    return 8000000u;                                  // HSI
}

bool RccModel::lse_selected() const noexcept {
    return (val_[kBdcr] & RCC_BDCR_RTCSEL) == RCC_BDCR_RTCSEL_LSE;
}

bool RccModel::lsi_enabled() const noexcept { return (val_[kCsr] & RCC_CSR_LSION) != 0u; }

// ══════════════════════════════════ RTC ═════════════════════════════════════

void RtcModel::reset() noexcept {
    for (auto& v : val_) v = 0u;
    counter_ = 0u;
    alarm_ = 0u;
    prescaler_ = 0u;
    prescaler_set_ = false;
    alarm_programmed_ = false;
    alarm_fired_ = false;
    config_mode_ = false;
}

uint32_t RtcModel::onRegRead(uint8_t index) noexcept {
    switch (index) {
        case kCrl: {
            // RTOFF всегда 1: запись в модель выполняется мгновенно (код
            // обязан её дождаться — на это и опирается последовательность).
            uint32_t v = RTC_CRL_RTOFF;
            if (config_mode_) v |= RTC_CRL_CNF;
            if (alarm_fired_) v |= RTC_CRL_ALRF;
            if (prescaler_set_) v |= RTC_CRL_RSF;   // регистры синхронизированы
            return v;
        }
        case kCnth: return (counter_ >> 16) & 0xFFFFu;
        case kCntl: return counter_ & 0xFFFFu;
        case kAlrh: return (alarm_ >> 16) & 0xFFFFu;
        case kAlrl: return alarm_ & 0xFFFFu;
        case kPrlh: return (prescaler_ >> 16) & 0xFFFFu;
        case kPrll: return prescaler_ & 0xFFFFu;
        default:    return val_[index];
    }
}

void RtcModel::onRegWrite(uint8_t index, uint32_t value) noexcept {
    switch (index) {
        case kCrl:
            // Запись — это read-modify-write (в коде `CRL &= ~ALRF`),
            // поэтому «снятие» флага видно как отсутствие бита в значении.
            if ((value & RTC_CRL_CNF) != 0u) config_mode_ = true;
            else                             config_mode_ = false;
            if ((value & RTC_CRL_ALRF) == 0u) alarm_fired_ = false;
            return;

        case kCrh:
            val_[kCrh] = value & RTC_CRH_ALRIE;
            return;

        // Регистры счётчика/будильника/прескалера пишутся ТОЛЬКО в режиме
        // конфигурации. На железе запись вне него молча теряется — здесь
        // так же, поэтому забытый CNF не «пройдёт» незамеченным.
        case kPrlh: case kPrll:
            if (!config_mode_) return;
            val_[index] = value;
            prescaler_ = (static_cast<uint32_t>(val_[kPrlh] & 0xFFFFu) << 16)
                       | (val_[kPrll] & 0xFFFFu);
            prescaler_set_ = true;
            return;

        case kCnth: case kCntl:
            if (!config_mode_) return;
            val_[index] = value;
            counter_ = (static_cast<uint32_t>(val_[kCnth] & 0xFFFFu) << 16)
                     | (val_[kCntl] & 0xFFFFu);
            return;

        case kAlrh: case kAlrl:
            if (!config_mode_) return;
            val_[index] = value;
            alarm_ = (static_cast<uint32_t>(val_[kAlrh] & 0xFFFFu) << 16)
                   | (val_[kAlrl] & 0xFFFFu);
            alarm_programmed_ = true;
            return;

        default:
            val_[index] = value;
            return;
    }
}

bool RtcModel::fireAlarm() noexcept {
    if (!alarm_programmed_) return false;
    // Будильник срабатывает, когда счётчик ДОХОДИТ до запрограммированного
    // значения: время «проходит» внутри STOP.
    if (counter_ < alarm_) counter_ = alarm_;
    alarm_fired_ = true;
    return true;
}

// ══════════════════════════════════ EXTI ════════════════════════════════════

uint32_t ExtiModel::onRegRead(uint8_t index) noexcept {
    if (index == kPr) return pending_;
    return (index < kRegCount) ? val_[index] : 0u;
}

void ExtiModel::onRegWrite(uint8_t index, uint32_t value) noexcept {
    if (index == kPr) {
        pending_ &= ~value;   // запись 1 снимает флаг (как на железе)
        return;
    }
    if (index < kRegCount) val_[index] = value;
}

// ══════════════════════════════════ USART1 ══════════════════════════════════

void UsartModel::reset() noexcept {
    regs.SR.bind(*this, kSr);
    regs.DR.bind(*this, kDr);
    regs.BRR.bind(*this, kBrr);
    regs.CR1.bind(*this, kCr1);
    regs.CR2.bind(*this, kCr2);
    regs.CR3.bind(*this, kCr3);
    regs.GTPR.bind(*this, kGtpr);

    for (auto& v : val_) v = 0u;

    // После включения передатчик пуст: TXE и TC установлены — ровно так же,
    // как на реальном USART. Драйвер обязан это учитывать: именно поэтому
    // перед проверкой TC он перечитывает SR после записи в DR.
    sr_ = static_cast<uint32_t>(USART_SR_TXE | USART_SR_TC);

    rx_len_ = rx_pos_ = 0u;
    tx_len_ = 0u;
    tx_hold_ = tx_shift_ = 0u;
    hold_valid_ = shift_valid_ = false;
    overflow_ = false;
    auto_tick = false;
}

uint32_t UsartModel::onRegRead(uint8_t index) noexcept {
    if (index == kSr) {
        const uint32_t v = sr_;
        // «Время идёт», пока драйвер опрашивает статус: так работает только
        // стратегия Polling — в ней цикл опроса и есть ожидание.
        if (auto_tick) tick();
        return v;
    }
    if (index == kDr) {
        const uint32_t v = val_[kDr] & 0xFFu;
        // Чтение DR снимает RXNE, IDLE и ошибки приёма (ORE/FE/NE).
        sr_ &= ~static_cast<uint32_t>(USART_SR_RXNE | USART_SR_IDLE |
                                      USART_SR_ORE  | USART_SR_FE |
                                      USART_SR_NE);
        deliverRx();                 // следующий байт из очереди — в DR
        return v;
    }
    return val_[index];
}

void UsartModel::onRegWrite(uint8_t index, uint32_t value) noexcept {
    val_[index] = value;
    if (index != kDr) return;

    // Запись в DR: байт принят передатчиком, но НЕ отправлен. TXE и TC
    // снимаются — поэтому драйвер обязан перечитать SR перед проверкой TC.
    if ((sr_ & USART_SR_TXE) == 0u) return;      // DR был занят: байт потерян
    tx_hold_ = static_cast<uint8_t>(value & 0xFFu);
    hold_valid_ = true;
    sr_ &= ~static_cast<uint32_t>(USART_SR_TXE | USART_SR_TC);
}

void UsartModel::deliverRx() noexcept {
    if ((sr_ & USART_SR_RXNE) != 0u) return;     // DR занят
    if (rx_pos_ >= rx_len_) return;
    val_[kDr] = rx_[rx_pos_++];
    sr_ |= static_cast<uint32_t>(USART_SR_RXNE);
}

void UsartModel::feedByte(uint8_t b) noexcept {
    sr_ &= ~static_cast<uint32_t>(USART_SR_IDLE);   // линия снова активна

    // Модель упрощена по сравнению с железом: байты складываются в FIFO,
    // переполнение (ORE) возникает только при её заполнении. Приёмный
    // регистр у F103 один, но «мгновенно» поданная пачка в тесте иначе
    // всегда давала бы ORE, и проверить разбор пачки было бы нельзя.
    if (rx_len_ >= kFifo) {
        overflow_ = true;
        sr_ |= static_cast<uint32_t>(USART_SR_ORE);
        return;
    }
    rx_[rx_len_++] = b;
    deliverRx();
}

void UsartModel::feed(const void* data, Size len) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
    for (Size i = 0u; i < len; ++i) feedByte(p[i]);
}

void UsartModel::leaveIdle() noexcept {
    sr_ |= static_cast<uint32_t>(USART_SR_IDLE);
}

void UsartModel::tick() noexcept {
    if (shift_valid_) {
        shift_valid_ = false;
        sr_ |= static_cast<uint32_t>(USART_SR_TC);   // сдвиг завершён
    }
    if (hold_valid_) {
        hold_valid_ = false;
        tx_shift_ = tx_hold_;
        shift_valid_ = true;
        if (tx_len_ < kFifo) tx_[tx_len_++] = tx_shift_;   // байт «ушёл в линию»
        sr_ |= static_cast<uint32_t>(USART_SR_TXE);        // DR свободен
        sr_ &= ~static_cast<uint32_t>(USART_SR_TC);        // передача не закончена
    }
}

bool UsartModel::irqPending() const noexcept {
    const uint32_t sr  = sr_;
    const uint32_t cr1 = val_[kCr1];

    const bool any =
        (((sr & USART_SR_RXNE) != 0u) && (cr1 & USART_CR1_RXNEIE) != 0u) ||
        (((sr & USART_SR_TXE)  != 0u) && (cr1 & USART_CR1_TXEIE)  != 0u) ||
        (((sr & USART_SR_TC)   != 0u) && (cr1 & USART_CR1_TCIE)   != 0u) ||
        (((sr & USART_SR_IDLE) != 0u) && (cr1 & USART_CR1_IDLEIE) != 0u) ||
        (((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0u) &&
         (cr1 & USART_CR1_RXNEIE) != 0u);

    // Прерывание возникает, только если оно разрешено И в периферии, И в NVIC.
    return any && g_nvic.enabled(static_cast<uint32_t>(USART1_IRQn));
}

bool UsartModel::dmaPush(uint8_t b) noexcept {
    if ((sr_ & USART_SR_TXE) == 0u) return false;   // периферия не готова
    onRegWrite(kDr, b);
    return true;
}

bool UsartModel::dmaPop(uint8_t& b) noexcept {
    if ((sr_ & USART_SR_RXNE) == 0u) return false;
    b = static_cast<uint8_t>(val_[kDr] & 0xFFu);
    // Снимается ТОЛЬКО RXNE: IDLE снимается парой «чтение SR + чтение DR»,
    // а DMA читает один DR. Поэтому флаг IDLE остаётся драйверу — иначе
    // в DMA-режиме конец пачки было бы нечем заметить.
    sr_ &= ~static_cast<uint32_t>(USART_SR_RXNE);
    deliverRx();
    return true;
}

// ═══════════════════════════════════ I2C1 ═══════════════════════════════════

void I2cModel::reset() noexcept {
    regs.CR1.bind(*this, kCr1);
    regs.CR2.bind(*this, kCr2);
    regs.OAR1.bind(*this, kOar1);
    regs.OAR2.bind(*this, kOar2);
    regs.DR.bind(*this, kDr);
    regs.SR1.bind(*this, kSr1);
    regs.SR2.bind(*this, kSr2);
    regs.CCR.bind(*this, kCcr);
    regs.TRISE.bind(*this, kTrise);

    for (auto& v : val_) v = 0u;
    sr1_ = 0u;
    sr2_ = 0u;
    sr1_read_ = false;
    bus_ = Bus::Idle;
    bus_stuck_ = false;

    for (auto& d : devices_) d = Device{};
    for (auto& r : dev_regs_) r = 0u;
    last_reg_ = 0u;

    tx_buf_len_ = 0u;
    for (auto& b : tx_buf_) b = 0u;
    rx_byte_ = 0u;
    rx_index_ = 0u;
    hold_ = false;
    shift_ = false;
    stop_count_ = 0u;
}

bool I2cModel::registerDevice(uint8_t address) noexcept {
    for (auto& d : devices_) {
        if (d.present && d.address == (address & 0x7Fu)) return false;
    }
    for (auto& d : devices_) {
        if (!d.present) {
            d.address = static_cast<uint8_t>(address & 0x7Fu);
            d.present = true;
            return true;
        }
    }
    return false;
}

bool I2cModel::slavePresent(uint8_t address) const noexcept {
    for (const auto& d : devices_) {
        if (d.present && d.address == (address & 0x7Fu)) return true;
    }
    return false;
}

void I2cModel::applySlaveWrites() noexcept {
    if (tx_buf_len_ == 0u) return;
    // Та же регистровая модель, что и в mock_bus.hpp: два байта — адрес
    // регистра и значение; один байт — только адрес регистра.
    if (tx_buf_len_ >= 2u) dev_regs_[tx_buf_[0] % kRegs] = tx_buf_[1];
    else                   last_reg_ = static_cast<uint8_t>(tx_buf_[0] % kRegs);
    tx_buf_len_ = 0u;
}

void I2cModel::serveRxByte() noexcept {
    if (bus_ != Bus::Rx) return;
    if ((sr1_ & I2C_SR1_RXNE) != 0u) return;
    val_[kDr] = dev_regs_[(last_reg_ + rx_index_) % kRegs];
    ++rx_index_;
    sr1_ |= static_cast<uint32_t>(I2C_SR1_RXNE);
}

void I2cModel::finishStop() noexcept {
    applySlaveWrites();
    sr1_ |= static_cast<uint32_t>(I2C_SR1_STOPF);
    ++stop_count_;

    // STOP при приёме: последний байт уже в DR, и его ещё можно прочитать —
    // именно так работает «NACK + STOP, затем чтение последнего байта».
    bus_ = ((bus_ == Bus::Rx) && ((sr1_ & I2C_SR1_RXNE) != 0u)) ? Bus::RxTail
                                                               : Bus::Idle;
}

uint32_t I2cModel::onRegRead(uint8_t index) noexcept {
    switch (index) {
        case kSr1:
            sr1_read_ = true;
            return sr1_;

        case kSr2: {
            const uint32_t v = sr2_;
            sr1_ &= ~static_cast<uint32_t>(I2C_SR1_ADDR);   // чтение SR2 снимает ADDR
            if (bus_ == Bus::AddrTx) {
                bus_ = Bus::Tx;
                sr1_ |= static_cast<uint32_t>(I2C_SR1_TXE); // DR свободен: можно писать
            } else if (bus_ == Bus::AddrRx) {
                bus_ = Bus::Rx;
                rx_index_ = 0u;
                serveRxByte();                              // первый байт от ведомого
            }
            return v;
        }

        case kDr: {
            const uint32_t v = val_[kDr] & 0xFFu;
            if (bus_ == Bus::Rx || bus_ == Bus::RxTail) {
                sr1_ &= ~static_cast<uint32_t>(I2C_SR1_RXNE);
                if (bus_ == Bus::Rx) serveRxByte();          // следующий байт
                else                 bus_ = Bus::Idle;       // хвост отдан
            }
            return v;
        }

        default:
            return val_[index];
    }
}

void I2cModel::onRegWrite(uint8_t index, uint32_t value) noexcept {
    switch (index) {
        case kCr1: {
            // Сброс контроллера.
            if ((value & I2C_CR1_SWRST) != 0u) {
                val_[kCr1] = 0u;
                sr1_ = sr2_ = 0u;
                bus_ = Bus::Idle;
                bus_stuck_ = false;     // «залипшая» шина лечится recover()
                tx_buf_len_ = 0u;
                return;
            }

            // STOPF снимается чтением SR1 с последующей записью в CR1.
            if (sr1_read_) {
                sr1_ &= ~static_cast<uint32_t>(I2C_SR1_STOPF);
                sr1_read_ = false;
            }

            uint32_t stored = value;

            if ((value & I2C_CR1_START) != 0u) {
                stored &= ~static_cast<uint32_t>(I2C_CR1_START);   // снимается железом
                if (bus_ == Bus::Idle) {
                    if (bus_stuck_) {
                        sr1_ |= static_cast<uint32_t>(I2C_SR1_BERR);
                    } else {
                        sr1_ |= static_cast<uint32_t>(I2C_SR1_SB);
                        bus_ = Bus::Started;
                    }
                } else if (bus_ == Bus::Tx || bus_ == Bus::AddrTx) {
                    // Повторный START без STOP (чтение регистра).
                    applySlaveWrites();
                    sr1_ |= static_cast<uint32_t>(I2C_SR1_SB);
                    bus_ = Bus::Started;
                }
            }

            if ((value & I2C_CR1_STOP) != 0u) {
                stored &= ~static_cast<uint32_t>(I2C_CR1_STOP);
                finishStop();
            }

            val_[kCr1] = stored;
            return;
        }

        case kSr1:
            // Флаги SR1 снимаются записью нуля в соответствующий бит.
            sr1_ &= value;
            return;

        case kDr: {
            val_[kDr] = value;
            const uint8_t b = static_cast<uint8_t>(value & 0xFFu);

            if (bus_ == Bus::Started) {
                // Байт адреса: бит 0 — направление.
                sr1_ &= ~static_cast<uint32_t>(I2C_SR1_SB);
                if (slavePresent(static_cast<uint8_t>(b >> 1u))) {
                    sr1_ |= static_cast<uint32_t>(I2C_SR1_ADDR);
                    bus_  = ((b & 1u) != 0u) ? Bus::AddrRx : Bus::AddrTx;
                } else {
                    // Устройство не ответило: NACK (AF).
                    sr1_ |= static_cast<uint32_t>(I2C_SR1_AF);
                    bus_  = Bus::Idle;
                }
                return;
            }

            if (bus_ == Bus::Tx) {
                if (tx_buf_len_ < sizeof(tx_buf_)) tx_buf_[tx_buf_len_++] = b;
                hold_ = true;
                sr1_ &= ~static_cast<uint32_t>(I2C_SR1_TXE | I2C_SR1_BTF);
            }
            return;
        }

        default:
            val_[index] = value;
            return;
    }
}

void I2cModel::tick() noexcept {
    if (shift_) {
        shift_ = false;
        sr1_ |= static_cast<uint32_t>(I2C_SR1_BTF);      // байт ушёл в линию
    }
    if (hold_) {
        hold_ = false;
        shift_ = true;
        sr1_ |= static_cast<uint32_t>(I2C_SR1_TXE);      // DR снова свободен
    }
}

bool I2cModel::irqEvPending() const noexcept {
    const uint32_t cr2 = val_[kCr2];
    if ((cr2 & I2C_CR2_ITEVTEN) == 0u) return false;

    const bool ev =
        ((sr1_ & (I2C_SR1_SB | I2C_SR1_ADDR | I2C_SR1_STOPF | I2C_SR1_BTF)) != 0u) ||
        (((sr1_ & (I2C_SR1_TXE | I2C_SR1_RXNE)) != 0u) &&
         (cr2 & I2C_CR2_ITBUFEN) != 0u);

    return ev && g_nvic.enabled(static_cast<uint32_t>(I2C1_EV_IRQn));
}

bool I2cModel::irqErPending() const noexcept {
    const uint32_t cr2 = val_[kCr2];
    if ((cr2 & I2C_CR2_ITERREN) == 0u) return false;

    const bool er =
        (sr1_ & (I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR)) != 0u;

    return er && g_nvic.enabled(static_cast<uint32_t>(I2C1_ER_IRQn));
}

bool I2cModel::dmaPush(uint8_t b) noexcept {
    if (bus_ != Bus::Tx) return false;
    if ((sr1_ & I2C_SR1_TXE) == 0u) return false;
    onRegWrite(kDr, b);
    return true;
}

bool I2cModel::dmaPop(uint8_t& b) noexcept {
    if (bus_ != Bus::Rx && bus_ != Bus::RxTail) return false;
    if ((sr1_ & I2C_SR1_RXNE) == 0u) return false;
    b = static_cast<uint8_t>(onRegRead(kDr) & 0xFFu);
    return true;
}

} // namespace embed::test
