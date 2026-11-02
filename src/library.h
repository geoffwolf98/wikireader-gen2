/*
 * zimwiki - a "library" is one ZIM on the card:
 *
 *   prepared:  NAME.IDX + NAME.Z00, NAME.Z01, ...  (from zimprep)
 *   plain:     NAME.ZIM  (no search index, single file up to 4 GiB)
 *
 * either in the top folder or in a folder of their own (one level down,
 * e.g. MATHS/MATHS.IDX).  Its history (NAME.HIS) and file map (NAME.EXT)
 * are kept next to its files; ZWSTATE.DAT stays in the top folder.
 *
 * This module finds them, maps ZIM offsets onto the chunk files, and
 * runs title searches either through the index or, for plain ZIMs,
 * through the ZIM's own title list.
 */
#ifndef LIBRARY_H
#define LIBRARY_H

#include "zim.h"
#include "index.h"
#include "fat.h"

#define LIB_MAX       32	/* wikis on one card; only one is open at a time */
#define LIB_MAX_CHUNK 100

typedef struct lib_info {
	char name[9];		/* file base name, e.g. "WIKI" */
	char dir[9];		/* folder on the card, "" for the top folder */
	char key[9];		/* unique id: the folder name, or name if at the top */
	char title[128];
	uint32_t articles;
	int prepared;
} lib_info_t;

typedef struct lib_hit {
	char title[256];
	uint32_t entry;		/* final (non redirect) ZIM entry */
	uint32_t pos;		/* position in the search order */
	uint8_t redirect;
} lib_hit_t;

typedef struct library {
	lib_info_t info;
	fat_vol_t *vol;
	fat_file_t idx_file;
	zw_io_t idx_io;
	idx_t idx;
	fat_file_t chunk[LIB_MAX_CHUNK];
	uint8_t chunk_ready[LIB_MAX_CHUNK];
	uint32_t nchunks, shift;
	zw_io_t zim_io;
	zim_t zim;
	int ext_dirty;		/* extent cache needs saving */
	uint32_t total;		/* records in search order */
} library_t;

int lib_mount(fat_vol_t *v);
/* path of one of the library's files: "DIR/NAME.EXT" or "NAME.EXT" */
void lib_path(const lib_info_t *li, const char *ext, char *out);
int lib_scan(fat_vol_t *v, lib_info_t *out, int max);
int lib_open(library_t *L, fat_vol_t *v, const lib_info_t *info);
void lib_close(library_t *L);
void lib_save_extents(library_t *L);

/* first position at or after the (unfolded) query */
int lib_seek(library_t *L, const char *query, uint32_t *pos);
/* up to max hits starting at pos */
int lib_results(library_t *L, uint32_t pos, lib_hit_t *out, int max);
/* random article */
int lib_random(library_t *L, uint32_t seed, uint32_t *entry);

#endif
