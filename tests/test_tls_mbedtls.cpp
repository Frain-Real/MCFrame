// tests/test_tls_mbedtls.cpp — compile/link smoke for bundled mbedTLS.
#include "os/net/tls_mbedtls.hpp"
#include <cstdio>

int main() {
    embed::net::MbedTlsProvider<2> provider;
    embed::net::TlsSession* s = nullptr;
    const embed::Status st = provider.create(s);
    std::printf("mbedtls provider=%d session=%p\n", static_cast<int>(st), (void*)s);
    return (st == embed::Status::Ok && s != nullptr) ? 0 : 1;
}
