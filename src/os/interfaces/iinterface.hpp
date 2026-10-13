// iinterface.hpp
#pragma once
#include "../common.hpp"
#include "../async/async_kernel.hpp"   // время и уступка для линейных методов

namespace embed {

  /// Дескриптор аппаратного интерфейса.
  /// Заполняется наследником как static constexpr kDescriptor.
  struct InterfaceDescriptor
  {
      const char* name;
      const char* type;
      const char* version;

      bool supportsDma;
      bool supportsInterrupts;
      bool supportsPowerControl;
  };

  /// CRTP-база для аппаратных интерфейсов.
  ///
  /// Исправлено относительно прежней версии:
  ///  * деструктор был protected и НЕ virtual — удаление через
  ///    IHwInterface<T>* давало UB; теперь public virtual;
  ///  * descriptor() каждый наследник обязан был реализовать сам, и в
  ///    mock-реализациях он остался без return (UB + -Werror=return-type).
  ///    Теперь дескриптор один на наследника (kDescriptor) и берётся из базы;
  ///  * Config{} в базе конфликтовал с Config у IUart/II2c — удалён;
  ///  * методы помечены noexcept: в прошивке исключений нет.
  template<typename Impl>
  class IHwInterface
  {
    public:
      enum class AsyncMode {
          Polling = 0,
          Async,
      };

      virtual ~IHwInterface() = default;

      IHwInterface(const IHwInterface&)            = delete;
      IHwInterface& operator=(const IHwInterface&) = delete;

      AsyncMode mode = AsyncMode::Polling;

      Status open() noexcept  { return self().open(); }
      Status close() noexcept { return self().close(); }

      Status read(void* buffer, Size size, Size& bytesRead) noexcept {
          return self().read(buffer, size, bytesRead);
      }
      Status write(const void* data, Size size, Size& bytesWritten) noexcept {
          return self().write(data, size, bytesWritten);
      }

      Status ioctl(unsigned request, void* arg) noexcept {
          return self().ioctl(request, arg);
      }

      /// Дескриптор берётся у наследника: Impl::kDescriptor.
      const InterfaceDescriptor& descriptor() const noexcept {
          return Impl::kDescriptor;
      }

      Status setRxCallback(Callback_h cb, void* arg) noexcept {
          return self().setRxCallback(cb, arg);
      }
      Status setTxCallback(Callback_h cb, void* arg) noexcept {
          return self().setTxCallback(cb, arg);
      }
      Status setAsyncMode(AsyncMode m) noexcept {
          mode = m;
          return Status::Ok;
      }

    protected:
      IHwInterface() = default;

      // ── Помощники для линейных методов интерфейсов ──
      // Вынесены в базу, чтобы каждый драйвер не дублировал их.

      static uint64_t timerNow() noexcept { return async::kernel::time_ms(); }
      static bool     timerAvailable() noexcept { return async::kernel::has_time(); }
      static void     yieldCpu() noexcept { async::kernel::yield(); }

      Impl&       self()       noexcept { return *static_cast<Impl*>(this); }
      const Impl& self() const noexcept { return *static_cast<const Impl*>(this); }
  };

}
