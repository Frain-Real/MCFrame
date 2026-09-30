#include "mock_interface.hpp"
#include "../../os/utilities.hpp"

embed::Status MockInterface::read(void* buffer, embed::Size size, embed::Size& bytesRead)
{
	char data[] = "read";
    embed::Size dataSize = sizeof(data) - 1;

    if (buffer == nullptr || size == 0) {
        bytesRead = 0;
        return embed::Status::Ok; // или ошибка, если так принято в интерфейсе
    }
    embed::Size n = (size < dataSize) ? size : dataSize;
    embed::utilities::fast_memcpy(buffer, data, static_cast<embed::Size>(n));
    bytesRead = n;
	return embed::Status::Ok;
}
