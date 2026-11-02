/*
 * zimwiki - read-only FAT16/FAT32 access for large files.
 *
 * The WikiReader's Tiny FatFs follows the FAT chain from the start of
 * the file on every backwards seek, which is painfully slow on multi-GB
 * files.  Here each file's cluster chain is turned into a short list of
 * extents (contiguous cluster runs) once, after which any offset maps
 * to a card sector in O(log extents) and large reads become single
 * multi-sector commands.  Extent lists can be saved and reloaded so the
 * chain walk only happens the first time a file is seen.
 */
#ifndef FAT_H
#define FAT_H

#include "zw.h"

typedef struct fat_vol {
	uint32_t part_lba;
	uint32_t fat_lba;
	uint32_t fat_sectors;
	uint32_t data_lba;
	uint32_t root_lba;	/* FAT16 fixed root directory */
	uint32_t root_sectors;
	uint32_t root_cluster;	/* FAT32 */
	uint32_t cluster_count;
	uint8_t spc_shift;	/* log2(sectors per cluster) */
	uint8_t fat32;
} fat_vol_t;

typedef struct fat_extent {
	uint32_t file_cluster;	/* first file-relative cluster of the run */
	uint32_t disk_cluster;
	uint32_t count;
} fat_extent_t;

typedef struct fat_file {
	char name[13];		/* "NAME.EXT" */
	uint8_t attr;
	uint32_t size;
	uint32_t first_cluster;
	uint32_t stamp;		/* write date/time from the directory entry */
	fat_extent_t *ext;
	uint32_t n_ext;
} fat_file_t;

int fat_mount(fat_vol_t *v);

/* look up "NAME.EXT" or "DIR/NAME.EXT" (8.3 names, any case) */
int fat_lookup(fat_vol_t *v, const char *path, fat_file_t *f);

/* call cb for each file in directory `dir` ("" = root); cb returns
 * non-zero to stop */
typedef int (*fat_dir_cb)(void *arg, const fat_file_t *f);
int fat_list(fat_vol_t *v, const char *dir, fat_dir_cb cb, void *arg);

/* walk the cluster chain and build f->ext.  progress (may be NULL) is
 * called now and then with clusters done / total. */
typedef void (*fat_progress_cb)(void *arg, uint32_t done, uint32_t total);
int fat_build_extents(fat_vol_t *v, fat_file_t *f, fat_progress_cb cb, void *arg);

/* serialise / restore extents (format: count, then 3 words each) */
uint32_t fat_extents_size(const fat_file_t *f);
void fat_extents_save(const fat_file_t *f, uint8_t *out);
int fat_extents_load(fat_vol_t *v, fat_file_t *f, const uint8_t *in, uint32_t len);

int fat_read(fat_vol_t *v, const fat_file_t *f, uint32_t off, void *buf, uint32_t len);
void fat_close(fat_file_t *f);

#endif
