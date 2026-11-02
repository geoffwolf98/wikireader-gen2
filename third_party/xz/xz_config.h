/* xz-embedded configuration for zimwiki (host and WikiReader) */
#ifndef XZ_CONFIG_H
#define XZ_CONFIG_H

#define XZ_DEC_DYNALLOC
/* ZIM xz clusters use CRC32/CRC64 checks; skip verification of
 * unknown/unsupported check types instead of failing */
#define XZ_DEC_ANY_CHECK

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "xz.h"

void *zw_alloc(size_t size);
void zw_free(void *p);

#define kmalloc(size, flags) zw_alloc(size)
#define kfree(ptr) zw_free(ptr)
#define vmalloc(size) zw_alloc(size)
#define vfree(ptr) zw_free(ptr)

#define memeq(a, b, size) (memcmp(a, b, size) == 0)
#define memzero(buf, size) memset(buf, 0, size)

#ifndef min
#	define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#define min_t(type, x, y) min(x, y)

#ifndef fallthrough
#	define fallthrough do { } while (0)
#endif

#ifndef __always_inline
#	define __always_inline inline
#endif

static inline uint32_t get_unaligned_le32(const uint8_t *buf)
{
	return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
	       ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static inline uint32_t get_unaligned_be32(const uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
	       ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static inline void put_unaligned_le32(uint32_t val, uint8_t *buf)
{
	buf[0] = (uint8_t)val;
	buf[1] = (uint8_t)(val >> 8);
	buf[2] = (uint8_t)(val >> 16);
	buf[3] = (uint8_t)(val >> 24);
}

static inline void put_unaligned_be32(uint32_t val, uint8_t *buf)
{
	buf[0] = (uint8_t)(val >> 24);
	buf[1] = (uint8_t)(val >> 16);
	buf[2] = (uint8_t)(val >> 8);
	buf[3] = (uint8_t)val;
}

#ifndef get_le32
#	define get_le32 get_unaligned_le32
#endif

#endif
