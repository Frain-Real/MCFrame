#include <mock_interface.hpp>
#include <iostream>

int main(void)
{
  MockInterface inf;
  char str[6] = {};
  embed::Size byteread = 0;
  inf.read(str, sizeof(str) - 1, byteread);
  str[byteread] = '\0';
  std::cout << str << std::endl;
  return 0;
}
