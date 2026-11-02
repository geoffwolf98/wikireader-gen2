/*
 * zimwiki - minimal ZIM file reader
 *
 * Reads ZIM v5 (old namespaces A/I/-/M/X) and v6 (new namespaces
 * C/M/W/X), zstd and xz clusters, normal and extended (64-bit offset)
 * clusters.
 *
 * Compressed clusters are decoded into a small LRU cache.  Decoding
 * stops as soon as the requested blob has been produced and resumes
 * from the same point if a later blob of that cluster is wanted, so
 * opening an article costs, on average, half a cluster of decoding
 * instead of a whole one, and neighbouring articles are often free.
 */
#ifndef ZIM_H
#define ZIM_H

#include "zw.h"

#define ZIM_MAX_URL     256	/* bytes incl. NUL, longer names are truncated */
#define ZIM_MAX_TITLE   256
#define ZIM_MAX_MIME    64
#define ZIM_CACHE_SLOTS 3
#define ZIM_NONE        0xffffffffu

enum {
	ZIM_MIME_REDIRECT   = 0xffff,
	ZIM_MIME_LINKTARGET = 0xfffe,
	ZIM_MIME_DELETED    = 0xfffd,
};

typedef struct zim_dirent {
	uint16_t mime;		/* index into zim_t.mime[], or ZIM_MIME_* */
	char ns;
	uint32_t cluster;	/* content entries */
	uint32_t blob;
	uint32_t redirect;	/* redirect entries: target entry index */
	char url[ZIM_MAX_URL];
	char title[ZIM_MAX_TITLE];	/* never empty: falls back to url */
} zim_dirent_t;

typedef struct zim_slot {
	uint32_t cluster;	/* ZIM_NONE when free */
	uint32_t last_use;
	uint8_t comp;		/* 4 = xz, 5 = zstd */
	uint8_t ext;		/* 1 when blob offsets are 64 bit */
	uint8_t done;		/* whole cluster decoded */
	uint64_t in_start;	/* compressed data range in the file */
	uint64_t in_pos;
	uint64_t in_end;
	uint8_t *in_buf;
	uint32_t in_len, in_off;
	uint8_t *out;
	uint32_t out_cap, out_len;
	void *zstd;		/* ZSTD_DStream * while partially decoded */
	void *xz;		/* struct xz_dec * while partially decoded */
} zim_slot_t;

typedef struct zim {
	zw_io_t io;
	uint16_t major, minor;
	uint8_t uuid[16];
	uint32_t entry_count;
	uint32_t cluster_count;
	uint32_t main_page;	/* entry index or ZIM_NONE */
	uint64_t url_ptr_pos;
	uint64_t title_ptr_pos;
	uint64_t cluster_ptr_pos;
	uint64_t mime_list_pos;
	uint64_t checksum_pos;
	char *mime_buf;
	const char *mime[ZIM_MAX_MIME];
	uint16_t nmime;
	uint16_t html_mime;	/* index of text/html or 0xffff */
	char content_ns;	/* 'C' (new) or 'A' (old) */
	zim_slot_t slot[ZIM_CACHE_SLOTS];
	uint32_t clock;
	uint8_t *raw;		/* blobs from uncompressed clusters */
	uint32_t raw_cap;
} zim_t;

int zim_open(zim_t *z, const zw_io_t *io);
void zim_close(zim_t *z);

/* directory entry by URL-order index */
int zim_dirent(zim_t *z, uint32_t idx, zim_dirent_t *d);

/* exact lookup by namespace + url (binary search on the URL list) */
int zim_find(zim_t *z, char ns, const char *url, uint32_t *idx);

/* follow redirects (at most 8 hops); *d receives the final entry */
int zim_follow(zim_t *z, uint32_t idx, uint32_t *final_idx, zim_dirent_t *d);

/* get a blob.  The pointer stays valid until the next zim_blob() call. */
int zim_blob(zim_t *z, uint32_t cluster, uint32_t blob,
	     const uint8_t **data, uint32_t *len);

/* small metadata value such as "Title" or "Language" into out */
int zim_metadata(zim_t *z, const char *name, char *out, size_t cap);

/* release cached clusters (e.g. to free memory) */
void zim_flush_cache(zim_t *z);

#endif
