# MCFrame

MCFrame — bare-metal C++17 framework для STM32F1/F4/G4 и ESP32-C3:
статическое распределение памяти, собственный планировщик, HAL/BSP, NVS,
OTA с двумя слотами, сеть, USB device, TLS и Rust-приложение.

Подробное описание вынесено в [`docs/reference.md`](docs/reference.md);
ниже — карта проекта и быстрый старт.

## Быстрый старт

F4/G4 используют bundled mbedTLS из `third_party/mbedtls`:
```bash
git submodule update --init --recursive
python3 -m venv .build/mbedtls-venv
.build/mbedtls-venv/bin/pip install -r requirements-mbedtls.txt
export PATH="$PWD/.build/mbedtls-venv/bin:$PATH"
```
Без Python-зависимостей CMake соберёт F4/G4 с `MCFRAME_MBEDTLS=OFF`;
включить TLS можно `-DMCFRAME_MBEDTLS=ON`.


```bash
# host-тесты (30 наборов)
./build.sh mock Debug && ctest --test-dir .build/mock --output-on-failure

# F1 flat
./build.sh stm32 Release

# F4/G4 self-test + bootloader
./build.sh stm32f4 Release
./build.sh stm32g4 Release

# ESP32-C3 (ESP-IDF 6.0.2)
cd firmware/esp32c3 && idf.py set-target esp32c3 && idf.py build

# Rust-приложение (nightly + rust-src, см. rust-toolchain.toml)
./build.sh stm32 Release -DMCFRAME_RUST_APP=ON -DMCFRAME_IMAGE=flat
```

## Что где

| Область | Код | Документация |
|---|---|---|
| Ядро, планировщик, IPC | [`src/os/core`](src/os/core), [`src/os/common.hpp`](src/os/common.hpp) | [`docs/reference.md`](docs/reference.md) |
| HAL/платформы | [`src/hal`](src/hal), [`src/platforms`](src/platforms) | [`docs/hal.md`](docs/hal.md), [`docs/targets.md`](docs/targets.md) |
| Платы/BSP | [`src/boards`](src/boards) | [`docs/reference.md`](docs/reference.md) |
| NVS | [`src/os/nvs`](src/os/nvs) | [`docs/phase6.md`](docs/phase6.md) |
| OTA/загрузчик | [`src/os/ota`](src/os/ota), [`src/os/boot`](src/os/boot), [`bootloader`](bootloader) | [`docs/phase6.md`](docs/phase6.md) |
| Сеть (MQTT/HTTP/SNTP/TLS) | [`src/os/net`](src/os/net) | [`docs/network.md`](docs/network.md) |
| W5500 | [`src/os/net/w5500.hpp`](src/os/net/w5500.hpp) | [`docs/network.md`](docs/network.md) |
| USB device | [`src/os/usb`](src/os/usb), [`src/platforms/stm32f4g4/usb_otg_fs.cpp`](src/platforms/stm32f4g4/usb_otg_fs.cpp), [`src/platforms/stm32g4/usb_fs_g4.cpp`](src/platforms/stm32g4/usb_fs_g4.cpp) | [`docs/usb.md`](docs/usb.md) |
| Rust | [`rust/mcframe_app`](rust/mcframe_app), [`src/os/rust`](src/os/rust) | [`docs/rust.md`](docs/rust.md) |
| ФС SD/littlefs | [`src/platforms/stm32/sd_spi.hpp`](src/platforms/stm32/sd_spi.hpp), [`src/os/fs`](src/os/fs) | [`docs/fs.md`](docs/fs.md) |
| HIL | — | [`docs/hil.md`](docs/hil.md) |

## Возможности

- **Ядро:** статические TCB/стеки/очереди, приоритеты, mutex/PI, semaphore,
  mailbox, event flags, work queue, shell.
- **Питание:** tickless/STOP + RTC wake, IWDG, PVD/brown-out; F1/F4/G4.
- **OTA:** CRC32/SHA-256/подпись, два слота, откат, meta-backup;
  F4/G4 bootloader в своих секторах.
- **Сеть:** MQTT 3.1.1 (QoS0/1/2, LWT, keepalive/reconnect), HTTP/1.1,
  SNTP, TLS (mbedTLS на C3), W5500 SPI с DHCP/DNS/link/IRQ, renew и TCP listener/accept.
- **USB:** CDC-ACM, DFU (RAM/flash), MSC (READ/WRITE), RNDIS + полный
  `net::Factory` (ARP/IPv4/UDP/TCP/DNS/DHCP), composite, F4 OTG FS и G4 USB FS;
  CDC shell/OTA; TCP shell.
- **BLE:** peripheral/observer/central, GATT client service/char discovery,
  read/write by handle, CCCD/notify.
- **W5500:** DHCP/DNS/link/IRQ и TCP listen/accept.
- **FS:** SD SPI + littlefs 2.x (LFS_NO_MALLOC), erase/lock/hotplug.
- **Качество:** libFuzzer-цели, coverage-скрипт, CI jobs.
- **Rust:** `no_std` staticlib, C ABI-мост, safe-ish API, FFI
  GPIO/SPI/I2C/NVS/net/USB, F1/F4/G4 targets.
- **Тесты:** 30 host-наборов, ASan/UBSan, fuzz smoke, coverage,
  FFI NVS/I2C/net/USB; CI: GCC/ARM, C3, Rust.

## Статус и ограничения

- F1 flat: 2 КБ NVS + 2 КБ OTA meta; F1 slot: без внутренней NVS.
- F4: слот A 448 КБ, слот B 384 КБ, meta 16 КБ + backup 16 КБ;
  загрузчик по умолчанию `crc` из-за 16 КБ, `sha/signed` — явной опцией.
- G4: слоты по 48 КБ, meta 4 КБ + backup 4 КБ, NVS 4 КБ.
- W5500/G4 USB/G4 RNDIS требуют HIL; см. [`docs/hil.md`](docs/hil.md).
- TLS на STM32 требует mbedTLS в проекте; на C3 включён `board::tls()`.
- F1 slot почти заполнен; новые крупные функции — во flat/F4/G4.

## Документация

- [`docs/reference.md`](docs/reference.md) — полный справочник;
- [`docs/network.md`](docs/network.md) — MQTT/HTTP/SNTP/TLS/W5500/RNDIS;
- [`docs/usb.md`](docs/usb.md) — CDC/DFU/MSC/RNDIS/composite;
- [`docs/fs.md`](docs/fs.md) — SD/littlefs;
- [`docs/ble.md`](docs/ble.md) — GATT client;
- [`docs/shell.md`](docs/shell.md) — shell по USB CDC и TCP;
- [`docs/rust.md`](docs/rust.md) — Rust + FFI;
- [`docs/tls.md`](docs/tls.md) — TLS, bundled mbedTLS, CA-store, provisioning;
- [`docs/hil.md`](docs/hil.md) — HIL-чеклист.

## CI

[`.github/workflows/ci.yml`](.github/workflows/ci.yml): mock Debug/Release,
ASan/UBSan, target matrix, STM32F1/F4/G4, ESP32-C3 IDF, Rust, fuzz/coverage.

## Лицензия

См. [`LICENSE`](LICENSE), если присутствует в репозитории.
