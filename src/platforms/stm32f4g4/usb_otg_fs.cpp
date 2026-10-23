// platforms/stm32f4g4/usb_otg_fs.cpp
#include "usb_otg_fs.hpp"

#if defined(MCFRAME_SOC_STM32F4)

#include "stm32m4_min.h"

namespace embed::stm32f4g4::usb {
namespace {

constexpr uint32_t kOtgBase = 0x50000000UL;
#define OTG_REG(off) (*reinterpret_cast<volatile uint32_t*>((kOtgBase + (off))))
#define OTG_FIFO(n)  (*reinterpret_cast<volatile uint32_t*>((kOtgBase + 0x1000u + (n) * 0x1000u)))

constexpr uint32_t kGahbcfg   = 0x008u;
constexpr uint32_t kGusbcfg   = 0x00Cu;
constexpr uint32_t kGrstctl   = 0x010u;
constexpr uint32_t kGintsts   = 0x014u;
constexpr uint32_t kGintmsk   = 0x018u;
constexpr uint32_t kGrxstsp   = 0x028u;
constexpr uint32_t kGccfg     = 0x038u;
constexpr uint32_t kDcfg      = 0x800u;
constexpr uint32_t kDctl      = 0x804u;
constexpr uint32_t kDiepmsk   = 0x810u;
constexpr uint32_t kDoepmsk   = 0x814u;
constexpr uint32_t kDaintmsk  = 0x81Cu;
constexpr uint32_t kDiepctl0  = 0x900u;
constexpr uint32_t kDiepint0  = 0x908u;
constexpr uint32_t kDieptsiz0 = 0x910u;
constexpr uint32_t kDoepctl0  = 0xB00u;
constexpr uint32_t kDoeptsiz0 = 0xB10u;
constexpr uint32_t kDiepctl1  = 0x920u;
constexpr uint32_t kDieptsiz1 = 0x930u;
constexpr uint32_t kDoepctl1  = 0xB20u;
constexpr uint32_t kDoeptsiz1 = 0xB30u;

constexpr uint32_t kGintRxFlvl = 1u << 4;
constexpr uint32_t kGintUsbRst = 1u << 12;
constexpr uint32_t kGintEnumDne = 1u << 13;
constexpr uint32_t kGintIepInt = 1u << 18;
constexpr uint32_t kGintOepInt = 1u << 19;
constexpr uint32_t kDctlSdis   = 1u << 1;
constexpr uint32_t kDiepEpena  = 1u << 31;
constexpr uint32_t kDiepCnak   = 1u << 26;
constexpr uint32_t kDiepStall  = 1u << 21;
constexpr uint32_t kDiepSnak   = 1u << 27;
constexpr uint32_t kEptypeCtl  = 0u << 18;
constexpr uint32_t kEptypeBulk = 2u << 18;
constexpr uint32_t kMpsiz64    = 64u;
constexpr uint32_t kEp0Packet  = 64u;
constexpr uint32_t kDiepintXfrc = 1u << 0;

bool wait_ahb_idle() noexcept {
    uint32_t guard = 100000u;
    while ((OTG_REG(kGrstctl) & (1u << 31)) == 0u) {
        if (--guard == 0u) return false;
    }
    return true;
}

void fifo_flush(uint32_t fifo) noexcept {
    OTG_REG(kGrstctl) |= (1u << 5) | ((fifo & 0x1Fu) << 6);
    uint32_t guard = 100000u;
    while ((OTG_REG(kGrstctl) & (1u << 5)) != 0u && --guard != 0u) { }
}

void write_fifo(uint32_t ep, const uint8_t* data, Size len) noexcept {
    const Size words = (len + 3u) / 4u;
    for (Size i = 0u; i < words; ++i) {
        uint32_t v = 0u;
        for (Size b = 0u; b < 4u; ++b) {
            const Size idx = i * 4u + b;
            if (idx < len) v |= static_cast<uint32_t>(data[idx]) << (8u * b);
        }
        OTG_FIFO(ep) = v;
    }
}

Size read_fifo(uint32_t ep, uint8_t* data, Size len) noexcept {
    const Size words = (len + 3u) / 4u;
    Size done = 0u;
    for (Size i = 0u; i < words; ++i) {
        const uint32_t v = OTG_FIFO(ep);
        for (Size b = 0u; b < 4u && done < len; ++b) {
            data[done++] = static_cast<uint8_t>(v >> (8u * b));
        }
    }
    return done;
}

} // namespace

Status OtgFsHal::init(SetupCallback on_setup, OutCallback on_out, void* ctx) noexcept {
    setup_cb_ = on_setup;
    out_cb_ = on_out;
    ctx_ = ctx;
    ctrl_out_len_ = 0u;
    ctrl_pos_ = 0u;
    ctrl_in_data_ = nullptr;
    ctrl_in_len_ = 0u;
    ctrl_in_pos_ = 0u;
    ctrl_in_active_ = false;
    expect_out_ = false;
    pending_address_ = 0u;
    address_pending_ = false;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->AHB2ENR |= (1u << 7);       // OTGFSEN (F4, AHB2)

    // PA11/PA12 — AF10 (OTG FS DM/DP).
    GPIOA->MODER = (GPIOA->MODER & ~((3u << 22) | (3u << 24))) |
                   ((2u << 22) | (2u << 24));
    GPIOA->OSPEEDR |= (3u << 22) | (3u << 24);
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFu << 12) | (0xFu << 16))) |
                    ((10u << 12) | (10u << 16));

    OTG_REG(kGrstctl) |= 1u;
    if (!wait_ahb_idle()) return Status::HardwareError;

    OTG_REG(kGusbcfg) |= (1u << 30);          // FDMOD: device mode
    OTG_REG(kGusbcfg) = (OTG_REG(kGusbcfg) & ~(0xFu << 10)) | (9u << 10);
    OTG_REG(kGccfg) &= ~(1u << 16);           // PWRDWN = 0
    OTG_REG(kGahbcfg) |= 1u;                  // GINTMSK
    OTG_REG(kDcfg) = 3u;                      // full speed
    OTG_REG(kDctl) |= kDctlSdis;              // soft disconnect
    OTG_REG(kDiepctl0) = kMpsiz64 | kEptypeCtl;
    OTG_REG(kDoepctl0) = kMpsiz64 | kEptypeCtl;
    // EP1 — bulk IN/OUT для CDC/MSC (64 байта, как у FS).
    OTG_REG(kDiepctl1) = kMpsiz64 | kEptypeBulk;
    OTG_REG(kDoepctl1) = kMpsiz64 | kEptypeBulk;
    OTG_REG(kDaintmsk) = 0xFFFFFFFFu;
    OTG_REG(kDiepmsk) = 0xFFFFFFFFu;
    OTG_REG(kDoepmsk) = 0xFFFFFFFFu;
    OTG_REG(kGintmsk) = kGintRxFlvl | kGintUsbRst | kGintEnumDne |
                        kGintIepInt | kGintOepInt;
    fifo_flush(0u);

    OTG_REG(kDctl) &= ~kDctlSdis;             // подключиться к шине
    inited_ = true;
    return Status::Ok;
}

void OtgFsHal::poll() noexcept {
    if (!inited_) return;
    uint32_t st = OTG_REG(kGintsts);
    if ((st & kGintUsbRst) != 0u) {
        OTG_REG(kGintsts) = kGintUsbRst;
        handle_reset();
    }
    if ((st & kGintEnumDne) != 0u) {
        OTG_REG(kGintsts) = kGintEnumDne;
    }
    if ((st & kGintRxFlvl) != 0u) handle_rx();
    if ((st & kGintIepInt) != 0u) {
        if ((OTG_REG(kDiepint0) & kDiepintXfrc) != 0u) handle_ep0_in_done();
        OTG_REG(kGintsts) = kGintIepInt;
    }
    if ((st & kGintOepInt) != 0u) OTG_REG(kGintsts) = kGintOepInt;
}

void OtgFsHal::handle_reset() noexcept {
    OTG_REG(kDcfg) &= ~0x7Fu;                 // address 0
    OTG_REG(kDiepctl0) = kMpsiz64 | kEptypeCtl;
    OTG_REG(kDoepctl0) = kMpsiz64 | kEptypeCtl;
    ctrl_in_data_ = nullptr;
    ctrl_in_len_ = 0u;
    ctrl_in_pos_ = 0u;
    ctrl_in_active_ = false;
    ctrl_out_len_ = 0u;
    ctrl_pos_ = 0u;
    expect_out_ = false;
    pending_address_ = 0u;
    address_pending_ = false;
    pending_setup_ = embed::usb::Setup{};
    fifo_flush(0u);
}

void OtgFsHal::handle_rx() noexcept {
    const uint32_t pkt = OTG_REG(kGrxstsp);
    const uint8_t ep = static_cast<uint8_t>(pkt & 0x0Fu);
    const Size len = (pkt >> 4) & 0x7FFu;
    const uint8_t pktsts = static_cast<uint8_t>((pkt >> 17) & 0x0Fu);

    if (ep == 0u && pktsts == 6u && len >= 8u) {   // SETUP
        embed::usb::Setup s{};
        uint8_t b[8]{};
        (void)read_fifo(0u, b, sizeof(b));
        s.type = b[0];
        s.request = b[1];
        s.value = static_cast<uint16_t>(b[2] | (b[3] << 8));
        s.index = static_cast<uint16_t>(b[4] | (b[5] << 8));
        s.length = static_cast<uint16_t>(b[6] | (b[7] << 8));

        pending_setup_ = s;
        ctrl_out_len_ = 0u;
        ctrl_pos_ = 0u;
        expect_out_ = false;

        if (s.length != 0u && !s.in()) {
            if (s.length > sizeof(ctrl_out_)) {
                stall_ep0();
                return;
            }
            expect_out_ = true;
            // Принимаем data stage. Для длины <=64 хватает одного пакета;
            // при большей длине OTG FS всё равно режет на 64-байтные пакеты.
            const uint32_t packets = (s.length + kEp0Packet - 1u) / kEp0Packet;
            OTG_REG(kDoeptsiz0) = (packets << 19) | s.length;
            OTG_REG(kDoepctl0) |= kDiepCnak;
            return;
        }

        if (setup_cb_ != nullptr) setup_cb_(ctx_, s, nullptr, 0u);
        else stall_ep0();
        return;
    }

    if (ep == 0u && (pktsts == 2u || pktsts == 3u)) {
        if (expect_out_) {
            const Size room = sizeof(ctrl_out_) - ctrl_out_len_;
            const Size n = (len > room) ? room : len;
            ctrl_out_len_ += read_fifo(0u, ctrl_out_ + ctrl_out_len_, n);
            if (ctrl_out_len_ >= pending_setup_.length || len < kEp0Packet) {
                expect_out_ = false;
                ctrl_pos_ = 0u;
                if (setup_cb_ != nullptr) {
                    setup_cb_(ctx_, pending_setup_, ctrl_out_, ctrl_out_len_);
                }
            }
        } else {
            // Status OUT после IN data stage (ZLP) — просто поглотить.
            uint8_t tmp[8]{};
            (void)read_fifo(0u, tmp, len < sizeof(tmp) ? len : sizeof(tmp));
        }
        return;
    }

    if (ep != 0u && pktsts == 2u) {
        uint8_t tmp[64]{};
        const Size n = (len > sizeof(tmp)) ? sizeof(tmp) : len;
        const Size got = read_fifo(ep, tmp, n);
        if (out_cb_ != nullptr) out_cb_(ctx_, ep, tmp, got);
    }
}

void OtgFsHal::send_ep0_next() noexcept {
    if (!ctrl_in_active_) return;
    if (ctrl_in_pos_ >= ctrl_in_len_) {
        // ZLP/status IN: одна пустая передача.
        OTG_REG(kDieptsiz0) = 1u << 19;
        OTG_REG(kDiepctl0) |= kDiepEpena | kDiepCnak;
        ctrl_in_active_ = false;
        return;
    }
    Size chunk = ctrl_in_len_ - ctrl_in_pos_;
    if (chunk > kEp0Packet) chunk = kEp0Packet;
    write_fifo(0u, ctrl_in_data_ + ctrl_in_pos_, chunk);
    OTG_REG(kDieptsiz0) = chunk | (1u << 19);
    OTG_REG(kDiepctl0) |= kDiepEpena | kDiepCnak;
    ctrl_in_pos_ += chunk;
    if (ctrl_in_pos_ >= ctrl_in_len_) ctrl_in_active_ = false;
}

void OtgFsHal::handle_ep0_in_done() noexcept {
    OTG_REG(kDiepint0) = kDiepintXfrc;
    if (ctrl_in_active_) {
        send_ep0_next();
        return;
    }
    // SET_ADDRESS применяется после завершения status stage, иначе хост
    // отправит status на старый адрес и enumeration сорвётся.
    if (address_pending_ && !ctrl_in_active_) {
        OTG_REG(kDcfg) = (OTG_REG(kDcfg) & ~0x7Fu) | (pending_address_ & 0x7Fu);
        address_pending_ = false;
    }
}

Status OtgFsHal::ep0_write(const void* data, Size len) noexcept {
    if (!inited_) return Status::Unavailable;
    if (len != 0u && data == nullptr) return Status::InvalidArg;
    ctrl_in_data_ = static_cast<const uint8_t*>(data);
    ctrl_in_len_ = len;
    ctrl_in_pos_ = 0u;
    ctrl_in_active_ = true;
    send_ep0_next();
    return Status::Ok;
}

Status OtgFsHal::ep0_read(void* data, Size cap, Size& got) noexcept {
    got = 0u;
    if (ctrl_pos_ >= ctrl_out_len_) return Status::Timeout;
    Size n = ctrl_out_len_ - ctrl_pos_;
    if (n > cap) n = cap;
    auto* out = static_cast<uint8_t*>(data);
    for (Size i = 0u; i < n; ++i) out[i] = ctrl_out_[ctrl_pos_ + i];
    ctrl_pos_ += n;
    got = n;
    return Status::Ok;
}

Status OtgFsHal::ep_write(uint8_t ep, const void* data, Size len) noexcept {
    if (!inited_ || ep == 0u || ep > 3u) return Status::InvalidArg;
    const auto* p = static_cast<const uint8_t*>(data);
    if (p != nullptr) write_fifo(ep, p, len);
    if (ep == 1u) {
        OTG_REG(kDiepctl1) = kMpsiz64 | kEptypeBulk;
        OTG_REG(kDieptsiz1) = static_cast<uint32_t>(len & 0x7FFu) | (1u << 19);
        OTG_REG(kDiepctl1) |= kDiepEpena | kDiepCnak;
    }
    return Status::Ok;
}

Status OtgFsHal::ep_read(uint8_t, void*, Size, Size& got) noexcept {
    got = 0u;
    return Status::NotSupported;
}

void OtgFsHal::stall_ep0() noexcept {
    OTG_REG(kDiepctl0) |= kDiepStall | kDiepSnak;
    OTG_REG(kDoepctl0) |= (1u << 21);
}

Status OtgFsHal::set_address(uint8_t address) noexcept {
    if (!inited_) return Status::Unavailable;
    pending_address_ = address & 0x7Fu;
    address_pending_ = true;
    return Status::Ok;
}

} // namespace embed::stm32f4g4::usb

#endif // MCFRAME_SOC_STM32F4
