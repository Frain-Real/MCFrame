// async_kernel.hpp
#pragma once
// ---------------------------------------------------------------------------
// Адаптер «асинхронный ввод-вывод -> планировщик и время».
//
// Асинхронный модуль не включает sheduler.hpp и timer.hpp напрямую: это
// тянуло бы их в каждый TU. Здесь собраны ровно те операции, которые ему
// нужны, и все они безопасны при вызове ДО старта планировщика —
// возвращают «недоступно» вместо зависания.
// ---------------------------------------------------------------------------

#include "../core/primitives/mutex.hpp"   // sched_api: current/block_on/unblock_one/wake/yield
#include "../config/time_config.hpp"
#include "../power/power.hpp"             // удержание системы от глубокого сна

namespace embed::async::kernel {

/// Текущая задача либо nullptr (до start() планировщика — nullptr).
inline TaskHandler current() noexcept {
    return sched_api::current();
}

/// Есть ли контекст задачи: только тогда имеет смысл «усыпить и ждать».
inline bool in_task() noexcept {
    return sched_api::current() != nullptr;
}

/// Заблокировать текущую задачу на объекте obj на ticks тиков
/// (0 — без таймаута). Безопасно вызывать из контекста задачи.
inline Status block_on(void* obj, uint32_t ticks) noexcept {
    return sched_api::block_on(obj, ticks);
}

/// Разбудить задачу. Безопасно вызывать ИЗ ПРЕРЫВАНИЯ:
/// не берёт критическую секцию, состояние задачи меняется под PRIMASK.
inline bool wake(TaskHandler t) noexcept {
    return sched_api::wake(t);
}

/// Уступить процессор, оставаясь готовым (не блокироваться).
inline void yield() noexcept {
    sched_api::yield();
}

/// Доступно ли монотонное время: таймер инициализирован и запущен.
/// Пока таймера нет, асинхронное ожидание переходит на ограниченное
/// число уступок, чтобы не зависнуть навсегда.
inline bool has_time() noexcept {
    return config::OsTimer::TimeAvailable();
}

/// Монотонное время, мс.
inline uint64_t time_ms() noexcept {
    return config::OsTimer::NowMs();
}

/// Удержание системы от глубокого сна на время ожидания периферии.
///
/// ЗАЧЕМ ЭТО В МОДУЛЕ, А НЕ В ПРИКЛАДНОМ КОДЕ
/// В глубоком сне (STOP) гаснет тактирование, поэтому незавершённый обмен по
/// UART/I2C/DMA не продвинется НИКОГДА: задача, ждущая его без таймаута,
/// зависла бы навсегда, а с таймаутом — потеряла бы данные, которые уже
/// должна была принять. Пока идёт ожидание, система обязана оставаться
/// работоспособной, и это свойство операции ввода-вывода, а не приложения:
/// поэтому удержание берёт сам модуль (см. async::completion::wait).
///
/// Стоимость удержания — ноль, пока супервизор энергосбережения не
/// инициализирован (или глубокий сон выключен).
///
/// ЧЕГО УДЕРЖАНИЕ НЕ ДЕЛАЕТ: оно не запрещает Sleep (WFI). Остановку ядра
/// будит любое прерывание, в том числе от той самой периферии, — это и есть
/// нормальное поведение ожидания.
struct KeepAwake {
    KeepAwake() noexcept : lease_(power::Supervisor::instance().keep_awake()) {}
    ~KeepAwake() noexcept = default;

    KeepAwake(const KeepAwake&)            = delete;
    KeepAwake& operator=(const KeepAwake&) = delete;
    KeepAwake(KeepAwake&&)                 = delete;
    KeepAwake& operator=(KeepAwake&&)      = delete;

    /// Удерживается ли система прямо сейчас (диагностика/тесты).
    bool held() const noexcept { return lease_.held(); }

private:
    power::Supervisor::Lease lease_;
};

} // namespace embed::async::kernel
