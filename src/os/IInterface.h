#pragma once

class IInterface;

using Callback_h = void (*)(IInterface *interf);

class IInterface 
{
  private:
  Callback_h cb_func;
  public:
    virtual ~IInterface() = default;
    
    IInterface(Callback_h call) : cb_func(call) {}

    virtual char* read() = 0;
    virtual bool write(char* str) = 0;
    virtual char* write_read(char* str) = 0;
};
