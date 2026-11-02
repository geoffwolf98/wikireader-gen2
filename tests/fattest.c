/*
 * fattest IMAGE FILE HOSTFILE [FILE HOSTFILE ...]
 * Reads FILE from the FAT image with the zimwiki FAT layer (random
 * offsets and lengths) and compares with HOSTFILE.
 */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/fat.h"
#include "../src/blockdev.h"
#include "../host/hostio.h"

static FILE *img;

int plat_sector_read(uint32_t lba, void *buf, uint32_t count)
{
	if (fseeko(img, (off_t)lba * 512, SEEK_SET) != 0)
		return -1;
	return fread(buf, 512, count, img) == count ? 0 : -1;
}

static int list_cb(void *arg, const fat_file_t *f)
{
	printf("  %-12s %10u attr %02x\n", f->name, f->size, f->attr);
	return 0;
}

int main(int argc, char **argv)
{
	fat_vol_t v;
	int i, r, fails = 0;

	if (argc < 2)
		return 2;
	img = fopen(argv[1], "rb");
	if (!img || bd_init(64) < 0 || (r = fat_mount(&v)) < 0) {
		printf("mount failed %d\n", img ? r : -99);
		return 1;
	}
	printf("FAT%d part_lba %u clusters %u spc %u\n", v.fat32 ? 32 : 16,
	       v.part_lba, v.cluster_count, 1u << v.spc_shift);
	fat_list(&v, "", list_cb, NULL);

	for (i = 2; i + 1 < argc; i += 2) {
		fat_file_t f;
		FILE *h = fopen(argv[i + 1], "rb");
		uint8_t *want, *got, *saved;
		uint32_t size, k, t;
		if ((r = fat_lookup(&v, argv[i], &f)) < 0 || !h) {
			printf("FAIL lookup %s %d\n", argv[i], r);
			fails++;
			continue;
		}
		zw_stats.sector_reads = 0;
		if ((r = fat_build_extents(&v, &f, NULL, NULL)) < 0) {
			printf("FAIL extents %s %d\n", argv[i], r);
			fails++;
			continue;
		}
		t = zw_stats.sector_reads;
		/* round trip through the extent cache format */
		saved = malloc(fat_extents_size(&f));
		fat_extents_save(&f, saved);
		if ((r = fat_extents_load(&v, &f, saved, fat_extents_size(&f))) < 0) {
			printf("FAIL extents reload %d\n", r);
			fails++;
		}
		size = f.size;
		want = malloc(size + 1);
		got = malloc(size + 1);
		if (fread(want, 1, size, h) != size) {
			printf("FAIL host read\n");
			return 1;
		}
		memset(got, 0xaa, size);
		if ((r = fat_read(&v, &f, 0, got, size)) < 0 || memcmp(got, want, size)) {
			printf("FAIL full read %s %d\n", argv[i], r);
			fails++;
		}
		srand(1);
		for (k = 0; k < 3000 && size; ++k) {
			uint32_t off = (uint32_t)rand() % size;
			uint32_t len = (uint32_t)rand() % ((k & 7) == 0 ? 200000 : 700);
			if (len > size - off)
				len = size - off;
			if ((r = fat_read(&v, &f, off, got, len)) < 0 || memcmp(got, want + off, len)) {
				printf("FAIL random read %s off %u len %u (%d)\n", argv[i], off, len, r);
				fails++;
				break;
			}
		}
		printf("ok   %-12s %u bytes, %u extents, chain walk %u reads\n",
		       f.name, size, f.n_ext, t);
		fat_close(&f);
		free(want);
		free(got);
		free(saved);
		fclose(h);
	}
	return fails ? 1 : 0;
}
