# Rust-приложение

См. [`rust/README.md`](../rust/README.md). Кратко:

- `rust/mcframe_app` — `no_std` staticlib;
- C ABI-мост [`rust_bridge.cpp`](../src/os/rust/rust_bridge.cpp);
- `MCFRAME_RUST_APP=ON` в CMake собирает staticlib и линкует `mc_tests_rust`;
- для STM32: F1 — `thumbv7m-none-eabi`, F4/G4 — `thumbv7em-none-eabihf`;
  без установленного rust-std используется
  `cargo +nightly build -Z build-std=core --target …`;
- `MCFRAME_RUST_APP=ON` подставляет target и линкует staticlib (с
  `--undefined` для C ABI-моста);
- вызов из C++: `embed::rust::run()`; в примере это отдельная задача,
  но для гарантированного вытаскивания объекта используется прямая ссылка
  на `mcframe_rust_main()`.

Ограничения: нет `alloc`/`std`, строки — статические NUL-terminated; для
форматирования используйте `core::fmt::Write` в свой буфер.
