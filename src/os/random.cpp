// random.cpp
// ---------------------------------------------------------------------------
// Переопределение rand()/srand() из libc.
//
// Прежде эти функции были ОПРЕДЕЛЕНЫ в utilities.hpp без inline/static:
// включение заголовка в два .cpp давало multiple definition, а состояние
// (_legacy_rand_ctx) было static, то есть своё в каждой единице трансляции.
//
// Теперь определение одно на программу, состояние — здесь же.
// ---------------------------------------------------------------------------
#include "utilities.hpp"

namespace {
embed::utilities::os_rand_ctx_t g_rand_ctx{0u};
bool g_rand_inited = false;

inline void ensure_init() noexcept {
    if (!g_rand_inited) {
        (void)embed::utilities::os_rand_init(&g_rand_ctx, 1u);
        g_rand_inited = true;
    }
}
} // namespace

extern "C" int rand(void) {
    ensure_init();
    // Обрезаем до 31 бита: RAND_MAX в libc равен 2^31-1.
    return static_cast<int>(embed::utilities::os_rand(&g_rand_ctx) & 0x7FFFFFFFu);
}

extern "C" void srand(unsigned int seed) {
    (void)embed::utilities::os_rand_init(&g_rand_ctx, static_cast<uint32_t>(seed));
    g_rand_inited = true;
}
