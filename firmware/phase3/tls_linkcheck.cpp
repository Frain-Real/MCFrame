// F4 link/size check: pull the bundled mbedTLS TLS client into a real image.
#include "os/net/tls_mbedtls.hpp"

int main() {
    static embed::net::MbedTlsProvider<1> provider;
    embed::net::TlsSession* session = nullptr;
    return (provider.create(session) == embed::Status::Ok && session != nullptr) ? 0 : 1;
}
