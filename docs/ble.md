# BLE GATT client

[`IBle`](../src/hal/IBle.hpp) кроме peripheral/observer/central даёт
GATT-клиент:

```cpp
auto& ble = board::ble();
BleGattServiceCallback on_svc = ...;
ble.discover_services(conn, on_svc, ctx);

ble.discover_chars(conn, svc.start_handle, svc.end_handle, on_chr, ctx);
ble.read_char(conn, chr.handle, on_read, ctx);
ble.write_char(conn, chr.handle, data, len, true, on_write, ctx);
ble.subscribe_notify(conn, chr.handle, true, on_notify, ctx);
```

На C3 [`ble_esp.cpp`](../src/boards/esp32c3/ble_esp.cpp) реализует это через
NimBLE `ble_gattc_disc_all_svcs/disc_all_chrs/read/write_flat`.

CCCD/notify:

- `subscribe_notify()` ищет дескриптор `0x2902` через
  `ble_gattc_disc_all_dscs` и пишет `0x0001`/`0x0000`;
- входящие `BLE_GAP_EVENT_NOTIFY_RX` приходят в `BleGattNotifyCallback`.

Ограничения:

- discovery сервисов/характеристик/CCCD асинхронный; колбэки приходят из
  NimBLE task;
- диапазон поиска CCCD — `char_handle .. char_handle+2` (стандартное
  расположение); для нестандартных профилей нужен end handle;
- HIL на реальном peripheral не проводился.
