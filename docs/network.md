# Сеть: сокеты, MQTT, HTTP, SNTP

Сетевой слой не зависит от lwIP: протоколы работают через
[`net::Socket`](../src/os/net/socket.hpp), а платформа даёт фабрику сокетов.

## Сокеты

```cpp
net::Factory& f = board::sockets();      // ESP32-C3: lwIP backend
net::Socket* s = nullptr;
f.create(net::Protocol::Tcp, s);

net::Endpoint ep{};
ep.port = 1883;
f.resolve("broker.local", ep);
s->connect(ep, 5000);
s->send(data, len, sent);
s->recv(buf, sizeof(buf), got, 1000);
s->close();
```

`recv()` обязан вернуть `Timeout` по таймауту и `Ok` с `got == 0` при закрытии.
TCP использует `connect/send/recv`; UDP — `send_to/recv_from`.

## MQTT

[`mqtt::Client`](../src/os/net/mqtt.hpp) — MQTT 3.1.1, QoS 0/1/2,
keepalive, reconnect, Last Will, subscribe, входящие PUBLISH:

```cpp
mqtt::Client<768> mq;
mqtt::Config cfg{};
cfg.host = "broker.local";
cfg.client_id = "sensor-1";
mq.init(board::sockets(), cfg);
mq.connect();
cfg.will_topic = "status/sensor-1";
cfg.will_payload = "offline";
cfg.will_len = 7u;
cfg.will_qos = 1u;
mq.subscribe("cmd/#", 2u);
mq.publish("telemetry/temp", "21.5", 4, 2u);   // QoS2: PUBREC/PUBREL/PUBCOMP
mq.set_callback(on_message, this);
for (;;) { (void)mq.poll(1000); }
```

`poll()` сам отправляет PINGREQ по keepalive, принимает PINGRESP/PUBLISH/
PUBACK/PUBREC/PUBREL/PUBCOMP и при обрыве делает reconnect с backoff.
Входящий QoS2 отдаётся приложению только после PUBREL (exactly-once), а не
дважды на PUBLISH. Буфер статический, размер задаётся шаблоном.

## HTTP

[`http::Client`](../src/os/net/http.hpp) — HTTP/1.1 GET/POST, Content-Length
и chunked, тело через sink:

```cpp
http::Response r{};
http::Client<512> http;
http.get(board::sockets(), "http://example.com/data", r, sink, this);
```

HTTPS идёт через TLS-фабрику; по умолчанию `request()` для `https://`
возвращает `NotSupported` (защита от случайного plaintext на 443). Включение:

```cpp
http::Client<512> https;
https.enable_tls();
https.get(board::tls(), "https://example.com/", r, sink, this);
```

## TLS

[`tls.hpp`](../src/os/net/tls.hpp) — `TlsFactoryT<N>` поверх обычного
`Factory`: DNS/raw TCP остаются прежними, а `TlsSession` добавляет handshake,
send/recv и close. Провайдер криптографии подключается отдельно.
[`tls_mbedtls.hpp`](../src/os/net/tls_mbedtls.hpp) — готовый mbedTLS-провайдер
(TLS 1.2/1.3, X.509, SNI, проверка hostname); на C3 он подключён к
`board::tls()`, на STM32 нужен mbedTLS в проекте.

```cpp
net::TlsConfig tcfg{};
tcfg.ca = ca_der;           // nullptr + verify=false — только для отладки
tcfg.ca_len = ca_len;
tcfg.verify = true;
board::tls().set_config(tcfg);

mqtt::Client<768> mq;
mq.init(board::tls(), cfg);  // тот же MQTT, но поверх TLS
```

## SNTP

```cpp
sntp::Result r{};
sntp::sync(board::sockets(), "pool.ntp.org", 2000u, r);
// OsTimer и RTC уже синхронизированы
```

`query()` только возвращает время; `sync()` дополнительно вызывает
`OsTimer::SyncTime()` и `WallClock::persist()`.

## ESP32-C3 backend

[`net_socket_esp.cpp`](../src/boards/esp32c3/net_socket_esp.cpp) реализует
`net::Factory` поверх lwIP BSD sockets (`socket/connect/select/send/recv`) с
таймаутами через `select`. Пул — 4 статических сокета; одновременно можно
держать MQTT, HTTP и SNTP.

## STM32: W5500 по SPI

Для плат без Ethernet PHY есть [`net::W5500Factory`](../src/os/net/w5500.hpp):
W5500 держит TCP/UDP-сокеты в железе, поэтому MQTT/HTTP/SNTP работают поверх
`net::Socket` без lwIP.

```cpp
net::W5500FactoryT<4> net{board::spi1(), board::spi_cs()};

net::W5500Config cfg{};              // MAC, IP, GW, mask
cfg.ip[3] = 50;
(void)net.init(cfg);

// Дальше — как на C3:
mqtt::Client<768> client;
client.init(net, mqtt_cfg);
```

Особенности:

- `resolve()` принимает IPv4-литерал или A-запрос через `cfg.dns`
  (W5500 сам DNS не умеет — запрос идёт UDP-сокетом);
- `cfg.use_dhcp = true` запускает DHCP DISCOVER/REQUEST на порту 68 и
  записывает IP/GW/mask/DNS в W5500 и `cfg`;
- `link_up()`/`wait_link()` читают PHYCFGR; `irq_pending()` использует
  активный-низкий INT-пин (если передан) или `Sn_IR`;
- CS/SPI/reset/INT — из платы;
- сокеты 0..5 — приложению, 6 — DNS, 7 — DHCP; слотов до 6;
- драйвер не привязан к STM32 и тестируется на модели W5500
  (`tests/test_w5500.cpp`, 21 проверка: SPI, link, IRQ, DHCP/DNS parsers,
  TCP listen/accept).

### W5500 HIL

HIL-проверка требует платы с W5500 и реального Ethernet-кабеля; в CI её нет.
Минимальный сценарий:

1. `link_up()` до `true`, затем `wait_link(true, 2000)`;
2. `cfg.use_dhcp=true`, `init()`; проверить выданные IP/GW/DNS;
3. `resolve("example.com")` (DNS) и `resolve("1.1.1.1")`;
4. TCP connect/HTTP GET, затем повтор после выдёргивания кабеля: link=false,
   IRQ/ошибки не должны вешать задачу;
5. проверить INT-пин мультиметром/осциллографом при RX.

Результаты HIL пока не зафиксированы; это ограничение, а не «проверено».

## USB RNDIS

[`rndis.hpp`](../src/os/usb/rndis.hpp) — RNDIS device (CDC control + data):

- `INITIALIZE/QUERY/SET/RESET/KEEPALIVE`, OID MAC/link/MTU/filter;
- Ethernet frames через bulk OUT/IN (`receive`, `send_frame`, `flush`);
- `FrameSink` для передачи кадров в lwIP netif.

[`rndis_factory.hpp`](../src/os/net/rndis_factory.hpp) добавляет компактный
IPv4-стек и реализует **полный `net::Factory`**:

- ARP, IPv4, UDP, TCP client, DNS A;
- `create(Tcp/Udp)`, `connect/send/recv/send_to/recv_from`;
- `dhcp()` (DISCOVER/REQUEST/ACK) и статический IP в `RndisNetConfig`;
- `Pump`-хук: приложение отдаёт bulk OUT в `Rndis::receive()`, а `poll()`
  отправляет TX и обслуживает RX.

```cpp
usb::rndis::Rndis rndis;
net::RndisFactory net(rndis, hal, cfg);
net.init();
net.set_pump(on_usb_pump, &ctx);
mqtt::Client<768> mq;
mq.init(net, mqtt_cfg);
```

Это не lwIP: TCP — минимальный клиент (без SACK/окна/retransmit-очереди),
рассчитанный на один-два сокета; для полноценного TCP нужен HIL/стек.
Тест `tests/test_rndis_factory.cpp` проверяет ARP + TCP handshake на
RNDIS-кадрах. DHCP/ARP/TCP на реальном USB-хосте — HIL-задача.

## STM32 без W5500

Можно реализовать `net::Factory` поверх PPP/AT-модема или lwIP-порта;
протоколы MQTT/HTTP/SNTP менять не нужно.

## Проверка

[`tests/test_net.cpp`](../tests/test_net.cpp) подаёт байты сервера из памяти:
CONNACK/SUBACK/PUBLISH (QoS0/1/2), LWT, reconnect, TLS-фабрика через
fake-provider, TCP shell, HTTP-ответ с Content-Length, SNTP-пакет. Проверяются заголовки,
разбор, таймауты и отсутствие лишних аллокаций.
