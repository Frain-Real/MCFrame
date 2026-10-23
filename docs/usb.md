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

Для контроллера с setup-callback (F4 OTG) используйте `usb::Port` — он
сам передаёт OUT-данные в `Device` и делает STALL при неподдержанном запросе:

```cpp
usb::cdc::Acm acm;
usb::Port port(hal, acm);
hal.init(&usb::Port::on_setup, on_bulk_out, &port);
// в задаче/цикле: hal.poll();
```

`Device` обрабатывает GET_STATUS, SET_ADDRESS (адрес применяется после status
stage), SET_CONFIGURATION, GET/SET_INTERFACE, строковые дескрипторы и
обрезает IN-ответ по `wLength`. OTG HAL режет EP0 на 64-байтные пакеты,
поэтому configuration descriptor >64 байт уходит корректно. `acm.on_out(...)`
вызывается для bulk OUT; `acm.write(hal, ...)` отправляет bulk IN.

## ## RNDIS

[`rndis.hpp`](../src/os/usb/rndis.hpp) — USB Ethernet (RNDIS):

- control: INITIALIZE/QUERY/SET/RESET/KEEPALIVE, OID MAC, MTU, link,
  packet filter;
- data: Ethernet frames в RNDIS packet header (44 байта) через bulk
  OUT/IN;
- `FrameSink` + `send_frame`/`flush` — мост к lwIP netif.

```cpp
usb::rndis::Rndis rndis;
usb::Port port(hal, rndis);
hal.init(&usb::Port::on_setup, on_bulk_out, &port);
rndis.set_sink(on_eth_frame, &netif);
// при bulk OUT: rndis.receive(data, len);
// при TX: rndis.send_frame(frame, len); rndis.flush(hal);
```

Сам RNDIS — L2; [`rndis_factory.hpp`](../src/os/net/rndis_factory.hpp)
добавляет ARP/IPv4/UDP/TCP/DNS/DHCP и реализует полный `net::Factory`, поэтому
MQTT/HTTP могут работать через тот же `net::Socket`. DHCP/ARP/TCP на реальном
USB-хосте — HIL-задача.

## Composite

[`composite.hpp`](../src/os/usb/composite.hpp) склеивает configuration
descriptor нескольких классов, перенумеровывает `bInterfaceNumber` и
проверяет, что endpoint-адреса не пересекаются. CDC + DFU (DFU использует
только EP0) — рабочий пример:

```cpp
usb::cdc::Acm acm;
usb::dfu::Dfu dfu(&flash_target);
usb::Class* funcs[] = {&acm, &dfu};
usb::Composite<> composite(funcs, 2u);
usb::Port port(hal, composite);
hal.init(&usb::Port::on_setup, on_bulk_out, &port);
```

CDC+MSC потребует remap endpoint-ов: обычные классы используют фиксированные
адреса.

## CDC shell и OTA

[`cdc_transport.hpp`](../src/os/usb/cdc_transport.hpp):

```cpp
usb::cdc::Acm acm;
usb::cdc::AcmTransport ota_transport(acm, hal);
ota::Config oc{};
oc.transport = &ota_transport;       // протокол OTA по USB CDC

usb::cdc::AcmConsole shell_port(acm, hal);
shell::Console console;
shell_port.init(console);
// в задаче: otg.poll(); acm.on_out(...); shell_port.poll();
```

`AcmTransport::max_payload()` = 252 Б; приложение обязано сначала отдать
bulk OUT в `Acm::on_out()`, затем вызвать `poll()`.

Классы и endpoints

| класс | interface | endpoints |
|---|---|---|
| CDC-ACM | 0 (comm) + 1 (data) | 0x81 notify, 0x01 OUT, 0x82 IN |
| DFU | 0 | только EP0 |
| MSC | 0 | 0x01 OUT, 0x82 IN |

Дескрипторы уже в LE и включают configuration + interface + functional/
endpoint-дескрипторы. VID/PID задаются в `usb::DeviceDescriptor`.

## DFU

`Dfu` без target хранит байты в статическом буфере 4 КБ (стенд). Для реальной
прошивки используйте [`dfu_storage.hpp`](../src/os/usb/dfu_storage.hpp):

```cpp
usb::dfu::StorageTarget target(storage, slot_offset, slot_capacity);
usb::dfu::Dfu dfu(&target);
```

Первый DNLOAD вызывает `StorageTarget::begin()` (erase), данные пишутся по
смещению, нулевой DNLOAD завершает `MANIFEST` и вызывает `finish()`.
GETSTATUS/GETSTATE отражают `DnBusy → Manifest`; ABORT сбрасывает сессию.

## MSC

`msc::BlockDevice` — интерфейс чтения/записи блоков (обычно 512 Б). SCSI
обрабатывается в `handle_cbw()`; CSW уходит после данных. READ(10) и
WRITE(10) поддержаны; для WRITE приложение передаёт принятые bulk OUT данные:

```cpp
msc.handle_cbw(cbw, sizeof(cbw), hal, out_data, out_len);
```

Если `write_block()` возвращает ошибку, CSW получает failed. Реальный
bulk-OUT маршрут зависит от контроллера (OTG callback/PMA) и проверяется HIL.

## Адаптер STM32 OTG FS

[`usb_otg_fs.cpp`](../src/platforms/stm32f4g4/usb_otg_fs.cpp) реализует
`usb::Hal` для STM32F4 OTG FS (device mode, polled):

- PA11/PA12 AF10, OTG FS clock, soft reset, FIFO, EP0;
- SETUP → data → status для EP0; SET_ADDRESS применяется после status;
- IN-передача EP0 режется на 64-байтные пакеты, `wLength` соблюдается;
- OUT data stage для class requests буферизуется до полного `wLength`;
- `poll()` читает RX FIFO/IEPINT и вызывает callbacks;
- `ep0_write/read`, `ep_write`, `stall_ep0`, `set_address`.

Минимальный запуск:

```cpp
usb::cdc::Acm acm;
usb::Port port(otg, acm);
otg.init(&usb::Port::on_setup, on_bulk_out, &port);
// периодически: otg.poll();
```

G4 использует USB FS (не OTG): [`usb_fs_g4.cpp`](../src/platforms/stm32g4/usb_fs_g4.cpp)
реализует EP0 enumeration + EP1 bulk через PMA; `board::usb()` отдаёт
`UsbFsG4Hal`. Адаптер собран, но **не проверен HIL** — PMA/BCDR-настройки
обязательно проверить на G431.

F4/G4 enumeration (Windows/Linux, hot-plug, suspend, composite) остаётся
задачей HIL.

## Проверка

[`tests/test_usb.cpp`](../tests/test_usb.cpp) использует `MockHal`:

- GET_DESCRIPTOR device/config, SET_CONFIGURATION;
- CDC SET/GET_LINE_CODING, OUT→read, write;
- DFU DNLOAD/UPLOAD/GETSTATUS/GETSTATE/ABORT;
- MSC GET_MAX_LUN, READ(10), INQUIRY, CSW.
