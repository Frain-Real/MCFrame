// async_port.hpp
#pragma once
// ---------------------------------------------------------------------------
// АСИНХРОННЫЙ ВВОД-ВЫВОД С ЛИНЕЙНЫМ API
//
// ИДЕЯ
// Пользователь пишет обычный последовательный код — как будто вызовы
// блокирующие:
//
//     AsyncPort<Hal> port;
//     port.write("hello\r\n", 7);
//     port.read(buf, sizeof buf, n);
//     port.writeLine("done");
//
// Внутри ни одного колбэка со стороны пользователя: задача усыпляется,
// а передача идёт по DMA или прерываниям. Как только она завершилась,
// обработчик прерывания будит задачу и линейный код продолжается со
// следующей строки.
//
// СТРАТЕГИЯ ВЫБИРАЕТСЯ ИНТЕРФЕЙСОМ
//   Polling — цикл опроса с уступкой планировщику (низкая нагрузка на CPU);
//   Async   — прерывания: HAL зовёт onRxComplete()/onTxComplete();
//   Dma     — передача без участия ядра: HAL зовёт onDmaComplete().
//
// В любом случае пользователь видит один и тот же линейный API.
//
// ЧЕГО ЗДЕСЬ НЕТ И ПОЧЕМУ
// Модуль не знает про конкретный HAL: всё общение с железом идёт через
// trait-структуру Hal. Так модуль остаётся header-only и не тянет за собой
// DMA-менеджер, ADC и прочие подсистемы.
// ---------------------------------------------------------------------------

#include "async_completion.hpp"

#include <type_traits>

namespace embed::async {

/// Стратегия выполнения передачи.
/// Выбирается конфигурацией интерфейса, а не прикладным кодом.
enum class Strategy : uint8_t {
    /// Опрос с уступкой планировщику. Годится всегда, но занимает CPU.
    Polling = 0,
    /// Прерывания: HAL сам сообщает о завершении.
    Async,
    /// DMA: ядро не участвует в пересылке, только в ожидании.
    Dma,
};

/// Значения по умолчанию для необязательных операций HAL.
///
/// Зачем: заставлять автора HAL реализовывать poll() и maxChunk(), даже
/// если он работает на DMA, — лишняя работа. Через этот шаблон модуль
/// берёт реализацию из Hal, если она есть, иначе подставляет заглушку.
template <typename Hal, typename = void>
struct HalDefaultPoll {
    static void poll(async::completion& /*c*/, void* /*arg*/) noexcept {}
};

template <typename Hal>
struct HalDefaultPoll<Hal, std::void_t<decltype(Hal::poll_on_irq())>> {
    static void poll(async::completion& c, void* arg) noexcept {
        Hal::poll_on_irq();
        (void)c;
        (void)arg;
    }
};

template <typename Hal, typename = void>
struct HalDefaultChunk {
    static constexpr Size value = 0u;      // 0 = без ограничения
};

template <typename Hal>
struct HalDefaultChunk<Hal, std::void_t<decltype(Hal::max_chunk())>> {
    static constexpr Size value = Hal::max_chunk();
};

template <typename Hal, typename = void>
struct HalDefaultCancel {
    static void cancel() noexcept {}
};

template <typename Hal>
struct HalDefaultCancel<Hal, std::void_t<decltype(Hal::cancel_transfer())>> {
    static void cancel() noexcept { Hal::cancel_transfer(); }
};

/// Порт последовательного ввода-вывода с линейным API.
///
/// Hal — trait-структура, описывающая конкретный HAL. Требуемые операции:
///
///   static Status configure(...);                  // по желанию
///   static Strategy strategy() noexcept;           // Polling/Async/Dma
///
///   // Передача. Должна ВЕРНУТЬ УПРАВЛЕНИЕ СРАЗУ (не ждать завершения)
///   // и так или иначе завершить объект completion.
///   static Status startWrite(completion& c, const void* data, Size len) noexcept;
///   static Status startRead (completion& c, void* buf, Size cap) noexcept;
///
/// ВАЖНО про время жизни: объект завершения принадлежит ПОРТУ (поле
/// completion_), а не локальной переменной. Драйвер сохраняет на него
/// указатель и завершает операцию позже — если бы объект жил на стеке
/// вызова, получалось бы обращение к уничтоженному объекту (это поймал ASan).
///
///   // Обслуживание. Вызывается модулем в циклах ожидания; для Polling
///   // здесь опрос регистров и завершение completion.
///   static void poll(async::completion& c) noexcept;
///
///   static void setRxCallback(completion& c) noexcept;   // привязка к ISR
///   static void cancel() noexcept;                       // прервать передачу
///   static Size maxChunk() noexcept;                     // предел за раз
///
/// Готовый пример trait-структуры — tests/test_async.cpp, а для реального
/// UART/DMA — docs в примере.
template <typename Hal, uint32_t DefaultTimeoutMs = 1000u>
class AsyncPort {
public:
    using Completion = async::completion;

    AsyncPort() noexcept  = default;

    AsyncPort(const AsyncPort&)            = delete;
    AsyncPort& operator=(const AsyncPort&) = delete;

    // ─────────────────────────── Конфигурация ───────────────────────────

    /// Таймаут по умолчанию для всех операций, мс.
    /// 0 означает «ждать без ограничения».
    void setDefaultTimeout(uint32_t ms) noexcept { default_timeout_ms_ = ms; }
    uint32_t defaultTimeout() const noexcept { return default_timeout_ms_; }

    /// Включить/выключить callback-стиль. Если включён, дополнительно к
    /// пробуждению задачи вызывается пользовательский колбэк —
    /// нужен, только если результат нужен вне задачи (например в ISR).
    void setCompletionCallback(DoneCallback cb, void* arg) noexcept {
        user_cb_     = cb;
        user_cb_arg_ = arg;
    }

    Strategy strategy() const noexcept { return Hal::strategy(); }

    // ─────────────────────────── Передача ───────────────────────────

    /// Отправить буфер целиком. Возвращает Status::Ok, если ушли все байты.
    ///
    /// Именно этот вызов выглядит как блокирующий: внутри задача спит,
    /// пока Hal не завершит передачу.
    Status write(const void* data, Size len) noexcept {
        return write(data, len, default_timeout_ms_);
    }

    Status write(const void* data, Size len, uint32_t timeout_ms) noexcept {
        if (data == nullptr || len == 0u) return Status::InvalidArg;

        const auto* bytes = static_cast<const uint8_t*>(data);
        Size sent = 0u;

        while (sent < len) {
            Size chunk = len - sent;
            constexpr Size kLimit = HalDefaultChunk<Hal>::value;
            if (kLimit != 0u && chunk > kLimit) chunk = kLimit;

            const Status st = transfer(chunk, timeout_ms,
                                       [&](Completion& c) {
                                           return Hal::startWrite(c, bytes + sent, chunk);
                                       });
            if (st != Status::Ok) return st;

            // Hal обязан отчитаться, сколько реально передал: иначе
            // при частичной записи цикл зациклился бы.
            Size done = last_completion_.transferred();
            if (done == 0u) done = chunk;      // драйвер не сообщает — считаем всё
            if (done > chunk) done = chunk;
            sent += done;
        }
        return Status::Ok;
    }

    /// Отправить строку (без завершающего нуля).
    Status write(const char* text) noexcept {
        if (text == nullptr) return Status::InvalidArg;
        Size n = 0u;
        while (text[n] != '\0') ++n;
        return write(text, n);
    }

    /// Отправить строку с CRLF.
    Status writeLine(const char* text) noexcept {
        const Status st = write(text);
        if (st != Status::Ok) return st;
        return write("\r\n", 2u);
    }

    /// Прочитать хотя бы один байт. Возвращает Ok, если прочитано > 0.
    /// bytes_read == 0 при Ok означает «данных нет» (для неблокирующего HAL).
    Status read(void* buf, Size capacity, Size& bytes_read) noexcept {
        return read(buf, capacity, bytes_read, default_timeout_ms_);
    }

    Status read(void* buf, Size capacity, Size& bytes_read,
                uint32_t timeout_ms) noexcept {
        bytes_read = 0u;
        if (buf == nullptr || capacity == 0u) return Status::InvalidArg;

        const Status st = transfer(capacity, timeout_ms,
                                   [&](Completion& c) {
                                       return Hal::startRead(c, buf, capacity);
                                   });
        if (st != Status::Ok) return st;

        Size got = last_completion_.transferred();
        if (got > capacity) got = capacity;
        bytes_read = got;
        return Status::Ok;
    }

    /// Прочитать ровно capacity байт или вернуть Timeout.
    /// Удобно для протоколов с фиксированной длиной кадра.
    Status readExact(void* buf, Size capacity, uint32_t timeout_ms) noexcept {
        if (buf == nullptr || capacity == 0u) return Status::InvalidArg;

        auto* out = static_cast<uint8_t*>(buf);
        Size got = 0u;

        const uint64_t start = kernel::has_time() ? kernel::time_ms() : 0u;

        while (got < capacity) {
            Size chunk = 0u;
            uint32_t left = timeout_ms;
            if (timeout_ms != 0u && kernel::has_time()) {
                const uint32_t spent = static_cast<uint32_t>(kernel::time_ms() - start);
                if (spent >= timeout_ms) return Status::Timeout;
                left = timeout_ms - spent;
            }

            const Status st = read(out + got, capacity - got, chunk, left);
            if (st == Status::Timeout) return Status::Timeout;
            if (st != Status::Ok) return st;

            got += chunk;
            if (chunk == 0u) {
                // Неблокирующий HAL: уступаем и пробуем снова, иначе
                // получился бы busy-loop.
                kernel::yield();
            }
        }
        return Status::Ok;
    }

    /// Читать до терминатора или заполнения буфера.
    /// Возвращает количество байт БЕЗ терминатора; терминатор поглощается.
    Status readUntil(void* buf, Size capacity, Size& bytes_read,
                     char terminator, uint32_t timeout_ms) noexcept {
        bytes_read = 0u;
        if (buf == nullptr || capacity == 0u) return Status::InvalidArg;

        auto* out = static_cast<uint8_t*>(buf);
        const uint64_t start = kernel::has_time() ? kernel::time_ms() : 0u;

        for (;;) {
            if (bytes_read >= capacity) return Status::NoMemory;

            uint32_t left = timeout_ms;
            if (timeout_ms != 0u && kernel::has_time()) {
                const uint32_t spent = static_cast<uint32_t>(kernel::time_ms() - start);
                if (spent >= timeout_ms) return Status::Timeout;
                left = timeout_ms - spent;
            }

            Size got = 0u;
            const Status st = read(out + bytes_read, capacity - bytes_read, got, left);
            if (st == Status::Timeout) return Status::Timeout;
            if (st != Status::Ok) return st;

            for (Size i = 0; i < got; ++i) {
                if (static_cast<char>(out[bytes_read]) == terminator) {
                    return Status::Ok;      // терминатор не включаем
                }
                ++bytes_read;
            }

            if (got == 0u) kernel::yield();
        }
    }

    /// Прервать текущую передачу.
    Status cancel() noexcept {
        HalDefaultCancel<Hal>::cancel();
        return Status::Ok;
    }

    /// Последний результат завершения (статус и объём).
    const Completion& lastCompletion() const noexcept { return last_completion_; }

    // ─────────────────── Точки входа для обработчиков ───────────────────

    /// Вызывать из обработчика приёма (RX) интерфейса.
    /// Завершает операцию и будит задачу — без колбэков со стороны
    /// пользователя.
    bool onRxComplete(Size bytes) noexcept {
        return last_completion_.post_ok(bytes);
    }

    /// Вызывать из обработчика передачи (TX).
    bool onTxComplete(Size bytes) noexcept {
        return last_completion_.post_ok(bytes);
    }

    /// Вызывать из обработчика DMA (HT или TC).
    bool onDmaComplete(Size bytes) noexcept {
        return last_completion_.post_ok(bytes);
    }

    /// Вызывать из обработчика ошибки интерфейса.
    bool onError(Status st) noexcept {
        return last_completion_.post_error(st);
    }

    /// Объект завершения текущей операции (для Hal, который завершает
    /// операцию сам).
    Completion& pending() noexcept { return last_completion_; }

private:
    /// Общий каркас одной передачи: подготовка -> старт -> ожидание.
    template <typename StartFn>
    Status transfer(Size /*len*/, uint32_t timeout_ms, StartFn start) noexcept {
        const WaitPolicy policy = (timeout_ms == 0u)
                                ? WaitPolicy::Forever
                                : WaitPolicy::Timeout;

        last_completion_.reset(policy, timeout_ms, user_cb_, user_cb_arg_);

        // Для стратегии Polling ожидание обязано опрашивать интерфейс:
        // иначе драйвер опроса никогда не узнает, что передача завершена.
        if (Hal::strategy() == Strategy::Polling) {
            last_completion_.set_poll(&HalDefaultPoll<Hal>::poll, nullptr);
        }

        last_completion_.begin();

        const Status st = start(last_completion_);
        if (st != Status::Ok) {
            // Передача не началась — снимаем защёлку, чтобы объект
            // не остался «завершённым» для следующей операции.
            (void)last_completion_.post_error(st);
            return st;
        }

        // Ключевая точка: здесь задача уходит в сон, а не крутится.
        if (!last_completion_.wait()) return Status::Unavailable;

        return last_completion_.status();
    }

    /// Владелец объекта завершения: живёт столько же, сколько порт.
    Completion    last_completion_{};
    uint32_t      default_timeout_ms_ = DefaultTimeoutMs;
    DoneCallback  user_cb_            = nullptr;
    void*         user_cb_arg_        = nullptr;
};

} // namespace embed::async
