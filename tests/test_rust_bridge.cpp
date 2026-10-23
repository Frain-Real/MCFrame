// tests/test_rust_bridge.cpp — host smoke-test C ABI Rust <-> C++.
#include "os/rust/rust_bridge.hpp"
#include <cstdio>

extern "C" unsigned int mcframe_rust_selfcheck(void);

int main() {
    const unsigned int v = mcframe_rust_selfcheck();
    std::printf("rust selfcheck=0x%08x available=%d\n", v,
                static_cast<int>(embed::rust::available()));
    return (v != 0u && embed::rust::available()) ? 0 : 1;
}
