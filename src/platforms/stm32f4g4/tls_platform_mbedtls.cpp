// tls_platform_mbedtls.cpp — platform hooks for bundled mbedTLS on F4/G4.
#include "stm32m4_min.h"
#include "../../os/config/time_config.hpp"

#if MCFRAME_WITH_MBEDTLS

#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "psa/crypto.h"
#include "psa/crypto_driver_random.h"

extern "C" mbedtls_ms_time_t mbedtls_ms_time(void) {
    return static_cast<mbedtls_ms_time_t>(embed::config::OsTimer::NowMs());
}

extern "C" mbedtls_time_t mcframe_mbedtls_time(mbedtls_time_t* out) {
    const uint64_t epoch = embed::config::OsTimer::NowEpochSeconds();
    const mbedtls_time_t t = static_cast<mbedtls_time_t>(epoch);
    if (out != nullptr) *out = t;
    return t;
}

namespace {
struct TimeHook {
    TimeHook() noexcept { (void)mbedtls_platform_set_time(&mcframe_mbedtls_time); }
};
TimeHook g_time_hook;
} // namespace

extern "C" int mbedtls_platform_get_entropy(psa_driver_get_entropy_flags_t flags,
                                             size_t* estimate_bits,
                                             unsigned char* output,
                                             size_t output_size) {
    (void)flags;
    if (estimate_bits == nullptr || output == nullptr || output_size == 0u) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
    RNG->CR |= RNG_CR_RNGEN;
    for (size_t i = 0u; i < output_size;) {
        uint32_t guard = 0u;
        while ((RNG->SR & RNG_SR_DRDY) == 0u) {
            if (++guard > 100000u || (RNG->SR & (RNG_SR_SEIS | RNG_SR_CEIS)) != 0u) {
                RNG->CR &= ~RNG_CR_RNGEN;
                return PSA_ERROR_INSUFFICIENT_ENTROPY;
            }
        }
        const uint32_t v = RNG->DR;
        const size_t n = (output_size - i < 4u) ? output_size - i : 4u;
        for (size_t k = 0u; k < n; ++k) output[i + k] = static_cast<unsigned char>(v >> (8u * k));
        i += n;
    }
    RNG->CR &= ~RNG_CR_RNGEN;
    *estimate_bits = output_size * 8u;
    return PSA_SUCCESS;
}

#endif
