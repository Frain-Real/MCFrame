# Shell over USB и TCP

## USB CDC

```cpp
usb::cdc::Acm acm;
usb::cdc::AcmConsole port(acm, usb_hal);
shell::Console console;
port.init(console);
// цикл: usb_hal.poll(); acm.on_out(...); port.poll();
```

## TCP

```cpp
shell::Console console;
shell::TcpServer server(board::sockets(), console);
server.begin(2323);
// цикл: server.poll();
```

`TcpServer` использует `Socket::listen/accept`; на C3 они реализованы через
lwIP. На W5500 listen/accept пока нет — используйте уже установленный сокет с
`shell::TcpConsole`, либо добавьте LISTEN/ACCEPT в `W5500Socket`.
