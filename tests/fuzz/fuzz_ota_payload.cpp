#include "os/ota/ota_protocol.hpp"
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace embed::ota::proto;
    if (data == nullptr || size == 0u) return 0;
    Info info{};
    Start start{};
    End end{};
    (void)decode_info(data, size, info);
    (void)decode_start(data, size, start);
    (void)decode_end(data, size, end);
    if (size >= 4u) (void)get_u32(data);
    return 0;
}
