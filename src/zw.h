/*
 * zimwiki - ZIM reader for the Openmoko WikiReader
 * Common definitions shared by every module.
 *
 * Written to compile with both a modern host compiler and the
 * WikiReader's gcc 3.3.2 (C33) toolchain, so: gnu99 subset only,
 * no 64-bit division in hot paths, no VLAs, no _Static_assert.
 */
#ifndef ZW_H
#define ZW_H

/* release number: shown on the search screen and by zimprep --version */
#define ZW_VERSION "1.0.41"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define ZW_MIN(a, b) ((a) < (b) ? (a) : (b))
#define ZW_MAX(a, b) ((a) > (b) ? (a) : (b))
#define ZW_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* error codes: 0 = ok, negative = failure */
enum {
	ZW_OK = 0,
	ZW_EIO = -1,		/* storage read failed */
	ZW_ENOMEM = -2,
	ZW_EFORMAT = -3,	/* corrupt or unsupported data */
	ZW_ENOTFOUND = -4,
	ZW_ERANGE = -5,
	ZW_EUNSUPPORTED = -6,
};

/* memory (implemented by the platform layer) */
void *zw_alloc(size_t size);
void *zw_calloc(size_t size);
void zw_free(void *p);
void *zw_realloc(void *p, size_t old_size, size_t new_size);

/* little-endian readers (ZIM is little-endian, C33 is too, but the
 * data is unaligned so read bytewise) */
static inline uint16_t zw_rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t zw_rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t zw_rd64(const uint8_t *p)
{
	return (uint64_t)zw_rd32(p) | ((uint64_t)zw_rd32(p + 4) << 32);
}
static inline void zw_wr16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void zw_wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* random access reader: everything above the storage layer reads
 * through one of these (FAT32 chunk set on the device, stdio files on
 * the host tools). */
typedef struct zw_io {
	int (*read)(void *ctx, uint64_t offset, void *buf, uint32_t len);
	void *ctx;
	uint64_t size;
} zw_io_t;

static inline int zw_io_read(const zw_io_t *io, uint64_t off, void *buf, uint32_t len)
{
	if (off > io->size || len > io->size - off)
		return ZW_ERANGE;
	return io->read(io->ctx, off, buf, len);
}

/* performance counters, shown by the simulator and the debug screen */
typedef struct zw_stats {
	uint32_t sector_reads;		/* read commands issued to the card */
	uint32_t sectors_read;		/* 512 byte sectors transferred */
	uint32_t cache_hits;
	uint32_t bytes_decompressed;
	uint32_t clusters_started;
	uint32_t clusters_resumed;
	uint32_t cluster_cache_hits;
} zw_stats_t;
extern zw_stats_t zw_stats;

/* long running loops report progress now and then.  The app uses it for
 * the "Opening" bar and to feed the device's watchdog (which resets the
 * unit after 20 s without a kernel call).  NULL in the PC tools.
 * stage: 0 = other, 1 = decompressing, 2 = reading HTML, 3 = layout */
extern void (*zw_progress)(int stage, uint32_t done, uint32_t total);
#define ZW_PROGRESS(st, d, t) do { if (zw_progress) zw_progress((st), (d), (t)); } while (0)
#define ZW_KEEPALIVE() ZW_PROGRESS(0, 0, 0)

/* debug log (platform) */
void zw_log(const char *fmt, ...)
#if defined(__GNUC__)
	__attribute__((format(printf, 1, 2)))
#endif
	;

#endif
