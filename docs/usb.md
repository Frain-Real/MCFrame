# USB device: CDC-ACM, DFU, MSC

USB-слой разделён на контроллер и классы:

```text
usb::Hal       — EP0/EPx, STALL (платформенный адаптер)
usb::Device    — дескрипторы, GET_DESCRIPTOR, SET_CONFIGURATION
usb::cdc::Acm  — CDC-ACM: line coding, bulk IN/OUT, notifications
usb::dfu::Dfu  — DFU 1.1: DETACH/DNLOAD/UPLOAD/GETSTATUS/… 
usb::msc::Msc  — MSC BOT + SCSI: INQUIRY, READ CAPACITY, READ(10)
```

## Подключение класса

```cpp
usb::cdc::Acm acm;
usb::Device dev;
dev.init(hal, acm);              // hal — адаптер контроллера
```

Контроллер вызывает `dev.setup(s)` для каждого setup-пакета (данные OUT —
третьим/четвёртым аргументом), а `acm.on_out(...)` — для данных bulk OUT.
`acm.write(hal, ...)` отправляет bulk IN; `msc.handle_cbw(...)` — CBW.

## Классы и endpoints

| класс | interface | endpoints |
|---|---|---|
| CDC-ACM | 0 (comm) + 1 (data) | 0x81 notify, 0x01 OUT, 0x82 IN |
| DFU | 0 | только EP0 |
| MSC | 0 | 0x01 OUT, 0x82 IN |

Дескрипторы уже в LE и включают configuration + interface + functional/
endpoint-дескрипторы. VID/PID задаются в `usb::DeviceDescriptor`.

## DFU

`Dfu` хранит принятые байты в статическом буфере (4 КБ в примере) и отдаёт
состояние через GETSTATUS/GETSTATE. Для реальной прошивки замените буфер на
`ota::Storage`/flash-драйвер: состояния `DnloadSync → DnBusy → Manifest`
должны соответствовать стиранию/записи.

## MSC

`msc::BlockDevice` — интерфейс чтения/записи блоков (обычно 512 Б). SCSI
обрабатывается в `handle_cbw()`; CSW уходит после данных. WRITE(10) в примере
возвращает `NotSupported`, чтобы не притворяться рабочим хранилищем.

## Адаптер STM32 OTG FS

[`usb_otg_fs.cpp`](../src/platforms/stm32f4g4/usb_otg_fs.cpp) реализует
`usb::Hal` для STM32F4 OTG FS (device mode, polled):

- PA11/PA12 AF10, OTG FS clock, soft reset, FIFO, EP0;
- `poll()` читает SETUP/OUT из RX FIFO и вызывает callbacks;
- `ep0_write/read`, `ep_write`, `stall_ep0`;
- setup callback приложения передаёт пакет в `usb::Device::setup()`.

Это начальный адаптер: G4 использует USB FS (не OTG) и требует отдельного
адаптера; на STM32F4 классы CDC/DFU/MSC уже можно подключить через
`OtgFsHal::init(on_setup, on_out, ctx)`. Полный status/endpoint lifecycle
нужно проверить на HIL.

## Проверка

[`tests/test_usb.cpp`](../tests/test_usb.cpp) использует `MockHal`:

- GET_DESCRIPTOR device/config, SET_CONFIGURATION;
- CDC SET/GET_LINE_CODING, OUT→read, write;
- DFU DNLOAD/UPLOAD/GETSTATUS/GETSTATE/ABORT;
- MSC GET_MAX_LUN, READ(10), INQUIRY, CSW.
