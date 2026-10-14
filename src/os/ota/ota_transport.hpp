// os/ota/ota_transport.hpp
#pragma once
// ---------------------------------------------------------------------------
// ПО КАКОМУ ИНТЕРФЕЙСУ ПРИНИМАЕТСЯ ПРОШИВКА.
//
// Протокол (ota_protocol.hpp) работает с ПОТОКОМ БАЙТ — ему безразлично, UART
// это, USB CDC, SPI в режиме ведомого или буфер в памяти. Здесь собраны
// адаптеры, которые приводят разные интерфейсы к одному контракту Transport:
//
//   * StreamTransport<Port> — любой линейный порт с read/write и таймаутом
//     (async::AsyncPort<Hal>, драйвер IUart<Impl>, USB CDC, «своя» шина).
//     Это основной путь: UART/RS-485/USB — потоки байт по своей природе;
//   * MemoryTransport — буфер в памяти: стенд, тесты и случай «образ уже
//     принят другим каналом»;
//   * NullTransport — интерфейса нет вовсе (Iface::None): устройство умеет
//     только проверить и активировать то, что уже лежит в хранилище.
//
// ЧЕГО ЗДЕСЬ НЕТ И ПОЧЕМУ
//   * собственного драйвера I2C-slave. I2C — не поток: обмен инициирует
//     ведущий, и «кадр» укладывается в транзакцию (запрос), а ответ уходит в
//     СЛЕДУЮЩУЮ транзакцию (чтение). Написать это «в общем виде» нельзя, не
//     соврав про таймауты и про то, кто кого ждёт: нужен драйвер режима
//     ведомого с буфером под кадр. Готовый такой транспорт пишется поверх
//     II2c<Impl> как наследник Transport (образец — StreamTransport) и
//     сообщает iface() == Iface::I2c; весь протокол выше от этого не меняется;
//   * повторов и таймаутов на уровне транспорта: за надёжность отвечает
//     ПРОТОКОЛ (next_offset + повтор кадра), а не транспорт. Транспорт обязан
//     только честно вернуть Timeout/ошибку — сервис решит, что с этим делать.
// ---------------------------------------------------------------------------

#include "ota_protocol.hpp"

namespace embed::ota {

namespace detail {

/// Определение вида порта: у async::AsyncPort запись — write(data, len, ms),
/// у драйвера интерфейса (IUart) — write(data, len, written, timeout).
///
/// Различие принципиальное: линейный API драйвера обязан СКАЗАТЬ, сколько
/// байт ушло (иначе цикл дозаписи в AsyncPort зациклился бы), а у порта это
/// уже учтено внутри. Обе формы поддержаны, поэтому один и тот же транспорт
/// работает и с портом, и с драйвером.
template <typename Port, typename = void>
struct HasWrite3 : std::false_type {};

template <typename Port>
struct HasWrite3<Port,
                 std::void_t<decltype(std::declval<Port&>().write(
                     std::declval<const void*>(), Size{0u}, uint32_t{0u}))>>
    : std::true_type {};

/// 0 в настройке означает «ждать бесконечно» — так это понимают и порт, и
/// драйвер интерфейса (Timeout vs WaitForever).
inline Timeout as_timeout(uint32_t timeout_ms) noexcept {
    return (timeout_ms == 0u) ? WaitForever : static_cast<Timeout>(timeout_ms);
}

} // namespace detail

// ─────────────────────────────── Интерфейс ─────────────────────────────────

class Transport {
public:
    virtual ~Transport() = default;

    Transport(const Transport&)            = delete;
    Transport& operator=(const Transport&) = delete;

    /// Открыть интерфейс (для UART — configure+open, для памяти — ничего).
    virtual Status begin() noexcept { return Status::Ok; }
    /// Закрыть интерфейс. Обязан быть безопасным при повторном вызове.
    virtual void end() noexcept {}

    /// Прочитать ХОТЯ БЫ ОДИН байт за timeout_ms.
    ///
    /// Возврат: Ok (bytes_read > 0) | Timeout | другая ошибка интерфейса.
    /// Ok с bytes_read == 0 допустим для неблокирующих драйверов и означает
    /// «данных пока нет» — сервис это учтёт и уступит процессор.
    virtual Status read(void* buffer, Size capacity, Size& bytes_read,
                        uint32_t timeout_ms) noexcept = 0;

    /// Записать len байт за timeout_ms. Ok — ушли ВСЕ байты
    /// (частичная запись означает, что ответ потерян, и хост повторит запрос).
    virtual Status write(const void* data, Size len, uint32_t timeout_ms) noexcept = 0;

    /// Предел полезной нагрузки одного кадра для ЭТОГО интерфейса.
    ///
    /// UART с DMA не имеет предела кроме буфера приёмника, а у SPI/I2C кадр
    /// обязан уложиться в буфер драйвера: если интерфейс сообщает меньше
    /// общего kMaxPayload, сервис честно объявит хосту СВОЙ предел в INFO —
    /// иначе хост присылал бы кадры, которые некуда положить.
    /// 0 — предела нет.
    virtual uint16_t max_payload() const noexcept { return 0u; }

    /// Вид интерфейса (уходит в INFO и в отчёты).
    virtual proto::Iface iface() const noexcept = 0;

    /// Имя для логов/отчётов.
    virtual const char* name() const noexcept = 0;

protected:
    Transport() = default;
};

// ─────────────────────── Транспорт над линейным портом ─────────────────────

/// Адаптер любого порта с линейным API (см. os/async/async_port.hpp и
/// os/interfaces/iuart.hpp).
///
/// Требования к Port:
///     Status read(void* buf, Size cap, Size& got, uint32_t timeout_ms);
///     Status write(const void* data, Size len, uint32_t timeout_ms);
/// Больше ничего: стратегия (Polling/Async/Dma) — дело порта, и для OTA она
/// невидима. Поэтому один и тот же код работает и с mock-драйвером на стенде,
/// и с USART1 на плате.
template <typename Port>
class StreamTransport : public Transport {
public:
    StreamTransport(Port& port, proto::Iface iface,
                    uint16_t max_payload = 0u,
                    const char* name = "stream") noexcept
        : port_(&port), iface_(iface), max_payload_(max_payload), name_(name) {}

    /// Задать предел кадра интерфейса (например размер буфера DMA).
    void set_max_payload(uint16_t max_payload) noexcept { max_payload_ = max_payload; }

    Status begin() noexcept override { return Status::Ok; }
    void   end() noexcept override {}

    Status read(void* buffer, Size capacity, Size& bytes_read,
                uint32_t timeout_ms) noexcept override {
        return port_->read(buffer, capacity, bytes_read, detail::as_timeout(timeout_ms));
    }

    Status write(const void* data, Size len, uint32_t timeout_ms) noexcept override {
        if constexpr (detail::HasWrite3<Port>::value) {
            return port_->write(data, len, timeout_ms);
        } else {
            Size written = 0u;
            const Status st = port_->write(data, len, written,
                                          detail::as_timeout(timeout_ms));
            if (st != Status::Ok) return st;
            // Драйвер интерфейса может отчитаться о ЧАСТИЧНОЙ записи: для
            // протокола «запрос-ответ» это означает потерянный ответ — хост
            // повторит запрос, а мы обязаны не делать вид, что ответ ушёл.
            return (written == len) ? Status::Ok : Status::Unavailable;
        }
    }

    uint16_t max_payload() const noexcept override { return max_payload_; }
    proto::Iface iface() const noexcept override { return iface_; }
    const char* name() const noexcept override { return name_; }

private:
    Port*          port_ = nullptr;
    proto::Iface   iface_ = proto::Iface::Custom;
    uint16_t       max_payload_ = 0u;
    const char*    name_ = "stream";
};

/// Псевдоним для случая «понятно, что это UART» — им пользуется приложение.
template <typename Port>
using UartTransport = StreamTransport<Port>;

// ──────────────────────── Транспорт в памяти (буфер) ───────────────────────
//
// Две стороны в одном объекте: то, что «пришло от хоста» (вход), и то, что
// «ушло хосту» (выход). Применение:
//   * тесты: хост-отправитель кладёт кадры в feed(), читает ответы из take();
//   * стенд: образ принимается из другого канала, а протокол прогоняется
//     локально;
//   * «прошивка уже в ОЗУ»: загрузчик принял образ и передаёт его сервису.

class MemoryTransport : public Transport {
public:
    /// Ёмкость входного и выходного буферов.
#ifndef MCFRAME_OTA_MEMORY_BUFFER
#define MCFRAME_OTA_MEMORY_BUFFER 2048u
#endif
    static constexpr Size kBuffer = MCFRAME_OTA_MEMORY_BUFFER;

    MemoryTransport() noexcept { reset(); }

    /// Полный сброс: буферы пусты, статистика обнулена.
    void reset() noexcept {
        in_len_ = in_read_ = 0u;
        out_len_ = 0u;
        reads_ = writes_ = 0u;
        closed_ = false;
    }

    // ── Сторона «хост» (нужна тестам и стенду) ──

    /// Положить данные «от хоста» устройству. Возврат — сколько влезло.
    Size feed(const void* data, Size len) noexcept {
        const auto* p = static_cast<const uint8_t*>(data);
        Size placed = 0u;
        while (placed < len) {
            compact();
            if (in_len_ >= kBuffer) break;
            in_[in_len_++] = p[placed++];
        }
        return placed;
    }

    /// Сколько байт ответов ждёт «хост».
    Size pending_out() const noexcept { return out_len_; }

    /// Забрать ответы устройства.
    Size take(void* buffer, Size capacity) noexcept {
        auto* dst = static_cast<uint8_t*>(buffer);
        const Size n = (out_len_ < capacity) ? out_len_ : capacity;
        for (Size i = 0u; i < n; ++i) dst[i] = out_[i];
        for (Size i = n; i < out_len_; ++i) out_[i - n] = out_[i];
        out_len_ -= n;
        return n;
    }

    /// Заглянуть в очередной ответ, не забирая его.
    const uint8_t* peek_out() const noexcept { return out_; }
    Size peek_out_len() const noexcept { return out_len_; }

    // ── Контракт Transport ──

    Status begin() noexcept override {
        closed_ = false;
        return Status::Ok;
    }
    void end() noexcept override { closed_ = true; }

    Status read(void* buffer, Size capacity, Size& bytes_read,
                uint32_t /*timeout_ms*/) noexcept override {
        ++reads_;
        bytes_read = 0u;
        if (buffer == nullptr || capacity == 0u) return Status::InvalidArg;

        const Size have = in_len_ - in_read_;
        if (have == 0u) return Status::Timeout;

        const Size n = (have < capacity) ? have : capacity;
        auto* dst = static_cast<uint8_t*>(buffer);
        for (Size i = 0u; i < n; ++i) dst[i] = in_[in_read_ + i];
        in_read_ += n;
        if (in_read_ == in_len_) in_read_ = in_len_ = 0u;
        bytes_read = n;
        return Status::Ok;
    }

    Status write(const void* data, Size len, uint32_t /*timeout_ms*/) noexcept override {
        ++writes_;
        if (data == nullptr && len != 0u) return Status::InvalidArg;
        if (closed_) return Status::Unavailable;

        const auto* src = static_cast<const uint8_t*>(data);
        for (Size i = 0u; i < len; ++i) {
            if (out_len_ >= kBuffer) return Status::NoMemory;   // ответ потерян
            out_[out_len_++] = src[i];
        }
        return Status::Ok;
    }

    uint16_t max_payload() const noexcept override {
        // Кадр обязан уложиться в буфер транспорта: иначе выданный ответ
        // пришлось бы резать, а протокол такого не допускает.
        const Size limit = kBuffer - (proto::kFrameOverhead + proto::kResponseSize);
        return (limit < proto::kMaxPayload) ? static_cast<uint16_t>(limit)
                                            : proto::kMaxPayload;
    }

    proto::Iface iface() const noexcept override { return proto::Iface::Memory; }
    const char* name() const noexcept override { return "memory"; }

    uint32_t reads() const noexcept { return reads_; }
    uint32_t writes() const noexcept { return writes_; }

private:
    void compact() noexcept {
        if (in_read_ == 0u) return;
        const Size rest = in_len_ - in_read_;
        for (Size i = 0u; i < rest; ++i) in_[i] = in_[in_read_ + i];
        in_len_ = rest;
        in_read_ = 0u;
    }

    uint8_t in_[kBuffer]{};
    Size    in_len_ = 0u;
    Size    in_read_ = 0u;
    uint8_t out_[kBuffer]{};
    Size    out_len_ = 0u;
    uint32_t reads_ = 0u;
    uint32_t writes_ = 0u;
    bool     closed_ = false;
};

// ────────────────────────── Транспорта нет (Iface::None) ───────────────────

/// Режим «интерфейса нет»: образ попадает в хранилище другим путём
/// (программатор, SPI-хост, второй загрузчик), а устройство умеет его
/// проверить (VERIFY) и активировать (ACTIVATE).
///
/// Это не заглушка «на всякий случай»: на плате без внешнего канала связи
/// (или когда канал занят другим протоколом) это единственный рабочий режим,
/// и он обязан быть явным, а не выражаться нулевым указателем.

class NullTransport : public Transport {
public:
    Status read(void* /*buffer*/, Size /*capacity*/, Size& bytes_read,
                uint32_t /*timeout_ms*/) noexcept override {
        bytes_read = 0u;
        return Status::NotSupported;
    }
    Status write(const void* /*data*/, Size /*len*/, uint32_t /*timeout_ms*/) noexcept override {
        return Status::NotSupported;
    }
    uint16_t max_payload() const noexcept override { return 0u; }
    proto::Iface iface() const noexcept override { return proto::Iface::None; }
    const char* name() const noexcept override { return "none"; }
};

} // namespace embed::ota
