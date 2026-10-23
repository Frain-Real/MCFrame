# Rust-приложение MCFrame

Крейт `rust/mcframe_app` — пример `no_std`-приложения, которое линкуется с
C++-прошивкой MCFrame. Он не использует `std`, `alloc`, `libc` и Rust
runtime: только C ABI-функции из [`src/os/rust/rust_bridge.cpp`](../src/os/rust/rust_bridge.cpp).

## Сборка

Цель зависит от ядра и ABI C++-прошивки:

| платформа | Rust target |
|---|---|
| STM32F1 (Cortex-M3, soft-float) | `thumbv7m-none-eabi` |
| STM32F4/G4 (Cortex-M4F, hard-float) | `thumbv7em-none-eabihf` |
| host/mock | обычный host |

Версия nightly и `rust-src` зафиксированы в корневом
[`rust-toolchain.toml`](../rust-toolchain.toml) (`nightly-2026-07-22`).
В окружении может не быть установленного `rust-std` для ARM; nightly собирает
`core` из `rust-src`:

```bash
cd rust/mcframe_app
# F1:
CARGO_HOME=../../.cargo-home cargo build --release --locked -Z build-std=core --target thumbv7m-none-eabi
# F4/G4:
CARGO_HOME=../../.cargo-home cargo build --release --locked -Z build-std=core --target thumbv7em-none-eabihf
```

Результат:

```text
rust/mcframe_app/target/thumbv7m-none-eabi/release/libmcframe_app.a
rust/mcframe_app/target/thumbv7em-none-eabihf/release/libmcframe_app.a
```

`CARGO_HOME` вынесен в рабочее дерево, потому что `$HOME/.cargo` может быть
read-only.

## Интеграция в прошивку

1. Соберите staticlib под тот же target, что и C++-прошивка.
2. Добавьте её в линковку приложения:

```cmake
add_executable(my_fw ...)
# Мост mc_log/mc_sleep_ms лежит в libMCFrame.a, а Rust-архив идёт после неё.
# --undefined заставляет вытащить объект моста до обработки Rust.
target_link_libraries(my_fw PRIVATE
    ${CMAKE_SOURCE_DIR}/rust/mcframe_app/target/thumbv7m-none-eabi/release/libmcframe_app.a
    MCFrame::MCFrame)
target_link_options(my_fw PRIVATE
    -Wl,--undefined=mc_log
    -Wl,--undefined=mc_sleep_ms
    -Wl,--undefined=mc_uptime_ms)
```

Либо включите `-DMCFRAME_RUST_APP=ON` — CMake сделает это сам.

3. Вызовите Rust-приложение из C++:

```cpp
#include "os/rust/rust_bridge.hpp"
#include "os/core/task/sheduler.hpp"

static uint32_t rust_stack[512];
void rust_task(void*) {
    embed::rust::run();   // вызывает mcframe_rust_main()
}

// после scheduler::instance().init():
embed::scheduler::instance().add_task(&rust_task, rust_stack, 512);
```

Или вызовите `mcframe_rust_main()` напрямую из своей задачи.

## C ABI

Из Rust доступны:

| функция | назначение |
|---|---|
| `mc_log(const char*)` | строка в платформенный лог |
| `mc_sleep_ms(uint32_t)` | сон задачи; вне scheduler — `Unavailable` |
| `mc_yield()` | уступить планировщик |
| `mc_uptime_ms()` | монотонные миллисекунды |
| `mc_epoch_seconds()` | календарное время (0, если не синхронизировано) |

Строки обязаны быть NUL-terminated и жить дольше вызова. Для форматирования
используйте `core::fmt::Write` в собственный буфер: `alloc` в примере нет.

## Проверка на хосте

```bash
cargo build --release
g++ -std=c++17 -I src tests/test_rust_bridge.cpp \
    .build/mock/src/libMCFrame.a target/release/libmcframe_app.a \
    -o /tmp/rust-smoke
```

(Путь к `libMCFrame.a` зависит от каталога сборки.)
