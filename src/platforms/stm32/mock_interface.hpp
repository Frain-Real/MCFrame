#pragma once

#include "../../os/interfaces/iinterface.hpp"

class MockInterface : public embed::IHwInterface<MockInterface>
{
public:
  MockInterface(){}
  MockInterface(embed::Callback_h txCb, embed::Callback_h rxCb){
    this->txCb = txCb;
    this->rxCb = rxCb;
  }
	embed::Status read(void* buffer, embed::Size size, embed::Size& bytesRead) ;
  embed::Status open() {return embed::Status::Ok;}
  embed::Status close() {return embed::Status::Ok;}
  embed::Status write(const void* data, embed::Size size, embed::Size& bytesWritten) {return embed::Status::Ok;}

  embed::Status ioctl(unsigned request, void* arg) {return embed::Status::Ok;}

  const embed::InterfaceDescriptor& descriptor() const {}

  embed::Status setRxCallback(embed::Callback_h cb, void* arg) {return embed::Status::Ok;}
  embed::Status setTxCallback(embed::Callback_h cb, void* arg) {return embed::Status::Ok;}
  embed::Status setAsyncMode(AsyncMode mode) {return embed::Status::Ok;}
};
