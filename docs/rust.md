# Rust-приложение

См. [`rust/README.md`](../rust/README.md). Кратко:

- `rust/mcframe_app` — `no_std` staticlib;
- C ABI-мост [`rust_bridge.cpp`](../src/os/rust/rust_bridge.cpp);
- `MCFRAME_RUST_APP=ON` в CMake собирает staticlib и линкует `mc_tests_rust`;
- для STM32: F1 — `thumbv7m-none-eabi`, F4/G4 — `thumbv7em-none-eabihf`;
  без установленного rust-std используется
  `cargo build -Z build-std=core --target …`;
- корневой [`rust-toolchain.toml`](../rust-toolchain.toml) фиксирует
  `nightly-2026-07-22` и `rust-src`; CI job `rust` проверяет host, F1 и
  обе ARM staticlib;
- `MCFRAME_RUST_APP=ON` подставляет target и линкует staticlib (с
  `--undefined` для C ABI-моста);
- вызов из C++: `embed::rust::run()`; в примере это отдельная задача,
  но для гарантированного вытаскивания объекта используется прямая ссылка
  на `mcframe_rust_main()`.

## FFI: GPIO/SPI/I2C/NVS/net/USB

C++-сторона регистрирует ресурсы, Rust получает целочисленные handle
(`mc_gpio_register`, `mc_spi_register`, `mc_i2c_register`, `mc_nvs_register`,
`mc_net_register`, `mc_usb_register`). Для каждого — configure/transfer/read/
write; ошибки — отрицательный `Status`, данные — явные `got`.

```rust
let gpio = unsafe { ffi::Gpio::register(pin_ptr) }.unwrap();
gpio.set(true);
let nvs = unsafe { ffi::Nvs::register(nvs_ptr) }.unwrap();
let mut buf = [0u8; 32];
let _ = nvs.read(b"key", &mut buf);
```

Это не заменяет безопасный Rust-API: handle остаются `Copy` и не проверяются
на время жизни; приложение обязано само не использовать ресурс после
уничтожения C++-объекта.

Ограничения: нет `alloc`/`std`, строки — статические NUL-terminated; для
форматирования используйте `core::fmt::Write` в свой буфер.
