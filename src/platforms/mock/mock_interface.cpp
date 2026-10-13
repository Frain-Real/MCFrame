// platforms/mock/mock_interface.cpp
#include "mock_interface.hpp"

#include "../../os/utilities.hpp"

embed::Status MockInterface::read(void* buffer, embed::Size size,
                                  embed::Size& bytesRead) noexcept
{
    // Тестовые данные: ровно то, что «пришло из UART».
    static const char kData[] = "read";
    constexpr embed::Size kDataSize = sizeof(kData) - 1u;

    if (buffer == nullptr || size == 0u) {
        bytesRead = 0u;
        return embed::Status::InvalidArg;   // прежде молча возвращался Ok
    }

    const embed::Size n = (size < kDataSize) ? size : kDataSize;
    (void)embed::utilities::memcpy_(buffer, kData, n);
    bytesRead = n;
    return embed::Status::Ok;
}
