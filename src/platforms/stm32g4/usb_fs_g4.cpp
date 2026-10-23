// platforms/stm32g4/usb_fs_g4.cpp
#include "usb_fs_g4.hpp"

#if defined(MCFRAME_SOC_STM32G4)

#include "stm32m4_min.h"

namespace embed::stm32g4::usb {
namespace {

constexpr uint32_t kUsbBase = 0x40005C00u;
constexpr uint32_t kPmaBase = 0x40006000u;
#define USB_REG(off) (*reinterpret_cast<volatile uint16_t*>((kUsbBase + (off))))
#define USB_PMA(off) (*reinterpret_cast<volatile uint16_t*>((kPmaBase + (off))))

constexpr uint16_t kEp0r = 0x00u;
constexpr uint16_t kEp1r = 0x04u;
constexpr uint16_t kCntr = 0x40u;
constexpr uint16_t kIstr = 0x44u;
constexpr uint16_t kDaddr = 0x4Cu;
constexpr uint16_t kBtable = 0x50u;
constexpr uint16_t kBcdr = 0x58u;

constexpr uint16_t kEpEaMask = 0x000Fu;
constexpr uint16_t kEpStatTx = 0x0030u;
constexpr uint16_t kEpStatRx = 0x00C0u;
constexpr uint16_t kEpTxValid = 0x0030u;
constexpr uint16_t kEpTxNak = 0x0020u;
constexpr uint16_t kEpTxStall = 0x0010u;
constexpr uint16_t kEpRxValid = 0x00C0u;
constexpr uint16_t kEpRxStall = 0x0040u;
constexpr uint16_t kEpCtrTx = 0x0100u;
constexpr uint16_t kEpCtrRx = 0x8000u;
constexpr uint16_t kEpSetup = 0x0800u;
constexpr uint16_t kCntrFres = 0x0001u;
constexpr uint16_t kCntrResetM = 0x0400u;
constexpr uint16_t kCntrCtrM = 0x8000u;
constexpr uint16_t kIstReset = 0x0400u;
constexpr uint16_t kIstCtr = 0x8000u;
constexpr uint16_t kIstEpMask = 0x000Fu;
constexpr uint16_t kIstDir = 0x0010u;
constexpr uint16_t kBcdrDppu = 0x8000u;

// PMA layout: BTABLE=0, descriptors 0..7 (64 B), buffers from 0x40.
constexpr uint16_t kEp0Rx = 0x0040u;
constexpr uint16_t kEp0Tx = 0x0080u;
constexpr uint16_t kEp1Rx = 0x00C0u;
constexpr uint16_t kEp1Tx = 0x0100u;

void pma_write(uint16_t addr, const void* data, Size len) noexcept {
    const auto* p = static_cast<const uint8_t*>(data);
    for (Size i = 0u; i < len; i += 2u) {
        uint16_t v = p[i];
        if (i + 1u < len) v |= static_cast<uint16_t>(p[i + 1u]) << 8;
        USB_PMA(static_cast<uint16_t>(addr + i)) = v;
    }
}
void pma_read(uint16_t addr, void* data, Size len) noexcept {
    auto* p = static_cast<uint8_t*>(data);
    for (Size i = 0u; i < len; i += 2u) {
        const uint16_t v = USB_PMA(static_cast<uint16_t>(addr + i));
        p[i] = static_cast<uint8_t>(v);
        if (i + 1u < len) p[i + 1u] = static_cast<uint8_t>(v >> 8);
    }
}
void pma_write16(uint16_t addr, uint16_t v) noexcept { USB_PMA(addr) = v; }

void ep_rx_count(uint8_t ep, uint16_t count) noexcept {
    const uint16_t b = static_cast<uint16_t>(ep * 8u);
    if (ep == 0u) pma_write16(static_cast<uint16_t>(b + 6u), count);
    else pma_write16(static_cast<uint16_t>(b + 6u),
                     static_cast<uint16_t>((1u << 15) | (1u << 10) | count));
}
void ep_tx_count(uint8_t ep, uint16_t count) noexcept {
    pma_write16(static_cast<uint16_t>(ep * 8u + 2u), count);
}
void ep_addr(uint8_t ep, uint16_t rx, uint16_t tx) noexcept {
    pma_write16(static_cast<uint16_t>(ep * 8u + 4u), rx);
    pma_write16(static_cast<uint16_t>(ep * 8u), tx);
}

} // namespace

Status UsbFsG4Hal::init(SetupCallback on_setup, OutCallback on_out, void* ctx) noexcept {
    setup_cb_ = on_setup; out_cb_ = on_out; ctx_ = ctx;
    ctrl_out_len_ = ctrl_pos_ = 0u;
    ctrl_in_ = nullptr; ctrl_in_len_ = ctrl_in_pos_ = 0u; ctrl_in_active_ = false;
    pending_setup_ = embed::usb::Setup{};
    expect_out_ = false;
    pending_address_ = 0u; address_pending_ = false;
    bulk_out_len_ = bulk_out_pos_ = 0u;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    MCFRAME_RCC_APB1ENR |= (1u << 26);            // USBEN (APB1ENR1, G4)
    GPIOA->MODER = (GPIOA->MODER & ~((3u << 22) | (3u << 24))) |
                   ((2u << 22) | (2u << 24));
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFu << 12) | (0xFu << 16))) |
                    ((10u << 12) | (10u << 16));

    USB_REG(kCntr) = kCntrFres;
    for (volatile uint32_t i = 0u; i < 10000u; ++i) { }
    USB_REG(kCntr) = 0u;
    USB_REG(kBtable) = 0u;
    ep_addr(0u, kEp0Rx, kEp0Tx);
    ep_addr(1u, kEp1Rx, kEp1Tx);
    ep_rx_count(0u, 64u);
    ep_rx_count(1u, 64u);
    USB_REG(kEp0r) = static_cast<uint16_t>(kEpRxValid | kEpTxNak);
    USB_REG(kEp1r) = static_cast<uint16_t>(kEpRxValid | kEpTxNak | 1u);
    USB_REG(kDaddr) = 0x80u;
    USB_REG(kBcdr) |= kBcdrDppu;
    USB_REG(kCntr) = static_cast<uint16_t>(kCntrCtrM | kCntrResetM);
    inited_ = true;
    return Status::Ok;
}

void UsbFsG4Hal::poll() noexcept {
    if (!inited_) return;
    const uint16_t istr = USB_REG(kIstr);
    if ((istr & kIstReset) != 0u) {
        USB_REG(kIstr) = static_cast<uint16_t>(~kIstReset);
        handle_reset();
    }
    if ((istr & kIstCtr) != 0u) {
        const uint8_t ep = static_cast<uint8_t>(istr & kIstEpMask);
        USB_REG(kIstr) = static_cast<uint16_t>(~kIstCtr);
        if (ep == 0u) handle_ep0();
        else if (ep == 1u) handle_ep1();
    }
}

void UsbFsG4Hal::handle_reset() noexcept {
    USB_REG(kDaddr) = 0x80u;
    USB_REG(kEp0r) = static_cast<uint16_t>(kEpRxValid | kEpTxNak);
    USB_REG(kEp1r) = static_cast<uint16_t>(kEpRxValid | kEpTxNak | 1u);
    ep_rx_count(0u, 64u);
    ep_rx_count(1u, 64u);
    ctrl_in_ = nullptr; ctrl_in_len_ = ctrl_in_pos_ = 0u; ctrl_in_active_ = false;
    ctrl_out_len_ = ctrl_pos_ = 0u; expect_out_ = false;
    bulk_out_len_ = bulk_out_pos_ = 0u;
    pending_address_ = 0u; address_pending_ = false;
}

void UsbFsG4Hal::handle_ep0() noexcept {
    uint16_t e = USB_REG(kEp0r);
    if ((e & kEpCtrRx) != 0u) {
        if ((e & kEpSetup) != 0u) {
            uint8_t b[8]{};
            pma_read(kEp0Rx, b, sizeof(b));
            embed::usb::Setup s{};
            s.type = b[0]; s.request = b[1];
            s.value = static_cast<uint16_t>(b[2] | (b[3] << 8));
            s.index = static_cast<uint16_t>(b[4] | (b[5] << 8));
            s.length = static_cast<uint16_t>(b[6] | (b[7] << 8));
            ctrl_out_len_ = 0u;
            ctrl_pos_ = 0u;
            pending_setup_ = s;
            expect_out_ = (s.length != 0u && !s.in());
            if (!expect_out_) {
                if (setup_cb_ != nullptr) setup_cb_(ctx_, s, nullptr, 0u);
                else stall_ep0();
            }
        } else {
            const uint16_t cnt = USB_PMA(6u) & 0x3FFu;
            const Size room = sizeof(ctrl_out_) - ctrl_out_len_;
            const Size n = (cnt > room) ? room : cnt;
            pma_read(kEp0Rx, ctrl_out_ + ctrl_out_len_, n);
            ctrl_out_len_ += n;
            if (expect_out_ &&
                (ctrl_out_len_ >= pending_setup_.length || cnt < 64u)) {
                expect_out_ = false;
                ctrl_pos_ = 0u;
                if (setup_cb_ != nullptr) {
                    setup_cb_(ctx_, pending_setup_, ctrl_out_, ctrl_out_len_);
                }
            }
        }
        e &= static_cast<uint16_t>(~kEpCtrRx);
        e |= kEpRxValid;
        USB_REG(kEp0r) = e;
    }
    if ((USB_REG(kEp0r) & kEpCtrTx) != 0u) {
        USB_REG(kEp0r) = static_cast<uint16_t>(kEpRxValid | kEpTxNak);
        if (ctrl_in_active_) {
            send_ep0_next();
        } else if (address_pending_) {
            USB_REG(kDaddr) = static_cast<uint16_t>(0x80u | pending_address_);
            address_pending_ = false;
        }
    }
}

void UsbFsG4Hal::handle_ep1() noexcept {
    uint16_t e = USB_REG(kEp1r);
    if ((e & kEpCtrRx) != 0u) {
        const uint16_t cnt = USB_PMA(14u) & 0x3FFu;
        const Size n = (cnt > sizeof(bulk_out_) - bulk_out_len_)
            ? sizeof(bulk_out_) - bulk_out_len_ : cnt;
        pma_read(kEp1Rx, bulk_out_ + bulk_out_len_, n);
        bulk_out_len_ += n;
        if (out_cb_ != nullptr) out_cb_(ctx_, 1u, bulk_out_ + bulk_out_pos_, bulk_out_len_ - bulk_out_pos_);
        bulk_out_pos_ = bulk_out_len_;
        e &= static_cast<uint16_t>(~kEpCtrRx);
        e |= kEpRxValid;
        USB_REG(kEp1r) = e;
    }
    if ((e & kEpCtrTx) != 0u) {
        e &= static_cast<uint16_t>(~kEpCtrTx);
        e = static_cast<uint16_t>((e & ~kEpStatTx) | kEpTxNak);
        USB_REG(kEp1r) = e;
    }
}

void UsbFsG4Hal::send_ep0_next() noexcept {
    if (!ctrl_in_active_) return;
    Size n = ctrl_in_len_ - ctrl_in_pos_;
    if (n > 64u) n = 64u;
    if (n != 0u) pma_write(kEp0Tx, ctrl_in_ + ctrl_in_pos_, n);
    ep_tx_count(0u, static_cast<uint16_t>(n));
    USB_REG(kEp0r) = static_cast<uint16_t>(kEpRxValid | kEpTxValid);
    ctrl_in_pos_ += n;
    if (ctrl_in_pos_ >= ctrl_in_len_) ctrl_in_active_ = false;
}

Status UsbFsG4Hal::ep0_write(const void* data, Size len) noexcept {
    if (!inited_) return Status::Unavailable;
    if (len != 0u && data == nullptr) return Status::InvalidArg;
    ctrl_in_ = static_cast<const uint8_t*>(data);
    ctrl_in_len_ = len; ctrl_in_pos_ = 0u; ctrl_in_active_ = true;
    send_ep0_next();
    return Status::Ok;
}

Status UsbFsG4Hal::ep0_read(void* data, Size cap, Size& got) noexcept {
    got = 0u;
    if (ctrl_pos_ >= ctrl_out_len_) return Status::Timeout;
    Size n = ctrl_out_len_ - ctrl_pos_;
    if (n > cap) n = cap;
    for (Size i = 0u; i < n; ++i) static_cast<uint8_t*>(data)[i] = ctrl_out_[ctrl_pos_ + i];
    ctrl_pos_ += n; got = n;
    return Status::Ok;
}

Status UsbFsG4Hal::ep_write(uint8_t ep, const void* data, Size len) noexcept {
    if (!inited_ || ep != 1u || len > 64u) return Status::InvalidArg;
    if (len != 0u) pma_write(kEp1Tx, data, len);
    ep_tx_count(1u, static_cast<uint16_t>(len));
    uint16_t e = USB_REG(kEp1r);
    e &= static_cast<uint16_t>(~(kEpStatTx | kEpCtrTx));
    e |= static_cast<uint16_t>(kEpTxValid | 1u | kEpRxValid);
    USB_REG(kEp1r) = e;
    return Status::Ok;
}

Status UsbFsG4Hal::ep_read(uint8_t ep, void* data, Size cap, Size& got) noexcept {
    got = 0u;
    if (ep != 1u) return Status::NotSupported;
    Size n = bulk_out_len_ - bulk_out_pos_;
    if (n == 0u) return Status::Timeout;
    if (n > cap) n = cap;
    for (Size i = 0u; i < n; ++i) static_cast<uint8_t*>(data)[i] = bulk_out_[bulk_out_pos_ + i];
    bulk_out_pos_ += n; got = n;
    return Status::Ok;
}

void UsbFsG4Hal::stall_ep0() noexcept {
    uint16_t e = USB_REG(kEp0r);
    e &= static_cast<uint16_t>(~(kEpStatTx | kEpStatRx));
    e |= static_cast<uint16_t>(kEpTxStall | kEpRxStall);
    USB_REG(kEp0r) = e;
}

Status UsbFsG4Hal::suspend() noexcept {
    if (!inited_) return Status::Unavailable;
    USB_REG(kCntr) |= (1u << 3);           // FSUSP
    return Status::Ok;
}
Status UsbFsG4Hal::resume() noexcept {
    if (!inited_) return Status::Unavailable;
    USB_REG(kCntr) &= static_cast<uint16_t>(~(1u << 3));  // clear FSUSP
    USB_REG(kCntr) |= (1u << 4);           // RESUME
    USB_REG(kCntr) &= static_cast<uint16_t>(~(1u << 4));
    return Status::Ok;
}

Status UsbFsG4Hal::set_address(uint8_t address) noexcept {
    if (!inited_) return Status::Unavailable;
    pending_address_ = static_cast<uint8_t>(address & 0x7Fu);
    address_pending_ = true;
    return Status::Ok;
}

} // namespace embed::stm32g4::usb

#endif
