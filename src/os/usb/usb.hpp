// os/usb/usb.hpp
// ---------------------------------------------------------------------------
// USB device: ядро стандартных запросов + классы CDC-ACM, DFU и MSC.
//
// Ядро не знает регистров USB-контроллера. Оно работает через usb::Hal:
// передать данные EP0/EPx, прочитать, выдать STALL. Поэтому один и тот же
// код классов проверяется на хосте и переносится на STM32/ESP через адаптер.
//
// Уровни:
//   Hal    — контроллер (STM32 OTG FS, mock);
//   Device — дескрипторы, GET_DESCRIPTOR/SET_CONFIGURATION, class requests;
//   Acm/Dfu/Msc — протоколы классов.
// ---------------------------------------------------------------------------
#pragma once

#include "../common.hpp"

namespace embed::usb {

struct Setup {
    uint8_t  type    = 0u;   ///< bmRequestType
    uint8_t  request = 0u;   ///< bRequest
    uint16_t value   = 0u;
    uint16_t index   = 0u;
    uint16_t length  = 0u;

    bool in() const noexcept { return (type & 0x80u) != 0u; }
    bool standard() const noexcept { return (type & 0x60u) == 0u; }
    bool class_request() const noexcept { return (type & 0x60u) == 0x20u; }
};

enum class Speed : uint8_t { Full = 0u, High = 1u };

struct DeviceDescriptor {
    uint8_t  bLength = 18u;
    uint8_t  bDescriptorType = 1u;
    uint16_t bcdUSB = 0x0200u;
    uint8_t  bDeviceClass = 0u;
    uint8_t  bDeviceSubClass = 0u;
    uint8_t  bDeviceProtocol = 0u;
    uint8_t  bMaxPacketSize0 = 64u;
    uint16_t idVendor = 0x1209u;
    uint16_t idProduct = 0x0001u;
    uint16_t bcdDevice = 0x0100u;
    uint8_t  iManufacturer = 1u;
    uint8_t  iProduct = 2u;
    uint8_t  iSerialNumber = 0u;
    uint8_t  bNumConfigurations = 1u;
};
static_assert(sizeof(DeviceDescriptor) == 18u, "USB device descriptor");

/// Контроллер. Все методы могут вернуть NotSupported, если endpoint нет.
class Hal {
public:
    virtual ~Hal() = default;
    virtual Status ep0_write(const void* data, Size len) noexcept = 0;
    virtual Status ep0_read(void* data, Size cap, Size& got) noexcept = 0;
    virtual Status ep_write(uint8_t ep, const void* data, Size len) noexcept = 0;
    virtual Status ep_read(uint8_t ep, void* data, Size cap, Size& got) noexcept = 0;
    virtual void stall_ep0() noexcept = 0;
};

class Device;

/// Класс-функция USB. config_descriptor() возвращает ВЕСЬ configuration
/// descriptor (config + interface + functional + endpoints), уже в LE.
class Class {
public:
    virtual ~Class() = default;
    virtual const uint8_t* config_descriptor(Size& len) const noexcept = 0;
    virtual Status init(Device* dev) noexcept { (void)dev; return Status::Ok; }

    /// Обработать class-specific запрос. out/out_len — данные OUT-стадии
    /// (могут быть nullptr). in/in_len — ответ для IN-стадии.
    virtual Status control(const Setup& s, const uint8_t* out, Size out_len,
                           const uint8_t*& in, Size& in_len) noexcept = 0;

    virtual void configured() noexcept {}
    virtual void set_interface(uint16_t) noexcept {}
};

class Device {
public:
    Device() noexcept = default;

    Status init(Hal& hal, Class& cls, const DeviceDescriptor& dd = DeviceDescriptor{}) noexcept {
        hal_ = &hal;
        cls_ = &cls;
        dd_ = dd;
        configured_ = false;
        address_ = 0u;
        return cls_->init(this);
    }

    Hal& hal() noexcept { return *hal_; }
    bool configured() const noexcept { return configured_; }
    uint8_t address() const noexcept { return address_; }

    /// Обработать setup-пакет. Для IN-запроса ответ уже отправлен через
    /// Hal::ep0_write и доступен в last_response().
    Status setup(const Setup& s, const uint8_t* out = nullptr, Size out_len = 0u) noexcept {
        if (hal_ == nullptr || cls_ == nullptr) return Status::Unavailable;

        if (s.standard()) {
            switch (s.request) {
                case 0x00: return Status::Ok;                 // GET_STATUS (0)
                case 0x01: return Status::Ok;                 // CLEAR_FEATURE
                case 0x03: return Status::Ok;                 // SET_FEATURE
                case 0x05:                                    // SET_ADDRESS
                    address_ = static_cast<uint8_t>(s.value & 0x7Fu);
                    return Status::Ok;
                case 0x06: return get_descriptor(s);
                case 0x08: return get_configuration();
                case 0x09:                                    // SET_CONFIGURATION
                    configured_ = (s.value != 0u);
                    if (configured_) cls_->configured();
                    return Status::Ok;
                case 0x0A: return Status::Ok;                 // GET_INTERFACE (0)
                case 0x0B:
                    cls_->set_interface(s.value);
                    return Status::Ok;
                default: break;
            }
        }
        if (s.class_request()) {
            const uint8_t* in = nullptr;
            Size in_len = 0u;
            const Status st = cls_->control(s, out, out_len, in, in_len);
            if (st != Status::Ok) return st;
            if (s.in() && in != nullptr && in_len > 0u) {
                return ep0_reply(in, in_len);
            }
            return Status::Ok;
        }
        return Status::NotSupported;
    }

    Status ep0_reply(const void* data, Size len) noexcept {
        last_len_ = (len > sizeof(last_)) ? sizeof(last_) : len;
        for (Size i = 0u; i < last_len_; ++i) last_[i] = static_cast<const uint8_t*>(data)[i];
        last_valid_ = true;
        return hal_->ep0_write(data, len);
    }

    const uint8_t* last_response() const noexcept { return last_valid_ ? last_ : nullptr; }
    Size last_len() const noexcept { return last_len_; }
    void clear_response() noexcept { last_valid_ = false; last_len_ = 0u; }

private:
    Status get_descriptor(const Setup& s) noexcept {
        const uint8_t type = static_cast<uint8_t>(s.value >> 8);
        if (type == 1u) {                                   // DEVICE
            return ep0_reply(&dd_, sizeof(dd_));
        }
        if (type == 2u) {                                   // CONFIGURATION
            Size n = 0u;
            const uint8_t* d = cls_->config_descriptor(n);
            return (d == nullptr) ? Status::Unavailable : ep0_reply(d, n);
        }
        if (type == 3u) {                                   // STRING
            static const uint8_t lang[4] = {4u, 3u, 0x09u, 0x04u};
            static const uint8_t prod[] = {24u, 3u, 'M',0,'C',0,'F',0,'r',0,'a',0,'m',0,'e',0,' ',0,'U',0,'S',0,'B',0};
            if ((s.value & 0xFFu) == 0u) return ep0_reply(lang, sizeof(lang));
            if ((s.value & 0xFFu) == 1u) return ep0_reply(prod, sizeof(prod));
            return Status::NotFound;
        }
        return Status::NotSupported;
    }

    Status get_configuration() noexcept {
        const uint8_t cfg = configured_ ? 1u : 0u;
        return ep0_reply(&cfg, 1u);
    }

    Hal*   hal_ = nullptr;
    Class* cls_ = nullptr;
    DeviceDescriptor dd_{};
    bool    configured_ = false;
    uint8_t address_ = 0u;
    uint8_t last_[256]{};
    Size    last_len_ = 0u;
    bool    last_valid_ = false;
};

} // namespace embed::usb
