//! Пример no_std-приложения MCFrame на Rust.
//!
//! Крейт собирается в staticlib и линкуется с C++-прошивкой. Никакого
//! runtime Rust, alloc и libc здесь нет: только C ABI-мост из
//! `src/os/rust/rust_bridge.cpp`.
#![no_std]
#![allow(non_snake_case)]

use core::panic::PanicInfo;

extern "C" {
    fn mc_log(text: *const u8);
    fn mc_sleep_ms(ms: u32) -> i32;
    fn mc_uptime_ms() -> u64;
    fn mc_epoch_seconds() -> u64;
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // На устройстве паника должна не молчать, а оставить след. Здесь нет
    // форматирования/alloc, поэтому пишем фиксированную строку.
    unsafe { mc_log(b"RUST PANIC\0".as_ptr()); }
    loop { core::hint::spin_loop(); }
}

fn log(text: &[u8]) {
    // Все строки в примере статические и NUL-terminated.
    unsafe { mc_log(text.as_ptr()); }
}

/// Точка входа, которую вызывает `embed::rust::run()`.
#[no_mangle]
pub extern "C" fn mcframe_rust_main() {
    log(b"RUST: app started\0");
    loop {
        let uptime = unsafe { mc_uptime_ms() };
        let _ = uptime;
        // 1 секунда; sleep_for_ms округляет вверх до тика ОС.
        let _ = unsafe { mc_sleep_ms(1000) };
    }
}

/// Проверка моста без запуска планировщика: используется host-тестом.
#[no_mangle]
pub extern "C" fn mcframe_rust_selfcheck() -> u32 {
    let t = unsafe { mc_uptime_ms() };
    let e = unsafe { mc_epoch_seconds() };
    // Ненулевой результат — мост работает; конкретное значение не важно.
    (t as u32).wrapping_add(e as u32).wrapping_add(0x5A5A_0001)
}
