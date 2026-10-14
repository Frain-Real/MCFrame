// platforms/mock/flash_mock.hpp
#pragma once
// ---------------------------------------------------------------------------
// МОДЕЛЬ ЭНЕРГОНЕЗАВИСИМОЙ ПАМЯТИ ДЛЯ MOCK-СБОРКИ.
//
// Зачем модель, а не «просто массив»: логика OTA опирается на СВОЙСТВА
// носителя, а не на факт записи. Проверять её на массиве, который принимает
// всё, — значит не проверить ничего. Модель воспроизводит именно то, на чём
// ошибаются:
//   * стирание переводит байты в 0xFF, и только после него запись осмысленна;
//   * программирование умеет ТОЛЬКО сбрасывать биты (1 -> 0): попытка
//     записать поверх нестёртого места возвращает отказ, как на железе;
//   * запись принимает только порции, кратные granularity (у встроенной Flash
//     это half-word, как у драйвера STM32): невыровненная запись отвергается —
//     ровно так же, как её отвергнет настоящее железо. Благодаря этому хостовый
//     тест ловит ошибку выравнивания в сервисе, а не «прощает» её;
//   * без снятия защиты (unlock) запись и стирание запрещены;
//   * можно ИНЪЕЦИРОВАТЬ отказ: «write вернул Ok, а в носителе не то»,
//     «erase не сработал». Именно эти состояния отличают честную проверку
//     прочитанного от доверия коду возврата, и именно они дают тесты.
//
// Модель используется ДВУМЯ путями:
//   * platform::FlashOps mock-платформы (встроенная Flash 128 КБ, страница 1 КБ);
//   * platform::Ops::ext_rom_* (внешняя память 64 КБ, страница 4 КБ).
// Обе — статические массивы: тесты не должны зависеть от кучи.
// ---------------------------------------------------------------------------
#include "../../os/iplatform.hpp"

namespace embed::mock_platform {

/// Модель энергонезависимой памяти: страницы, стирание, программирование.
class FlashModel {
public:
    /// lockable — есть ли у носителя защита от записи, которую надо снимать.
    /// У встроенной Flash STM32 она есть (CR.LOCK), у внешней микросхемы —
    /// нет: она не «заперта», просто её команды никто не отправил. Модель
    /// повторяет и это, иначе внешняя память на стенде вела бы себя строже,
    /// чем на плате.
    FlashModel(uint8_t* memory, uint32_t capacity,
               uint32_t page_size, uint32_t granularity,
               bool lockable = true) noexcept
        : mem_(memory), cap_(capacity), page_(page_size), gran_(granularity),
          lockable_(lockable), locked_(lockable) {}

    FlashModel(const FlashModel&)            = delete;
    FlashModel& operator=(const FlashModel&) = delete;

    // ── Поведение (то, что проверяют тесты) ──

    /// Сброс в исходное состояние: память стёрта, защита взведена, счётчики
    /// и инъекции отказов убраны.
    void reset() noexcept {
        for (uint32_t i = 0u; i < cap_; ++i) mem_[i] = 0xFFu;
        locked_          = lockable_;
        unlock_fails_    = false;
        erase_fails_     = false;
        erase_fails_from = 0u;
        program_fails_   = false;
        write_drops_     = false;
        write_drops_from = 0u;
        erases_          = 0u;
        programs_        = 0u;
        bytes_written_   = 0u;
        refused_         = 0u;
    }

    /// Стереть всё (модель «новая микросхема»).
    void fill_erased() noexcept {
        for (uint32_t i = 0u; i < cap_; ++i) mem_[i] = 0xFFu;
    }

    uint8_t* buffer() noexcept { return mem_; }

    /// Заполнить «прошивкой» — имитация уже записанного образа.
    void preload(uint32_t offset, const void* data, uint32_t len) noexcept {
        const auto* p = static_cast<const uint8_t*>(data);
        for (uint32_t i = 0u; i < len && (offset + i) < cap_; ++i) {
            mem_[offset + i] = p[i];
        }
    }

    // ── Инъекции отказов ──
    bool unlock_fails_  = false;   ///< unlock() не срабатывает (Option Bytes)
    bool erase_fails_   = false;   ///< стирание возвращает отказ
    /// С какого смещения отказ стирания включается. Нужно, чтобы проверять
    /// отказ ИМЕННО при стирании слота: страницы метаданных при этом стираются
    /// честно, и видно, снята ли метка до стирания.
    uint32_t erase_fails_from = 0u;
    bool program_fails_ = false;   ///< программирование возвращает отказ
    bool write_drops_   = false;   ///< запись «проходит», но в память не попадает
    /// С какого смещения включается подмена записи. Нужно, чтобы проверять
    /// ИМЕННО образ: метаданные при этом пишутся честно, и отказ виден на
    /// проверке чтением образа, а не на записи состояния.
    uint32_t write_drops_from = 0u;

    // ── Счётчики (диагностика тестов) ──
    uint32_t erases_        = 0u;
    uint32_t programs_      = 0u;
    uint32_t bytes_written_ = 0u;
    uint32_t refused_       = 0u;   ///< отказов из-за записи в нестёртое место

    bool locked() const noexcept { return locked_; }
    uint32_t capacity() const noexcept { return cap_; }
    uint32_t page_size() const noexcept { return page_; }
    uint32_t granularity() const noexcept { return gran_; }

    // ── Операции (контракт platform::FlashOps, но в терминах модели) ──

    bool unlock() noexcept {
        if (unlock_fails_) return false;
        locked_ = false;
        return true;
    }

    void lock() noexcept { locked_ = true; }

    bool erase(uint32_t offset, uint32_t len) noexcept {
        if (locked_) return false;
        if (erase_fails_ && offset >= erase_fails_from) return false;
        if (page_ == 0u || (offset % page_) != 0u || (len % page_) != 0u) return false;
        if (offset > cap_ || (cap_ - offset) < len) return false;

        for (uint32_t i = 0u; i < len; ++i) mem_[offset + i] = 0xFFu;
        ++erases_;
        return true;
    }

    bool program(uint32_t offset, const void* data, uint32_t len) noexcept {
        if (locked_ || program_fails_) return false;
        if (data == nullptr && len != 0u) return false;
        if (offset > cap_ || (cap_ - offset) < len) return false;

        const auto* src = static_cast<const uint8_t*>(data);
        // Грануля программирования: носитель принимает только целые порции.
        // Выравнивание — забота вызывающего (сервис OTA пишет кратными
        // порциями и добивает последнюю значением 0xFF).
        if (gran_ > 1u && ((offset % gran_) != 0u || (len % gran_) != 0u)) {
            return false;
        }
        // Сначала проверяем ВСЁ, потом пишем: на железе частичная запись
        // оставляет «полузаписанное» состояние, и модель не должна делать
        // вид, что этого не бывает.
        for (uint32_t i = 0u; i < len; ++i) {
            if ((mem_[offset + i] & src[i]) != src[i]) {
                ++refused_;
                return false;      // бит 0 -> 1 не записывается
            }
        }
        // «Ok», но в памяти ничего не изменилось — так ведёт себя изнашиваемая
        // Flash: контроллер отчитался, ячейка не переключилась.
        if (write_drops_ && offset >= write_drops_from) return true;

        for (uint32_t i = 0u; i < len; ++i) mem_[offset + i] &= src[i];
        ++programs_;
        bytes_written_ += len;
        return true;
    }

    bool read(uint32_t offset, void* data, uint32_t len) noexcept {
        if (data == nullptr && len != 0u) return false;
        if (offset > cap_ || (cap_ - offset) < len) return false;
        auto* dst = static_cast<uint8_t*>(data);
        for (uint32_t i = 0u; i < len; ++i) dst[i] = mem_[offset + i];
        return true;
    }

    /// Испортить байт «в носителе» — проверка того, что сервис читает
    /// прочитанное, а не верит своему счётчику.
    void poke(uint32_t offset, uint8_t value) noexcept {
        if (offset < cap_) mem_[offset] = value;
    }

private:
    uint8_t* mem_  = nullptr;
    uint32_t cap_  = 0u;
    uint32_t page_ = 0u;
    uint32_t gran_ = 1u;
    bool     lockable_ = true;
    bool     locked_ = true;
};

/// Встроенная Flash mock-платформы: 128 КБ, страница 1 КБ, half-word —
/// ровно как у STM32F103RB. Размеры совпадают с железом намеренно: прогон на
/// хосте должен упираться в те же пределы, что и плата.
FlashModel& flash_model() noexcept;

/// Внешняя память mock-платформы (то, что на STM32 отдал бы SPI-flash):
/// 64 КБ, стирание страницами по 4 КБ, без защиты от записи (её снимать не
/// нужно — в отличие от встроенной Flash у STM32).
FlashModel& ext_rom_model() noexcept;

/// Таблицы для platform::ops().
const platform::FlashOps& mock_flash_ops() noexcept;

} // namespace embed::mock_platform
