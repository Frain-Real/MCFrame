# HIL: что проверять на реальной плате

CI проверяет сборку, host-модели и парсеры; HIL требует железа.

## W5500

Подключение: SPI1, CS, INT (активный низкий), питание 3.3 В, Ethernet-кабель.

1. `wait_link(true, 2000)` — PHYCFGR.LNK.
2. `W5500Config.use_dhcp=true; init()` — DHCP DISCOVER/OFFER/REQUEST/ACK;
   проверить IP/GW/mask/DNS.
3. `resolve("example.com")` — A-запрос к `cfg.dns`; `resolve("1.1.1.1")` —
   литерал.
4. TCP/HTTP GET, затем выдернуть кабель: `link_up=false`, IRQ не должен
   блокировать задачу; после возврата кабеля — reconnect.
5. Снять осциллографом INT при RX/TX и проверить, что `irq_pending()` видит
   фронт.

## STM32F4 USB OTG FS / G4 USB FS

1. Подключить к Linux/Windows; проверить device/config/string descriptors.
2. SET_ADDRESS, SET_CONFIGURATION, GET/SET_INTERFACE.
3. CDC: line coding, bulk echo, shell; OTA-кадры через `AcmTransport`.
4. DFU: DNLOAD в слот, MANIFEST, GETSTATUS/GETSTATE, UPLOAD.
5. MSC: READ(10)/WRITE(10), CSW, hot-plug.
6. Composite CDC+DFU: оба интерфейса, class requests по `wIndex`.
7. G4: проверить PMA/BCDR и EP0 enumeration на G431 — адаптер собран, но
   HIL ещё не проводился.

## C3 TLS

1. `board::tls()` с CA; HTTPS GET/MQTT;
2. неверный CA/hostname должен отклоняться;
3. reconnect после обрыва Wi-Fi.

## USB RNDIS

1. Windows/Linux видит RNDIS/Remote NDIS device, получает MAC/MTU.
2. Host DHCP выдаёт IP; проверить ICMP/ARP и Ethernet frames.
3. Проверить `INITIALIZE/QUERY/SET/RESET`, packet filter.
4. Поднять lwIP netif и пройти HTTP GET; без IP-стека MQTT/HTTP не работают.

## BLE GATT client

1. Подключиться к peripheral, discover services/chars.
2. Read/write по handle, проверить ошибки/таймауты.
3. Notify/indicate (CCCD) пока не обёрнут — HIL/доработка.

## SD/littlefs

1. `Card::init()` на реальной SDHC, READ/WRITE/CSD capacity.
2. littlefs mount/format/read/write, power-fail.
3. Одновременная работа SD и W5500 на SPI1 — проверить CS/lock (сейчас
   общий CS-пин; нужен отдельный CS для второй SPI-периферии).
