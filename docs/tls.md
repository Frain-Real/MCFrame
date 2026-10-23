# TLS: C3 и STM32

## Слой

[`tls.hpp`](../src/os/net/tls.hpp) — транспортно-независимый `TlsFactoryT<N>`:
сырой TCP + `TlsSession`, MQTT/HTTP продолжают работать через `net::Socket`.
[`tls_mbedtls.hpp`](../src/os/net/tls_mbedtls.hpp) — mbedTLS 2/3/4 provider.

## CA-store и provisioning

[`ca_store.hpp`](../src/os/net/ca_store.hpp):

```cpp
net::CaStore ca;
ca.add(1u, root_der, root_len);
ca.save(board::nvs(), "tls_ca");

net::CaStore loaded;
loaded.load(board::nvs(), "tls_ca");
net::TlsConfig cfg = loaded.config();
board::tls().set_config(cfg);
```

Bundle — contiguous DER/PEM; до 4 сертификатов по 2 КБ. Provisioning можно
делать через BLE/USB/OTA, записав bundle в NVS. Учтите лимит значения NVS
(`MCFRAME_NVS_VALUE_MAX`): большой CA bundle может не поместиться в один
ключ — храните его в отдельном разделе/внешнем носителе.

## ESP32-C3

`board::tls()` уже подключён к mbedTLS и lwIP. Нужно задать `CaStore::config()`
до первого HTTPS/MQTT-TLS.

## STM32

mbedTLS 4.x уже лежит в `third_party/mbedtls` (submodule). Нужны nested
submodules и Python-зависимость генератора:

```bash
git submodule update --init --recursive
python3 -m venv .build/mbedtls-venv
.build/mbedtls-venv/bin/pip install -r requirements-mbedtls.txt
export PATH="$PWD/.build/mbedtls-venv/bin:$PATH"
```

(CI делает это автоматически).

Для F4/G4 `MCFRAME_MBEDTLS` включается автоматически, если найден submodule
и Python-модули генератора; иначе CMake оставит его OFF с сообщением.
Для mock/явной сборки:

```bash
cmake ... -DMCFRAME_MBEDTLS=ON \
  -DPython3_EXECUTABLE="$(command -v python3)"
```

CMake соберёт `mbedtls`, `mbedx509` и `tfpsacrypto` из submodule и
подключит их к `MCFrame`. Для F4/G4 используется
[`cmake/mbedtls_config_stm32.h`](../cmake/mbedtls_config_stm32.h) и
[`cmake/tf_psa_crypto_config_stm32.h`](../cmake/tf_psa_crypto_config_stm32.h):
отключены FS/NET/TIMING, включены `MBEDTLS_PLATFORM_MS_TIME_ALT` и
ENTROPY driver; [`tls_platform_mbedtls.cpp`](../src/platforms/stm32f4g4/tls_platform_mbedtls.cpp)
реализует `mbedtls_ms_time()` и `mbedtls_platform_get_entropy()` на RNG.

Добавьте `#include "os/net/tls_mbedtls.hpp"` в приложение. Для mbedTLS 4
провайдер сам вызывает `psa_crypto_init()`; для mbedTLS 2/3 инициализирует
CTR-DRBG. HIL всё ещё обязателен.

В appendix-приложении линкуйте **`MCFrame::Tls`**, а не просто
`MCFrame::MCFrame`: этот INTERFACE-таргет заворачивает MCFrame + mbedTLS +
platform-hooks в `--start-group`, иначе статический линкер не повторит
`libMCFrame.a` после mbedTLS.

```cpp
net::MbedTlsProvider<2> provider;
net::TlsFactoryT<2> tls(w5500, provider);
tls.set_config(ca.config());
mqtt::Client<768> mq;
mq.init(tls, cfg);
```

```cmake
target_link_libraries(app PRIVATE MCFrame::Tls)
```

F4: diagnostic target:
```bash
cmake --build .build/stm32f4 --target tls_linkcheck
arm-none-eabi-size .build/stm32f4/firmware/phase3/tls_linkcheck.elf
```
Результат — 339 365 Б text + 4 992 Б data, влезает в слот 448 КБ. G4 (слот 48 КБ) полноценный mbedTLS TLS **не вмещает**;
нужен больший слот/урезанный профиль или TLS на внешнем модуле.

Внешний mbedTLS (без submodule) всё ещё можно подключить через
`MCFRAME_MBEDTLS_INCLUDE_DIR` и `MCFRAME_MBEDTLS_LIBRARIES`, но
`MCFrame::Tls`/platform-hooks рассчитаны на bundled-вариант.

Проверка: `tests/test_net.cpp` — fake TLS provider, HTTPS; CA-store —
add/save/load/remove. HIL: handshake, неверный CA/hostname, reconnect.

Ограничения: нет TLS server/mTLS-хранилища ключей; CA-store не защищён
secure element; HIL для STM32 не проводился.
