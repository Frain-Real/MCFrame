#pragma once

#include "../os/IInterface.h"

class MopInterface : public IInterface
{
public:
  MopInterface(Callback_h call) : IInterface(call){}
	char* read() override;
  char* write_read(char* str) override {return str;}
  bool write(char* str) override {return true;} 
};
