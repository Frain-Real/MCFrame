// os/net/http.cpp
#include "http.hpp"

namespace embed::net::http {
namespace {
bool starts_with(const char* s, const char* p) noexcept {
    while (*p != '\0') { if (*s++ != *p++) return false; }
    return true;
}
bool copy_span(char* dst, Size cap, const char* begin, const char* end) noexcept {
    const Size n = static_cast<Size>(end - begin);
    if (n == 0u || n + 1u > cap) return false;
    for (Size i = 0u; i < n; ++i) dst[i] = begin[i];
    dst[n] = '\0';
    return true;
}
} // namespace

bool parse_url(const char* url, Url& out) noexcept {
    if (url == nullptr) return false;
    const char* p = url;
    if (starts_with(p, "https://")) { out.tls = true; p += 8; }
    else if (starts_with(p, "http://")) { out.tls = false; p += 7; }
    else return false;

    const char* host_begin = p;
    while (*p != '\0' && *p != ':' && *p != '/') ++p;
    if (!copy_span(out.host, sizeof(out.host), host_begin, p)) return false;

    out.port = 80u;
    if (*p == ':') {
        ++p;
        uint32_t port = 0u;
        bool any = false;
        while (*p >= '0' && *p <= '9') {
            port = port * 10u + static_cast<uint32_t>(*p - '0');
            if (port > 65535u) return false;
            any = true;
            ++p;
        }
        if (!any || port == 0u) return false;
        out.port = static_cast<uint16_t>(port);
    }

    if (*p == '\0') {
        out.path[0] = '/';
        out.path[1] = '\0';
        return true;
    }
    if (*p != '/') return false;
    const char* path_begin = p;
    while (*p != '\0') ++p;
    return copy_span(out.path, sizeof(out.path), path_begin, p);
}

} // namespace embed::net::http
