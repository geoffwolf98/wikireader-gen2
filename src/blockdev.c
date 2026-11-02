/*
 * zimwiki - SD card block access with a small LRU block cache.
 */
#include "blockdev.h"

typedef struct bd_entry {
	uint32_t block;		/* lba / BD_BLOCK_SECTORS, or 0xffffffff */
	uint32_t last_use;
	int16_t hnext;		/* hash chain */
} bd_entry_t;

#define BD_HASH 256

static bd_entry_t *entries;
static uint8_t *data;
static uint32_t nblocks;
static uint32_t clock;
static int16_t hash[BD_HASH];

static int sector_read(uint32_t lba, void *buf, uint32_t count)
{
	zw_stats.sector_reads++;
	zw_stats.sectors_read += count;
	return plat_sector_read(lba, buf, count);
}

int bd_init(uint32_t cache_blocks)
{
	bd_shutdown();
	if (cache_blocks < 4)
		cache_blocks = 4;
	if (cache_blocks > 30000)
		cache_blocks = 30000;
	entries = zw_alloc(sizeof(bd_entry_t) * cache_blocks);
	data = zw_alloc((size_t)BD_BLOCK * cache_blocks);
	if (!entries || !data) {
		bd_shutdown();
		return ZW_ENOMEM;
	}
	nblocks = cache_blocks;
	bd_invalidate();
	return ZW_OK;
}

void bd_shutdown(void)
{
	zw_free(entries);
	zw_free(data);
	entries = NULL;
	data = NULL;
	nblocks = 0;
}

void bd_invalidate(void)
{
	uint32_t i;
	for (i = 0; i < BD_HASH; ++i)
		hash[i] = -1;
	for (i = 0; i < nblocks; ++i) {
		entries[i].block = 0xffffffffu;
		entries[i].last_use = 0;
		entries[i].hnext = -1;
	}
}

static void hash_remove(uint32_t i)
{
	int16_t *p = &hash[entries[i].block % BD_HASH];
	while (*p >= 0) {
		if ((uint32_t)*p == i) {
			*p = entries[i].hnext;
			return;
		}
		p = &entries[*p].hnext;
	}
}

static const uint8_t *get_block(uint32_t block)
{
	int16_t i = hash[block % BD_HASH];
	uint32_t victim = 0, k;

	while (i >= 0) {
		if (entries[i].block == block) {
			entries[i].last_use = ++clock;
			zw_stats.cache_hits++;
			return data + (size_t)i * BD_BLOCK;
		}
		i = entries[i].hnext;
	}
	for (k = 0; k < nblocks; ++k) {
		if (entries[k].block == 0xffffffffu) {
			victim = k;
			break;
		}
		if (entries[k].last_use < entries[victim].last_use)
			victim = k;
	}
	if (entries[victim].block != 0xffffffffu)
		hash_remove(victim);
	entries[victim].block = 0xffffffffu;
	if (sector_read(block * BD_BLOCK_SECTORS, data + (size_t)victim * BD_BLOCK,
			BD_BLOCK_SECTORS) != 0)
		return NULL;
	entries[victim].block = block;
	entries[victim].last_use = ++clock;
	entries[victim].hnext = hash[block % BD_HASH];
	hash[block % BD_HASH] = (int16_t)victim;
	return data + (size_t)victim * BD_BLOCK;
}

int bd_read(uint32_t lba, uint32_t offset, void *buf, uint32_t len)
{
	uint8_t *out = buf;

	lba += offset / BD_SECTOR;
	offset %= BD_SECTOR;

	while (len) {
		uint32_t block = lba / BD_BLOCK_SECTORS;
		uint32_t in_block = (lba % BD_BLOCK_SECTORS) * BD_SECTOR + offset;

		/* sector aligned and at least two blocks long: read directly */
		if (offset == 0 && len >= 2 * BD_BLOCK) {
			uint32_t count = len / BD_SECTOR;
			if (count > BD_MAX_BURST)
				count = BD_MAX_BURST;
			if (sector_read(lba, out, count) != 0)
				return ZW_EIO;
			out += count * BD_SECTOR;
			len -= count * BD_SECTOR;
			lba += count;
			continue;
		}
		{
			const uint8_t *b = get_block(block);
			uint32_t n = BD_BLOCK - in_block;
			if (!b)
				return ZW_EIO;
			if (n > len)
				n = len;
			memcpy(out, b + in_block, n);
			out += n;
			len -= n;
			offset += n;
			lba += offset / BD_SECTOR;
			offset %= BD_SECTOR;
		}
	}
	return ZW_OK;
}
