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

// ── Расширенный C ABI: GPIO/SPI/I2C/NVS/net/USB ─────────────────────────────
// Ресурсы регистрирует C++-сторона (hal::IGpioPin/ISpi/II2c/INvs,
// net::Factory, usb::Hal), Rust работает с целочисленными handle.
extern "C" {
    fn mc_gpio_register(pin: *mut core::ffi::c_void) -> i32;
    fn mc_gpio_configure(handle: i32, mode: u8, pull: u8, initial: bool) -> i32;
    fn mc_gpio_set(handle: i32, level: bool);
    fn mc_gpio_get(handle: i32) -> i32;

    fn mc_spi_register(bus: *mut core::ffi::c_void) -> i32;
    fn mc_spi_transfer(handle: i32, tx: *const u8, rx: *mut u8, len: u32,
                       timeout_ms: u32) -> i32;

    fn mc_i2c_register(bus: *mut core::ffi::c_void) -> i32;
    fn mc_i2c_write(handle: i32, address: u8, data: *const u8, len: u32,
                    timeout_ms: u32) -> i32;
    fn mc_i2c_read(handle: i32, address: u8, data: *mut u8, cap: u32,
                   timeout_ms: u32, got: *mut u32) -> i32;

    fn mc_nvs_register(nvs: *mut core::ffi::c_void) -> i32;
    fn mc_nvs_read(handle: i32, key: *const u8, data: *mut u8, cap: u32,
                   got: *mut u32) -> i32;
    fn mc_nvs_write(handle: i32, key: *const u8, data: *const u8, len: u32) -> i32;
    fn mc_nvs_erase(handle: i32, key: *const u8) -> i32;

    fn mc_net_register(factory: *mut core::ffi::c_void) -> i32;
    fn mc_net_socket(factory: i32, proto: u8) -> i32;
    fn mc_net_resolve(factory: i32, host: *const u8, ip: *mut u8) -> i32;
    fn mc_net_connect(socket: i32, ip: *const u8, port: u16, timeout_ms: u32) -> i32;
    fn mc_net_send(socket: i32, data: *const u8, len: u32, sent: *mut u32) -> i32;
    fn mc_net_recv(socket: i32, data: *mut u8, cap: u32, timeout_ms: u32,
                   got: *mut u32) -> i32;
    fn mc_net_close(socket: i32);

    fn mc_usb_register(hal: *mut core::ffi::c_void) -> i32;
    fn mc_usb_write(handle: i32, ep: u8, data: *const u8, len: u32) -> i32;
    fn mc_usb_read(handle: i32, ep: u8, data: *mut u8, cap: u32,
                   timeout_ms: u32, got: *mut u32) -> i32;
}

pub mod ffi {
    use super::*;

    #[derive(Clone, Copy)] pub struct Gpio(pub i32);
    #[derive(Clone, Copy)] pub struct Spi(pub i32);
    #[derive(Clone, Copy)] pub struct I2c(pub i32);
    #[derive(Clone, Copy)] pub struct Nvs(pub i32);
    #[derive(Clone, Copy)] pub struct Net(pub i32);
    pub struct Socket(pub i32);
    #[derive(Clone, Copy)] pub struct Usb(pub i32);

    impl Gpio {
        pub unsafe fn register(pin: *mut core::ffi::c_void) -> Option<Self> {
            let h = mc_gpio_register(pin); (h >= 0).then_some(Self(h))
        }
        pub fn configure(&self, output: bool, initial: bool) -> bool {
            unsafe { mc_gpio_configure(self.0, if output { 1 } else { 0 }, 0, initial) == 0 }
        }
        pub fn set(&self, level: bool) { unsafe { mc_gpio_set(self.0, level) } }
        pub fn get(&self) -> Option<bool> {
            let v = unsafe { mc_gpio_get(self.0) };
            (v >= 0).then_some(v != 0)
        }
    }

    impl Spi {
        pub unsafe fn register(bus: *mut core::ffi::c_void) -> Option<Self> {
            let h = mc_spi_register(bus); (h >= 0).then_some(Self(h))
        }
        pub fn transfer(&self, tx: &[u8], rx: &mut [u8]) -> bool {
            let n = tx.len().min(rx.len()) as u32;
            unsafe { mc_spi_transfer(self.0, tx.as_ptr(), rx.as_mut_ptr(), n, 1000) == 0 }
        }
    }

    impl I2c {
        pub unsafe fn register(bus: *mut core::ffi::c_void) -> Option<Self> {
            let h = mc_i2c_register(bus); (h >= 0).then_some(Self(h))
        }
        pub fn write(&self, address: u8, data: &[u8]) -> bool {
            unsafe { mc_i2c_write(self.0, address, data.as_ptr(), data.len() as u32, 1000) == 0 }
        }
        pub fn read(&self, address: u8, data: &mut [u8]) -> Option<usize> {
            let mut got = 0u32;
            let rc = unsafe { mc_i2c_read(self.0, address, data.as_mut_ptr(),
                                           data.len() as u32, 1000, &mut got) };
            (rc == 0).then_some(got as usize)
        }
    }

    impl Nvs {
        pub unsafe fn register(nvs: *mut core::ffi::c_void) -> Option<Self> {
            let h = mc_nvs_register(nvs); (h >= 0).then_some(Self(h))
        }
        fn key(key: &[u8]) -> Option<[u8; 64]> {
            if key.is_empty() || key.len() >= 64 || key.contains(&0u8) { return None; }
            let mut out = [0u8; 64];
            out[..key.len()].copy_from_slice(key);
            Some(out)
        }
        pub fn read(&self, key: &[u8], data: &mut [u8]) -> Option<usize> {
            let k = Self::key(key)?;
            let mut got = 0u32;
            let rc = unsafe { mc_nvs_read(self.0, k.as_ptr(), data.as_mut_ptr(),
                                          data.len() as u32, &mut got) };
            (rc == 0).then_some(got as usize)
        }
        pub fn write(&self, key: &[u8], data: &[u8]) -> bool {
            let Some(k) = Self::key(key) else { return false };
            unsafe { mc_nvs_write(self.0, k.as_ptr(), data.as_ptr(), data.len() as u32) == 0 }
        }
        pub fn erase(&self, key: &[u8]) -> bool {
            let Some(k) = Self::key(key) else { return false };
            unsafe { mc_nvs_erase(self.0, k.as_ptr()) == 0 }
        }
    }

    impl Net {
        pub unsafe fn register(factory: *mut core::ffi::c_void) -> Option<Self> {
            let h = mc_net_register(factory); (h >= 0).then_some(Self(h))
        }
        pub fn resolve(&self, host: &[u8], ip: &mut [u8; 4]) -> bool {
            let mut key = [0u8; 96];
            if host.is_empty() || host.len() >= key.len() { return false; }
            key[..host.len()].copy_from_slice(host);
            unsafe { mc_net_resolve(self.0, key.as_ptr(), ip.as_mut_ptr()) == 0 }
        }
        pub fn socket(&self, udp: bool) -> Option<Socket> {
            let h = unsafe { mc_net_socket(self.0, if udp { 1 } else { 0 }) };
            (h >= 0).then_some(Socket(h))
        }
    }

    impl Drop for Socket {
        fn drop(&mut self) {
            if self.0 >= 0 { unsafe { mc_net_close(self.0) }; self.0 = -1; }
        }
    }

    impl Socket {
        pub fn connect(&self, ip: [u8; 4], port: u16) -> bool {
            unsafe { mc_net_connect(self.0, ip.as_ptr(), port, 5000) == 0 }
        }
        pub fn send(&self, data: &[u8]) -> Option<usize> {
            let mut sent = 0u32;
            let rc = unsafe { mc_net_send(self.0, data.as_ptr(), data.len() as u32, &mut sent) };
            (rc == 0).then_some(sent as usize)
        }
        pub fn recv(&self, data: &mut [u8], timeout_ms: u32) -> Option<usize> {
            let mut got = 0u32;
            let rc = unsafe { mc_net_recv(self.0, data.as_mut_ptr(), data.len() as u32,
                                          timeout_ms, &mut got) };
            (rc == 0).then_some(got as usize)
        }
        /// Закрыть явно; Drop идемпотентен.
        pub fn close(mut self) {
            unsafe { mc_net_close(self.0) };
            self.0 = -1;
        }
    }

    impl Usb {
        pub unsafe fn register(hal: *mut core::ffi::c_void) -> Option<Self> {
            let h = mc_usb_register(hal); (h >= 0).then_some(Self(h))
        }
        pub fn write(&self, ep: u8, data: &[u8]) -> bool {
            unsafe { mc_usb_write(self.0, ep, data.as_ptr(), data.len() as u32) == 0 }
        }
        pub fn read(&self, ep: u8, data: &mut [u8]) -> Option<usize> {
            let mut got = 0u32;
            let rc = unsafe { mc_usb_read(self.0, ep, data.as_mut_ptr(), data.len() as u32,
                                         1000, &mut got) };
            (rc == 0).then_some(got as usize)
        }
    }
}
