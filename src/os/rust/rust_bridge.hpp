// os/rust/rust_bridge.hpp
// ---------------------------------------------------------------------------
// C ABI для Rust-приложения. Заголовок для C++-стороны; Rust-крейт объявляет
// те же extern "C" функции в src/lib.rs.
// ---------------------------------------------------------------------------
#pragma once

#include "../common.hpp"

namespace embed::rust {

/// Вызывается приложением. Если Rust-образ слинкован, вызывает
/// mcframe_rust_main(); иначе возвращает false.
bool run() noexcept;

/// Есть ли слинкованный Rust-образ.
bool available() noexcept;

} // namespace embed::rust
