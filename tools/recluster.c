/*
 * recluster - rewrite a ZIM with small clusters (zimprep --recluster)
 *
 * libzim packs about 2 MiB of articles into each compressed cluster,
 * so reading one article means decompressing, on average, a megabyte
 * of its neighbours.  On a 48 MHz CPU that is most of the time it
 * takes to open a page.  This writes the same ZIM (same entries in the
 * same order, same titles, redirects and metadata, same UUID) with
 * clusters of about TARGET KiB instead, straight into NAME.Z00,
 * NAME.Z01, ...  The result is still a valid ZIM that Kiwix can open.
 */
#define _FILE_OFFSET_BITS 64
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zstd.h>

#include "../src/zim.h"
#include "recluster.h"

/* ------------------------------------------------------------------ */
/* MD5 (RFC 1321), for the ZIM checksum */

typedef struct md5 {
	uint32_t a, b, c, d;
	uint64_t len;
	uint8_t buf[64];
	uint32_t n;
} md5_t;

static const uint32_t md5_k[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t md5_r[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(md5_t *m, const uint8_t *p)
{
	uint32_t w[16], a = m->a, b = m->b, c = m->c, d = m->d, i;
	for (i = 0; i < 16; ++i)
		w[i] = zw_rd32(p + i * 4);
	for (i = 0; i < 64; ++i) {
		uint32_t f, g, t;
		if (i < 16) { f = (b & c) | (~b & d); g = i; }
		else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
		else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
		else { f = c ^ (b | ~d); g = (7 * i) & 15; }
		t = d;
		d = c;
		c = b;
		f += a + md5_k[i] + w[g];
		b += (f << md5_r[i]) | (f >> (32 - md5_r[i]));
		a = t;
	}
	m->a += a; m->b += b; m->c += c; m->d += d;
}

static void md5_init(md5_t *m)
{
	memset(m, 0, sizeof(*m));
	m->a = 0x67452301; m->b = 0xefcdab89; m->c = 0x98badcfe; m->d = 0x10325476;
}

static void md5_update(md5_t *m, const uint8_t *p, size_t n)
{
	m->len += n;
	while (n) {
		uint32_t k = 64 - m->n;
		if (k > n)
			k = (uint32_t)n;
		memcpy(m->buf + m->n, p, k);
		m->n += k;
		p += k;
		n -= k;
		if (m->n == 64) {
			md5_block(m, m->buf);
			m->n = 0;
		}
	}
}

static void md5_final(md5_t *m, uint8_t out[16])
{
	uint64_t bits = m->len * 8;
	uint8_t pad = 0x80, z = 0, l[8];
	int i;
	md5_update(m, &pad, 1);
	while (m->n != 56)
		md5_update(m, &z, 1);
	for (i = 0; i < 8; ++i)
		l[i] = (uint8_t)(bits >> (8 * i));
	md5_update(m, l, 8);
	zw_wr32(out, m->a); zw_wr32(out + 4, m->b); zw_wr32(out + 8, m->c); zw_wr32(out + 12, m->d);
}

/* ------------------------------------------------------------------ */
/* output split over NAME.Z00, NAME.Z01, ... */

typedef struct cw {
	char base[4096];
	uint32_t shift;
	FILE *f[100];
	uint64_t size;
} cw_t;

static FILE *cw_file(cw_t *w, uint32_t c)
{
	char path[4200];
	if (c >= 100) {
		fprintf(stderr, "zimprep: output needs more than 100 chunks\n");
		exit(1);
	}
	if (!w->f[c]) {
		snprintf(path, sizeof(path), "%s.Z%02u", w->base, c);
		w->f[c] = fopen(path, "w+b");
		if (!w->f[c]) {
			perror(path);
			exit(1);
		}
	}
	return w->f[c];
}

static void cw_write(cw_t *w, uint64_t off, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len) {
		uint32_t c = (uint32_t)(off >> w->shift);
		uint64_t in = off - ((uint64_t)c << w->shift);
		uint64_t room = ((uint64_t)1 << w->shift) - in;
		size_t n = len < room ? len : (size_t)room;
		FILE *f = cw_file(w, c);
		if (fseeko(f, (off_t)in, SEEK_SET) != 0 || fwrite(p, 1, n, f) != n) {
			perror("write");
			exit(1);
		}
		p += n;
		off += n;
		len -= n;
	}
	if (off > w->size)
		w->size = off;
}

static void cw_read(cw_t *w, uint64_t off, void *data, size_t len)
{
	uint8_t *p = data;
	while (len) {
		uint32_t c = (uint32_t)(off >> w->shift);
		uint64_t in = off - ((uint64_t)c << w->shift);
		uint64_t room = ((uint64_t)1 << w->shift) - in;
		size_t n = len < room ? len : (size_t)room;
		FILE *f = cw_file(w, c);
		fflush(f);
		if (fseeko(f, (off_t)in, SEEK_SET) != 0 || fread(p, 1, n, f) != n) {
			perror("read back");
			exit(1);
		}
		p += n;
		off += n;
		len -= n;
	}
}

/* ------------------------------------------------------------------ */
/* parallel cluster compression */

#define BIG_CLUSTER (16u << 20)

typedef struct job {
	uint8_t *raw;		/* offsets table + blobs */
	size_t raw_len;
	uint8_t *out;
	size_t out_len;
} job_t;

typedef struct pool {
	job_t *jobs;
	int njobs, next, level;
	pthread_mutex_t mu;
} pool_t;

static void *worker(void *arg)
{
	pool_t *p = arg;
	ZSTD_CCtx *cc = ZSTD_createCCtx();
	ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, p->level);
	ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 0);
	for (;;) {
		job_t *j;
		size_t cap, r;
		pthread_mutex_lock(&p->mu);
		j = p->next < p->njobs ? &p->jobs[p->next++] : NULL;
		pthread_mutex_unlock(&p->mu);
		if (!j)
			break;
		if (j->raw_len > BIG_CLUSTER) {
			/* already compressed media and the like: level 19 on
			 * one core would take hours for little gain */
			j->out = malloc(j->raw_len + 1);
			j->out[0] = 1;	/* uncompressed cluster */
			memcpy(j->out + 1, j->raw, j->raw_len);
			j->out_len = j->raw_len + 1;
			continue;
		}
		cap = ZSTD_compressBound(j->raw_len) + 1;
		j->out = malloc(cap);
		j->out[0] = 5;	/* zstd cluster */
		r = ZSTD_compress2(cc, j->out + 1, cap - 1, j->raw, j->raw_len);
		if (ZSTD_isError(r)) {
			fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(r));
			exit(1);
		}
		j->out_len = r + 1;
	}
	ZSTD_freeCCtx(cc);
	return NULL;
}

/* ------------------------------------------------------------------ */

/* Kiwix full text / title search indexes (Xapian databases, one entry
 * each, often many GB): the WikiReader has its own title index */
static int is_search_index(const zim_dirent_t *d)
{
	if (d->ns == 'X' && (!strncmp(d->url, "fulltext/", 9) || !strncmp(d->url, "title/", 6)))
		return 1;
	if (d->ns == 'Z')		/* old ZIMs: Z/fulltextIndex/xapian */
		return 1;
	return 0;
}

static int mime_is(const zim_t *z, uint16_t mime, const char *prefix)
{
	return mime < z->nmime && !strncmp(z->mime[mime], prefix, strlen(prefix));
}

/* X/listing/titleOrdered/v0, v1: arrays of entry numbers (uint32) */
static int is_listing(const zim_t *z, uint16_t mime)
{
	return mime < z->nmime && !strcmp(z->mime[mime], "application/octet-stream+zimlisting");
}

typedef struct ref {
	uint32_t cluster, blob, entry;
} ref_t;

static int ref_cmp(const void *a, const void *b)
{
	const ref_t *x = a, *y = b;
	if (x->cluster != y->cluster)
		return x->cluster < y->cluster ? -1 : 1;
	if (x->blob != y->blob)
		return x->blob < y->blob ? -1 : 1;
	return x->entry < y->entry ? -1 : x->entry > y->entry;
}

/* raw dirent bytes and size */
static uint32_t read_dirent(zim_t *z, uint32_t i, uint8_t *buf, uint32_t cap)
{
	uint8_t p[8];
	uint64_t off;
	uint32_t hdr, len, n;
	uint16_t mime;
	zw_io_read(&z->io, z->url_ptr_pos + (uint64_t)i * 8, p, 8);
	off = zw_rd64(p);
	len = (uint32_t)(z->io.size - off < cap ? z->io.size - off : cap);
	if (zw_io_read(&z->io, off, buf, len) < 0) {
		fprintf(stderr, "zimprep: cannot read dirent %u\n", i);
		exit(1);
	}
	mime = zw_rd16(buf);
	hdr = mime == ZIM_MIME_REDIRECT ? 12 :
	      (mime == ZIM_MIME_LINKTARGET || mime == ZIM_MIME_DELETED) ? 8 : 16;
	n = hdr;
	while (n < len && buf[n]) n++;		/* url */
	n++;
	while (n < len && buf[n]) n++;		/* title */
	n++;
	n += buf[2];				/* extra parameter bytes */
	if (n > len) {
		fprintf(stderr, "zimprep: dirent %u too long\n", i);
		exit(1);
	}
	return n;
}

static time_t t_start, t_stage;

/* time per stage, so a long quiet stage is not mistaken for a hang */
static void stage_done(const char *what)
{
	time_t now = time(NULL);
	long a = (long)(now - t_stage), b = (long)(now - t_start);
	fprintf(stderr, "  %s took %ld min %02ld s (total %ld h %02ld min)\n",
		what, a / 60, a % 60, b / 3600, (b / 60) % 60);
	t_stage = now;
}

static void progress(const char *what, uint64_t done, uint64_t total)
{
	static time_t last;
	static const char *finished;	/* stage whose 100% line is already out */
	time_t now = time(NULL);
	if (done == total && finished == what)
		return;
	if (now == last && done != total)
		return;
	if (done == total)
		finished = what;
	last = now;
	fprintf(stderr, "\r%s %3d%%", what, total ? (int)(done * 100 / total) : 100);
	if (done == total)
		fprintf(stderr, "\n");
}

int recluster(zim_t *z, const char *base, uint32_t shift, uint32_t target_kb, int level,
	      int threads, int keep_fulltext, int keep_images, uint32_t **out_map,
	      uint32_t *out_entries, uint64_t *out_size, uint32_t *out_chunks)
{
	uint32_t dropped = 0;
	uint32_t *map = NULL;		/* old entry -> new, ZIM_NONE: left out */
	uint32_t nentries = z->entry_count, npics = 0, npicredir = 0;
	uint8_t *listing = NULL;	/* per entry: a title listing blob */
	cw_t w;
	ref_t *refs;
	uint32_t nrefs = 0, i, nclusters = 0, cap_clusters = 1024;
	uint32_t *new_cluster, *new_blob;	/* per entry */
	uint64_t *cluster_off, data_off, pos;
	uint8_t *raw = NULL;
	size_t raw_cap = 0, raw_len = 0;
	uint32_t *blob_len = NULL, nblobs = 0, cap_blobs = 0;
	uint8_t **blob_data = NULL;
	uint64_t mime_len, url_ptr_pos, title_ptr_pos, dirent_pos, cluster_ptr_pos;
	uint8_t hdr[80], dbuf[4096], md5[16];
	size_t target = (size_t)target_kb * 1024;
	pool_t pool;
	job_t *jobs = NULL;
	int njobs = 0, cap_jobs = 0;
	uint8_t *tmp_path_data;
	FILE *tmp;
	char tmp_path[4200];
	uint64_t tmp_len = 0;

	memset(&w, 0, sizeof(w));
	snprintf(w.base, sizeof(w.base), "%s", base);
	w.shift = shift;
	new_cluster = malloc(sizeof(uint32_t) * z->entry_count);
	new_blob = malloc(sizeof(uint32_t) * z->entry_count);
	refs = malloc(sizeof(ref_t) * z->entry_count);
	cluster_off = malloc(sizeof(uint64_t) * cap_clusters);

	t_start = t_stage = time(NULL);
	listing = calloc(z->entry_count ? z->entry_count : 1, 1);
	/* 0. pictures, and redirects that end at one, are left out (the
	 * WikiReader shows no pictures); the rest are renumbered in order,
	 * which keeps the URL order the ZIM format requires */
	if (!keep_images) {
		uint32_t next = 0;
		map = malloc(sizeof(uint32_t) * (z->entry_count ? z->entry_count : 1));
		for (i = 0; i < z->entry_count; ++i) {
			zim_dirent_t d;
			map[i] = 0;
			/* articles' pictures (C/..., or I/... in old ZIMs);
			 * metadata (the Kiwix icon, M/Illustration...) stays */
			if (zim_dirent(z, i, &d) == 0 && d.ns != 'M' &&
			    d.mime != ZIM_MIME_REDIRECT && d.mime != ZIM_MIME_LINKTARGET &&
			    d.mime != ZIM_MIME_DELETED && mime_is(z, d.mime, "image/")) {
				map[i] = ZIM_NONE;
				npics++;
			}
		}
		for (i = 0; i < z->entry_count; ++i) {
			zim_dirent_t d, t;
			uint32_t final;
			if (zim_dirent(z, i, &d) == 0 && d.mime == ZIM_MIME_REDIRECT &&
			    zim_follow(z, i, &final, &t) == 0 && final < z->entry_count &&
			    map[final] == ZIM_NONE) {
				map[i] = ZIM_NONE;
				npicredir++;
			}
		}
		for (i = 0; i < z->entry_count; ++i)
			map[i] = map[i] == ZIM_NONE ? ZIM_NONE : next++;
		nentries = next;
		if (!npics && !npicredir) {
			free(map);
			map = NULL;
		}
	}

	/* 1. which entries use which blob, in cluster order */
	for (i = 0; i < z->entry_count; ++i) {
		zim_dirent_t d;
		new_cluster[i] = new_blob[i] = ZIM_NONE;
		if (map && map[i] == ZIM_NONE)
			continue;
		if (zim_dirent(z, i, &d) == 0 && d.mime < z->nmime && is_listing(z, d.mime))
			listing[i] = 1;
		if (zim_dirent(z, i, &d) == 0 && d.cluster != ZIM_NONE && !keep_fulltext &&
		    is_search_index(&d)) {
			new_cluster[i] = ZIM_NONE - 1;	/* gets an empty blob below */
			dropped++;
		} else if (zim_dirent(z, i, &d) == 0 && d.cluster != ZIM_NONE) {
			refs[nrefs].cluster = d.cluster;
			refs[nrefs].blob = d.blob;
			refs[nrefs].entry = i;
			nrefs++;
		}
	}
	qsort(refs, nrefs, sizeof(ref_t), ref_cmp);

	/* 2. read blobs in that order, pack into new clusters, compress in
	 * parallel batches, append to a temporary file */
	snprintf(tmp_path, sizeof(tmp_path), "%s.clusters.tmp", base);
	tmp = fopen(tmp_path, "w+b");
	if (!tmp) {
		perror(tmp_path);
		exit(1);
	}
	pthread_mutex_init(&pool.mu, NULL);
	pool.level = level;
	(void)tmp_path_data;

#define FLUSH_JOBS() do { \
		pthread_t th[64]; int t; \
		pool.jobs = jobs; pool.njobs = njobs; pool.next = 0; \
		for (t = 0; t < threads; ++t) pthread_create(&th[t], NULL, worker, &pool); \
		for (t = 0; t < threads; ++t) pthread_join(th[t], NULL); \
		for (t = 0; t < njobs; ++t) { \
			if (nclusters == cap_clusters) { cap_clusters *= 2; \
				cluster_off = realloc(cluster_off, sizeof(uint64_t) * cap_clusters); } \
			cluster_off[nclusters++] = tmp_len; \
			if (fwrite(jobs[t].out, 1, jobs[t].out_len, tmp) != jobs[t].out_len) { \
				perror("write"); exit(1); } \
			tmp_len += jobs[t].out_len; \
			free(jobs[t].out); free(jobs[t].raw); } \
		njobs = 0; } while (0)

	i = 0;
	while (i < nrefs) {
		/* gather blobs up to the target size */
		uint32_t start = i, k;
		size_t total = 0;
		nblobs = 0;
		while (i < nrefs && (total < target || nblobs == 0)) {
			const uint8_t *p;
			uint32_t len;
			/* entries sharing a blob keep sharing it */
			if (i > start && refs[i].cluster == refs[i - 1].cluster &&
			    refs[i].blob == refs[i - 1].blob) {
				new_cluster[refs[i].entry] = nclusters + (uint32_t)njobs;
				new_blob[refs[i].entry] = nblobs - 1;
				i++;
				continue;
			}
			if (zim_blob(z, refs[i].cluster, refs[i].blob, &p, &len) < 0) {
				fprintf(stderr, "zimprep: cannot read entry %u\n", refs[i].entry);
				exit(1);
			}
			if (nblobs == cap_blobs) {
				cap_blobs = cap_blobs ? cap_blobs * 2 : 256;
				blob_len = realloc(blob_len, sizeof(uint32_t) * cap_blobs);
				blob_data = realloc(blob_data, sizeof(uint8_t *) * cap_blobs);
			}
			blob_len[nblobs] = len;
			blob_data[nblobs] = malloc(len ? len : 1);
			memcpy(blob_data[nblobs], p, len);
			if (map && listing[refs[i].entry]) {
				/* title listing: renumber, leaving out removed
				 * entries */
				uint32_t k2, n2 = 0;
				for (k2 = 0; k2 + 4 <= len; k2 += 4) {
					uint32_t e = zw_rd32(blob_data[nblobs] + k2);
					if (e < z->entry_count && map[e] != ZIM_NONE) {
						zw_wr32(blob_data[nblobs] + n2, map[e]);
						n2 += 4;
					}
				}
				len = n2;
				blob_len[nblobs] = len;
			}
			new_cluster[refs[i].entry] = nclusters + (uint32_t)njobs;
			new_blob[refs[i].entry] = nblobs;
			nblobs++;
			total += len;
			i++;
		}
		/* cluster body: offsets (n+1) then the blobs */
		raw_len = 4 * (size_t)(nblobs + 1) + total;
		raw = malloc(raw_len);
		{
			uint32_t off = 4 * (nblobs + 1);
			for (k = 0; k < nblobs; ++k) {
				zw_wr32(raw + 4 * k, off);
				memcpy(raw + off, blob_data[k], blob_len[k]);
				off += blob_len[k];
				free(blob_data[k]);
			}
			zw_wr32(raw + 4 * nblobs, off);
		}
		if (njobs == cap_jobs) {
			cap_jobs = cap_jobs ? cap_jobs * 2 : 64;
			jobs = realloc(jobs, sizeof(job_t) * cap_jobs);
		}
		jobs[njobs].raw = raw;
		jobs[njobs].raw_len = raw_len;
		njobs++;
		raw = NULL;
		if (njobs >= threads * 16)
			FLUSH_JOBS();
		progress("reclustering", i, nrefs);
	}
	if (dropped) {
		/* one cluster holding a single empty blob for the dropped
		 * search indexes: still a valid ZIM, just without them */
		uint32_t e;
		raw_len = 8;
		raw = malloc(raw_len);
		zw_wr32(raw, 8);
		zw_wr32(raw + 4, 8);
		for (e = 0; e < z->entry_count; ++e)
			if (new_cluster[e] == ZIM_NONE - 1) {
				new_cluster[e] = nclusters + (uint32_t)njobs;
				new_blob[e] = 0;
			}
		if (njobs == cap_jobs) {
			cap_jobs = cap_jobs ? cap_jobs * 2 : 64;
			jobs = realloc(jobs, sizeof(job_t) * cap_jobs);
		}
		jobs[njobs].raw = raw;
		jobs[njobs].raw_len = raw_len;
		njobs++;
		raw = NULL;
	}
	if (njobs) {
		size_t big = 0;
		int t;
		for (t = 0; t < njobs; ++t)
			if (jobs[t].raw_len > big)
				big = jobs[t].raw_len;
		if (big > BIG_CLUSTER / 4)
			fprintf(stderr, "\nfinishing the last batch (largest cluster %zu MB)...\n", big >> 20);
		FLUSH_JOBS();
	}
	progress("reclustering", nrefs, nrefs);
	stage_done("reclustering");
	if (dropped)
		fprintf(stderr, "left out %u full text search index entr%s (not used by the WikiReader)\n",
			dropped, dropped == 1 ? "y" : "ies");
	if (npics || npicredir)
		fprintf(stderr, "left out %u picture%s and %u redirect%s to them (the WikiReader shows no pictures; --keep-images keeps them)\n",
			npics, npics == 1 ? "" : "s", npicredir, npicredir == 1 ? "" : "s");
	(void)raw_cap;

	/* 3. assemble: header, MIME list, pointer lists, dirents,
	 * cluster pointers, clusters, checksum */
	{
		uint8_t *mime = (uint8_t *)z->mime_buf;
		uint64_t m = 0;
		while (mime[m])
			m += strlen((char *)mime + m) + 1;
		mime_len = m + 1;
	}
	pos = 80;
	cw_write(&w, pos, z->mime_buf, (size_t)mime_len);
	pos += mime_len;
	url_ptr_pos = pos;
	pos += (uint64_t)nentries * 8;
	title_ptr_pos = pos;
	{
		/* titles keep their order: copy the list (renumbered, without
		 * the entries left out) */
		uint8_t *tl = malloc((size_t)z->entry_count * 4 + 1);
		zw_io_read(&z->io, z->title_ptr_pos, tl, z->entry_count * 4);
		if (map) {
			uint32_t n2 = 0;
			for (i = 0; i < z->entry_count; ++i) {
				uint32_t e = zw_rd32(tl + (size_t)i * 4);
				if (e < z->entry_count && map[e] != ZIM_NONE)
					zw_wr32(tl + (size_t)(n2++) * 4, map[e]);
			}
			if (n2 != nentries) {
				fprintf(stderr, "zimprep: title list has %u of %u entries\n", n2, nentries);
				exit(1);
			}
		}
		cw_write(&w, pos, tl, (size_t)nentries * 4);
		free(tl);
	}
	pos += (uint64_t)nentries * 4;
	dirent_pos = pos;
	{
		uint8_t *ptrs = malloc((size_t)nentries * 8 + 1);
		for (i = 0; i < z->entry_count; ++i) {
			uint32_t n, ni = map ? map[i] : i;
			uint16_t mt;
			if (ni == ZIM_NONE)
				continue;
			n = read_dirent(z, i, dbuf, sizeof(dbuf));
			mt = zw_rd16(dbuf);
			if (mt != ZIM_MIME_REDIRECT && mt != ZIM_MIME_LINKTARGET && mt != ZIM_MIME_DELETED) {
				zw_wr32(dbuf + 8, new_cluster[i]);
				zw_wr32(dbuf + 12, new_blob[i]);
			} else if (mt == ZIM_MIME_REDIRECT && map) {
				uint32_t t = zw_rd32(dbuf + 8);
				if (t >= z->entry_count || map[t] == ZIM_NONE) {
					fprintf(stderr, "zimprep: redirect %u to a removed entry\n", i);
					exit(1);
				}
				zw_wr32(dbuf + 8, map[t]);
			}
			zw_wr32(ptrs + (size_t)ni * 8, (uint32_t)pos);
			zw_wr32(ptrs + (size_t)ni * 8 + 4, (uint32_t)(pos >> 32));
			cw_write(&w, pos, dbuf, n);
			pos += n;
			if ((i & 0x3fff) == 0)
				progress("writing entries", i, z->entry_count);
		}
		progress("writing entries", z->entry_count, z->entry_count);
		stage_done("writing entries");
		cw_write(&w, url_ptr_pos, ptrs, (size_t)nentries * 8);
		free(ptrs);
	}
	(void)dirent_pos;
	cluster_ptr_pos = pos;
	data_off = cluster_ptr_pos + (uint64_t)nclusters * 8;
	{
		uint8_t *cp = malloc((size_t)nclusters * 8 + 1);
		for (i = 0; i < nclusters; ++i) {
			uint64_t o = data_off + cluster_off[i];
			zw_wr32(cp + (size_t)i * 8, (uint32_t)o);
			zw_wr32(cp + (size_t)i * 8 + 4, (uint32_t)(o >> 32));
		}
		cw_write(&w, pos, cp, (size_t)nclusters * 8);
		free(cp);
	}
	pos = data_off;
	{
		uint8_t *buf = malloc(4 << 20);
		uint64_t done = 0;
		fflush(tmp);
		fseeko(tmp, 0, SEEK_SET);
		while (done < tmp_len) {
			size_t n = tmp_len - done < (4u << 20) ? (size_t)(tmp_len - done) : (4u << 20);
			if (fread(buf, 1, n, tmp) != n) {
				perror("read temporary file");
				exit(1);
			}
			cw_write(&w, pos, buf, n);
			pos += n;
			done += n;
			progress("copying clusters", done, tmp_len);
		}
		free(buf);
	}
	stage_done("copying clusters");
	fclose(tmp);
	remove(tmp_path);

	/* header */
	memset(hdr, 0, sizeof(hdr));
	zw_wr32(hdr, 72173914);
	zw_wr16(hdr + 4, z->major);
	zw_wr16(hdr + 6, z->minor);
	memcpy(hdr + 8, z->uuid, 16);
	zw_wr32(hdr + 24, nentries);
	zw_wr32(hdr + 28, nclusters);
#define W64(o, v) do { zw_wr32(hdr + (o), (uint32_t)(v)); zw_wr32(hdr + (o) + 4, (uint32_t)((uint64_t)(v) >> 32)); } while (0)
	W64(32, url_ptr_pos);
	W64(40, title_ptr_pos);
	W64(48, cluster_ptr_pos);
	W64(56, 80);
	zw_wr32(hdr + 64, map && z->main_page < z->entry_count ? map[z->main_page] : z->main_page);
	zw_wr32(hdr + 68, 0xffffffffu);
	W64(72, pos);
	cw_write(&w, 0, hdr, 80);

	/* checksum over everything before it */
	{
		md5_t m;
		uint8_t *buf = malloc(4 << 20);
		uint64_t done = 0;
		md5_init(&m);
		while (done < pos) {
			size_t n = pos - done < (4u << 20) ? (size_t)(pos - done) : (4u << 20);
			cw_read(&w, done, buf, n);
			md5_update(&m, buf, n);
			done += n;
			progress("checksum", done, pos);
		}
		md5_final(&m, md5);
		stage_done("checksum");
		free(buf);
	}
	cw_write(&w, pos, md5, 16);
	pos += 16;
	for (i = 0; i < 100; ++i)
		if (w.f[i] && fclose(w.f[i]) != 0) {
			perror("close");
			exit(1);
		}
	*out_size = pos;
	*out_chunks = (uint32_t)((pos + ((uint64_t)1 << shift) - 1) >> shift);
	fprintf(stderr, "reclustered: %u entries, %u clusters of ~%u KiB, %llu bytes (was %llu)\n",
		nentries, nclusters, target_kb, (unsigned long long)pos,
		(unsigned long long)z->io.size);
	*out_map = map;
	*out_entries = nentries;
	free(listing);
	free(new_cluster);
	free(new_blob);
	free(refs);
	free(cluster_off);
	free(jobs);
	free(blob_len);
	free(blob_data);
	return 0;
}
