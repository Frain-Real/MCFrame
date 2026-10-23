// Compile-time check: provider must compile against the target mbedTLS config.
#include "os/net/tls_mbedtls.hpp"

template class embed::net::MbedTlsProvider<2>;
