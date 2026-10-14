// tests/host/stm32_regs_fake.cpp
// ---------------------------------------------------------------------------
// Реализация программной модели регистров STM32F103 (см. stm32_regs_fake.hpp).
// ---------------------------------------------------------------------------
#include "stm32_regs_fake.hpp"

namespace embed::test {

UsartModel g_usart1{};
I2cModel   g_i2c1{};
RccModel   g_rcc{};
NvicModel  g_nvic{};

void fakeReset() noexcept {
    g_usart1.reset();
    g_i2c1.reset();
    g_nvic.reset();
    g_rcc.regs.APB1ENR = 0u;
    g_rcc.regs.APB2ENR = 0u;
    g_rcc.regs.AHBENR  = 0u;
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
