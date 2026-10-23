# cmake/rust.cmake
# ---------------------------------------------------------------------------
# Опциональная сборка Rust-приложения и линковка staticlib.
# По умолчанию OFF: Rust-тулчейн не обязателен для C++-сборки.
# ---------------------------------------------------------------------------
option(MCFRAME_RUST_APP "Собрать Rust-приложение и слинковать staticlib" OFF)

if(NOT MCFRAME_RUST_APP)
    return()
endif()

find_program(MCFRAME_CARGO cargo)
if(NOT MCFRAME_CARGO)
    message(FATAL_ERROR "MCFRAME_RUST_APP=ON, но cargo не найден в PATH")
endif()

set(_rust_env "CARGO_HOME=${CMAKE_SOURCE_DIR}/.cargo-home")
if(MCU_TARGET STREQUAL "mock")
    set(_rust_target "x86_64-unknown-linux-gnu")
    set(_rust_lib "${CMAKE_SOURCE_DIR}/rust/mcframe_app/target/${_rust_target}/release/libmcframe_app.a")
    set(_rust_build ${MCFRAME_CARGO} build --release --locked --target ${_rust_target})
elseif(MCU_TARGET STREQUAL "stm32")
    # F1 — Cortex-M3, soft-float ABI.
    set(_rust_target "thumbv7m-none-eabi")
    set(_rust_lib "${CMAKE_SOURCE_DIR}/rust/mcframe_app/target/thumbv7m-none-eabi/release/libmcframe_app.a")
    set(_rust_build ${MCFRAME_CARGO} build --release --locked -Z build-std=core
                    --target ${_rust_target})
elseif(MCU_TARGET STREQUAL "stm32f4" OR MCU_TARGET STREQUAL "stm32g4")
    # F4/G4 — Cortex-M4F, hard-float ABI (так собирается и C++ MCFrame).
    set(_rust_target "thumbv7em-none-eabihf")
    set(_rust_lib "${CMAKE_SOURCE_DIR}/rust/mcframe_app/target/thumbv7em-none-eabihf/release/libmcframe_app.a")
    # В окружении может не быть установленного rust-std для ARM. Nightly
    # умеет собрать core из rust-src: -Z build-std=core. CARGO_HOME — в
    # рабочем дереве, чтобы кэш не зависел от read-only $HOME.
    set(_rust_build ${MCFRAME_CARGO} build --release --locked -Z build-std=core
                    --target ${_rust_target})
else()
    message(FATAL_ERROR "Rust-приложение пока поддержано для mock/STM32")
endif()

add_custom_target(mcframe_rust_app
    COMMAND ${CMAKE_COMMAND} -E env "${_rust_env}"
            ${_rust_build}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/rust/mcframe_app
    COMMENT "Rust: mcframe_app (${_rust_target})")
set(MCFRAME_RUST_STATICLIB "${_rust_lib}" CACHE INTERNAL "Rust staticlib")
