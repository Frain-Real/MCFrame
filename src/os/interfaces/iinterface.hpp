#pragma once
#include "../common.hpp"

namespace embed {
  struct InterfaceDescriptor
  {
      const char* name;
      const char* type;

      const char* version;

      bool supportsDma;
      bool supportsInterrupts;
      bool supportsPowerControl;
  };
  template<typename Impl>
  class IHwInterface
  {
    protected:
      struct Config{};
      static InterfaceDescriptor desc;
      Callback_h txCb = nullptr;
      Callback_h rxCb = nullptr;
      ~IHwInterface() = default;
    public:
      enum class AsyncMode {
          Polling = 0,
          Async,
      };
      AsyncMode mode = AsyncMode::Polling;

      Status open() { return static_cast<Impl*>(this)->open(); };
      Status close() { return static_cast<Impl*>(this)->close(); };

      Status read(void* buffer, Size size, Size& bytesRead) { return static_cast<Impl*>(this)->read(buffer, size, bytesRead); };
      Status write(const void* data, Size size, Size& bytesWritten) { return static_cast<Impl*>(this)->write(data, size, bytesWritten); };

      Status ioctl(unsigned request, void* arg) { return static_cast<Impl*>(this)->ioctl(request, arg); };

      const InterfaceDescriptor& descriptor() const { return static_cast<Impl*>(this)->descriptor(); };

      Status setRxCallback(Callback_h cb, void* arg) { return static_cast<Impl*>(this)->setRxCallback(cb, arg); };
      Status setTxCallback(Callback_h cb, void* arg) { return static_cast<Impl*>(this)->setTxCallback(cb, arg); };
      Status setAsyncMode(AsyncMode mode) { return static_cast<Impl*>(this)->setAsyncMode(mode); };
  };
}