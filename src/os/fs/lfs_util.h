/*
 * Minimal lfs_util.h for MCFrame.
 *
 * littlefs upstream normally ships a larger lfs_util.h with optional
 * malloc/trace hooks. In MCFrame LFS_NO_MALLOC is defined and all caches are
 * supplied by the caller; logging is routed to the application only if it
 * defines LFS_DEBUG/LFS_ERROR itself.
 */
#ifndef LFS_UTIL_H
#define LFS_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LFS_NO_MALLOC
static inline void *lfs_malloc(size_t size) { return malloc(size); }
static inline void lfs_free(void *ptr) { free(ptr); }
#else
#define lfs_malloc(size) NULL
#define lfs_free(ptr) ((void)0)
#endif

static inline uint32_t lfs_min(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}
static inline uint32_t lfs_max(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}
static inline uint32_t lfs_aligndown(uint32_t off, uint32_t alignment) {
    return off - (off % alignment);
}
static inline uint32_t lfs_alignup(uint32_t off, uint32_t alignment) {
    return lfs_aligndown(off + alignment - 1u, alignment);
}

/// CRC-32, littlefs variant (reflected table, poly 0x04C11DB7).
static inline uint32_t lfs_crc(uint32_t crc, const void* buffer, size_t size) {
    static const uint32_t rtable[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    const uint8_t* data = (const uint8_t*)buffer;
    for (size_t i = 0; i < size; ++i) {
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xFu];
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xFu];
    }
    return crc;
}

static inline uint32_t lfs_fromle32(uint32_t v) {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
#else
    return v;
#endif
}
static inline uint32_t lfs_tole32(uint32_t v) { return lfs_fromle32(v); }
static inline uint32_t lfs_frombe32(uint32_t v) {
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
#else
    return v;
#endif
}
static inline uint32_t lfs_tobe32(uint32_t v) { return lfs_frombe32(v); }

static inline int lfs_scmp(uint32_t a, uint32_t b) {
    return (int)(unsigned)(a - b);
}
static inline uint32_t lfs_popc(uint32_t v) {
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}
static inline uint32_t lfs_npw2(uint32_t v) {
    uint32_t n = 1u;
    while (n < v) n <<= 1;
    return n;
}
static inline uint32_t lfs_ctz(uint32_t v) {
    uint32_t n = 0u;
    while ((v & 1u) == 0u) { v >>= 1; ++n; }
    return n;
}

#ifndef LFS_WARN
#define LFS_WARN(...) ((void)0)
#endif

#ifndef LFS_ASSERT
#define LFS_ASSERT(test) ((void)0)
#endif

#ifndef LFS_DEBUG
#define LFS_DEBUG(...) ((void)0)
#endif

#ifndef LFS_ERROR
#define LFS_ERROR(...) ((void)0)
#endif

#ifndef LFS_TRACE
#define LFS_TRACE(...) ((void)0)
#endif

#ifdef LFS_THREADSAFE
#ifndef LFS_LOCK
#define LFS_LOCK(cfg) ((void)(cfg))
#endif
#ifndef LFS_UNLOCK
#define LFS_UNLOCK(cfg) ((void)(cfg))
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif
