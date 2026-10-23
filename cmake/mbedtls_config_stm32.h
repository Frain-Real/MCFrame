/* MCFrame STM32: mbedTLS user config.
 * Include the stock config, then override only what a bare-metal target
 * needs. Kept as a header so mbedTLS' own config checks still run. */
#ifndef MCFRAME_MBEDTLS_CONFIG_STM32_H
#define MCFRAME_MBEDTLS_CONFIG_STM32_H

#include "mbedtls/mbedtls_config.h"

/* Use OsTimer/RTC for mbedtls_ms_time() instead of POSIX clock_gettime(). */
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_TIME_ALT

/* Bare metal: no filesystem, no PSA ITS file backend. */
#undef MBEDTLS_FS_IO
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C

#endif
