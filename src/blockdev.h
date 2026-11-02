/*
 * zimwiki - SD card block access with a small LRU block cache.
 *
 * Small reads (directory entries, pointer lists, FAT sectors) go
 * through 4 KiB cached blocks; large reads (compressed clusters) are
 * issued as multi-sector reads straight into the caller's buffer.
 */
#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include "zw.h"

#define BD_SECTOR        512
#define BD_BLOCK_SECTORS 8
#define BD_BLOCK         (BD_SECTOR * BD_BLOCK_SECTORS)
#define BD_MAX_BURST     128	/* sectors per read command (driver limit 255) */

/* provided by the platform: read `count` sectors starting at `lba` */
int plat_sector_read(uint32_t lba, void *buf, uint32_t count);

int bd_init(uint32_t cache_blocks);
void bd_shutdown(void);

/* read `len` bytes that are contiguous on the card, starting `offset`
 * bytes into sector `lba` (offset may exceed 512) */
int bd_read(uint32_t lba, uint32_t offset, void *buf, uint32_t len);

/* drop all cached blocks */
void bd_invalidate(void);

#endif
