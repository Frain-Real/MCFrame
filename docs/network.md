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

[`mqtt::Client`](../src/os/net/mqtt.hpp) — MQTT 3.1.1, QoS 0/1, subscribe,
ping, входящие PUBLISH:

```cpp
mqtt::Client<768> mq;
mqtt::Config cfg{};
cfg.host = "broker.local";
cfg.client_id = "sensor-1";
mq.init(board::sockets(), cfg);
mq.connect();
mq.subscribe("cmd/#");
mq.publish("telemetry/temp", "21.5", 4);
mq.set_callback(on_message, this);
for (;;) { (void)mq.poll(1000); }
```

Буфер статический, размер задаётся шаблоном. QoS 2 не поддерживается —
возвращается `NotSupported`, а не тихая деградация.

## HTTP

[`http::Client`](../src/os/net/http.hpp) — HTTP/1.1 GET/POST, Content-Length
и chunked, тело через sink:

```cpp
http::Response r{};
http::Client<512> http;
http.get(board::sockets(), "http://example.com/data", r, sink, this);
```

HTTPS не поддерживается на этом уровне: `parse_url()` помечает `https://`, а
`request()` возвращает `NotSupported`. На C3 HTTPS OTA идёт отдельным
`esp_https_ota`-путём (см. `firmware/esp32c3/components/mcframe/ota_https.cpp`).

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
// F4/G4: board::net() уже собран на SPI1/CS; на других платах создайте
// net::W5500FactoryT<4> net{board::spiX(), board::spi_cs()}.
net::W5500Config cfg{};              // MAC, IP, GW, mask
cfg.ip[3] = 50;
(void)board::net().init(cfg);

// Дальше — как на C3:
mqtt::Client<768> client;
client.init(net, mqtt_cfg);
```

Особенности:

- `resolve()` принимает только IPv4-литерал: у W5500 нет DNS;
- CS и SPI — из платы; W5500 можно повесить на SPI1/SPI2;
- сокетов до 8 (по 2 КБ TX/RX на сокет в примере);
- драйвер не привязан к STM32 и тестируется на модели W5500
  (`tests/test_w5500.cpp`, 11 проверок).

## STM32 без W5500

Можно реализовать `net::Factory` поверх PPP/AT-модема или lwIP-порта;
протоколы MQTT/HTTP/SNTP менять не нужно.

## Проверка

[`tests/test_net.cpp`](../tests/test_net.cpp) подаёт байты сервера из памяти:
CONNACK/SUBACK/PUBLISH, HTTP-ответ с Content-Length, SNTP-пакет. Проверяются
заголовки, разбор, таймауты и отсутствие лишних аллокаций.
