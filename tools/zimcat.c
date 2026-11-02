/*
 * zimcat - test tool for the zimwiki ZIM reader
 *
 *   zimcat FILE                 header summary
 *   zimcat FILE PATH            print entry PATH (namespace/url or url)
 *   zimcat --dump FILE          one line per entry: idx ns mime len fnv1a url
 *                               (compared against libzim by the tests)
 *   zimcat --order FILE SEED    as --dump but random access order
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/zim.h"
#include "../host/hostio.h"

static uint32_t fnv1a(const uint8_t *p, uint32_t n)
{
	uint32_t h = 2166136261u;
	while (n--) {
		h ^= *p++;
		h *= 16777619u;
	}
	return h;
}

static int dump_entry(zim_t *z, uint32_t i)
{
	zim_dirent_t d;
	const uint8_t *p;
	uint32_t len = 0, h = 0;
	int r = zim_dirent(z, i, &d);
	if (r < 0) {
		printf("%u ERR dirent %d\n", i, r);
		return r;
	}
	if (d.mime == ZIM_MIME_REDIRECT) {
		printf("%u %c R %u - %s\n", i, d.ns, d.redirect, d.url);
		return 0;
	}
	if (d.cluster == ZIM_NONE) {
		printf("%u %c X - - %s\n", i, d.ns, d.url);
		return 0;
	}
	r = zim_blob(z, d.cluster, d.blob, &p, &len);
	if (r < 0) {
		printf("%u %c ERR blob %d %s\n", i, d.ns, r, d.url);
		return r;
	}
	h = fnv1a(p, len);
	printf("%u %c %s %u %08x %s\n", i, d.ns,
	       d.mime < z->nmime ? z->mime[d.mime] : "?", len, h, d.url);
	return 0;
}

int main(int argc, char **argv)
{
	host_files_t f;
	zim_t z;
	int r, a = 1, mode = 0;
	unsigned seed = 0;

	if (argc > 1 && strcmp(argv[1], "--dump") == 0) mode = 1, a = 2;
	if (argc > 1 && strcmp(argv[1], "--order") == 0) mode = 2, a = 2;
	if (argc <= a) {
		fprintf(stderr, "usage: zimcat [--dump|--order] FILE [PATH|SEED]\n");
		return 2;
	}
	if ((r = host_files_open(&f, argv[a])) < 0) {
		fprintf(stderr, "cannot open %s (%d)\n", argv[a], r);
		return 1;
	}
	if ((r = zim_open(&z, &f.io)) < 0) {
		fprintf(stderr, "not a ZIM file (%d)\n", r);
		return 1;
	}

	if (mode == 1) {
		uint32_t i;
		for (i = 0; i < z.entry_count; ++i)
			dump_entry(&z, i);
	} else if (mode == 2) {
		uint32_t i, *perm = malloc(sizeof(uint32_t) * z.entry_count);
		seed = argc > a + 1 ? (unsigned)atoi(argv[a + 1]) : 1;
		srand(seed);
		for (i = 0; i < z.entry_count; ++i)
			perm[i] = i;
		for (i = z.entry_count; i > 1; --i) {
			uint32_t j = (uint32_t)rand() % i, t = perm[i - 1];
			perm[i - 1] = perm[j];
			perm[j] = t;
		}
		for (i = 0; i < z.entry_count; ++i)
			dump_entry(&z, perm[i]);
		free(perm);
	} else if (argc > a + 1) {
		const char *path = argv[a + 1];
		uint32_t idx;
		char ns = z.content_ns;
		zim_dirent_t d;
		const uint8_t *p;
		uint32_t len;
		if (path[0] && path[1] == '/') {
			ns = path[0];
			path += 2;
		}
		if ((r = zim_find(&z, ns, path, &idx)) < 0 ||
		    (r = zim_follow(&z, idx, &idx, &d)) < 0 ||
		    (r = zim_blob(&z, d.cluster, d.blob, &p, &len)) < 0) {
			fprintf(stderr, "not found / error %d\n", r);
			return 1;
		}
		fwrite(p, 1, len, stdout);
	} else {
		char title[256];
		printf("version %u.%u entries %u clusters %u main %u ns %c mimes %u\n",
		       z.major, z.minor, z.entry_count, z.cluster_count,
		       z.main_page, z.content_ns, z.nmime);
		if (zim_metadata(&z, "Title", title, sizeof(title)) == 0)
			printf("title %s\n", title);
	}
	fprintf(stderr, "stats: reads=%u decompressed=%u started=%u resumed=%u hits=%u\n",
		zw_stats.sector_reads, zw_stats.bytes_decompressed,
		zw_stats.clusters_started, zw_stats.clusters_resumed,
		zw_stats.cluster_cache_hits);
	zim_close(&z);
	host_files_close(&f);
	return 0;
}
