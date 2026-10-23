#include "os/net/http.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0u || size > 512u) return 0;
    char url[513]{};
    std::memcpy(url, data, size);
    embed::net::http::Url parsed{};
    (void)embed::net::http::parse_url(url, parsed);
    return 0;
}
