// platforms/mock/mock_interface.hpp
// ---------------------------------------------------------------------------
// Тестовый UART-интерфейс для сборки на ПК.
//
// Исправлено относительно прежней версии:
//  * descriptor() возвращал ссылку без return — UB и падение под
//    -Werror=return-type. Теперь дескриптор статический, берётся из базы;
//  * конструктор с параметрами txCb/rxCb перекрывал одноимённые члены
//    (-Wshadow и путаница) — параметры переименованы;
//  * метод configure() из IUart не был реализован вовсе.
//
// Второй, побайтово идентичный файл лежал в platforms/stm32/ — это была
// копия, не имеющая отношения к STM32. Он удалён.
// ---------------------------------------------------------------------------
#pragma once

#include "../../os/interfaces/iuart.hpp"

class MockInterface : public embed::IUart<MockInterface>
{
public:
    /// Дескриптор интерфейса (единственный на класс).
    static constexpr embed::InterfaceDescriptor kDescriptor{
        /* name                  */ "MockUart",
        /* type                  */ "uart",
        /* version               */ "1.0.0",
        /* supportsDma           */ false,
        /* supportsInterrupts    */ false,
        /* supportsPowerControl  */ false,
    };

    MockInterface() noexcept = default;

    MockInterface(embed::Callback_h tx_cb, embed::Callback_h rx_cb) noexcept {
        txCb_ = tx_cb;
        rxCb_ = rx_cb;
    }

    embed::Status read(void* buffer, embed::Size size, embed::Size& bytesRead) noexcept;

    embed::Status open() noexcept {
        opened_ = true;
        return embed::Status::Ok;
    }

    embed::Status close() noexcept {
        opened_ = false;
        return embed::Status::Ok;
    }

    embed::Status write(const void* data, embed::Size size,
                        embed::Size& bytesWritten) noexcept {
        (void)data;
        bytesWritten = size;         // приёмник всегда «принимает» всё
        return embed::Status::Ok;
    }

    embed::Status ioctl(unsigned request, void* arg) noexcept {
        (void)request;
        (void)arg;
        return embed::Status::Ok;
    }

    embed::Status configure(const Config& config) noexcept {
        config_ = config;
        return embed::Status::Ok;
    }

    embed::Status setRxCallback(embed::Callback_h cb, void* arg) noexcept {
        rxCb_ = cb;
        rxArg_ = arg;
        return embed::Status::Ok;
    }

    embed::Status setTxCallback(embed::Callback_h cb, void* arg) noexcept {
        txCb_ = cb;
        txArg_ = arg;
        return embed::Status::Ok;
    }

    bool isOpen() const noexcept { return opened_; }
    const Config& config() const noexcept { return config_; }

private:
    Config config_{};
    bool   opened_ = false;

    embed::Callback_h txCb_  = nullptr;
    embed::Callback_h rxCb_  = nullptr;
    void*             txArg_ = nullptr;
    void*             rxArg_ = nullptr;
};
