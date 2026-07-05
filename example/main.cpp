#include "../src/platforms/MopInterface.h"
#include <iostream>

void inf_cb(IInterface *interface)
{
  
}

int main(void)
{
  MopInterface inf(inf_cb);
  char *str = inf.read();
  std::cout << str << std::endl;
  return 0;
}
