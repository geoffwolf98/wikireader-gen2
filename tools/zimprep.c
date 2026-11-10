/*
 * zimprep - prepare a ZIM file for the WikiReader
 *
 *   zimprep [options] INPUT.zim OUTDIR
 *
 *   -n NAME       base name on the card, 1-8 letters/digits (default WIKI)
 *   --chunk-mb N  chunk size in MiB, power of two, max 2048 (default 2048)
 *   --index-only  write NAME.IDX only (chunks already on the card)
 *   --recluster [KB]  rewrite the ZIM with small clusters (default 128 KiB)
 *                 so the WikiReader decompresses far less per article
 *   --level N     zstd level for --recluster (default 19)
 *   --threads N   compression threads for --recluster (default: all CPUs)
 *   --keep-fulltext  keep Kiwix's Xapian search indexes when reclustering
 *                 (left out by default: large, slow, unused on the device)
 *   --keep-images keep the pictures when reclustering (left out by
 *                 default, with redirects to them: the WikiReader shows
 *                 no pictures, so a "maxi" ZIM comes out about the size of
 *                 a "nopic" one)
 *
 * Writes OUTDIR/NAME.IDX (title search index) and OUTDIR/NAME.Z00,
 * NAME.Z01, ... (the ZIM split into pieces FAT32 can hold).  Copy the
 * contents of OUTDIR to the SD card (top folder or a folder of its own).
 *
 * INPUT may also be the base of a split set: "wiki.zim" for
 * wiki.zimaa, wiki.zimab, ...
 */
#define _FILE_OFFSET_BITS 64
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../src/zim.h"
#include "../src/fold.h"
#include "../src/index.h"
#include "../host/hostio.h"
#include "recluster.h"
#include <unistd.h>

/* recluster's renumbering (NULL: none), applied to the index */
static uint32_t *g_map;
static uint32_t g_entries;

static uint32_t renum(uint32_t e)
{
	return g_map && e != ZIM_NONE ? g_map[e] : e;
}

typedef struct rec {
	uint32_t key;		/* offset into key pool */
	uint32_t title;		/* offset into title pool */
	uint32_t target;
	uint8_t flags;
	uint8_t tlen;
} rec_t;

typedef struct pool {
	char *p;
	size_t len, cap;
} pool_t;

static pool_t keys, titles;
static rec_t *recs;
static size_t nrec, caprec;

static void die(const char *fmt, const char *arg)
{
	fprintf(stderr, "zimprep: ");
	fprintf(stderr, fmt, arg);
	fprintf(stderr, "\n");
	exit(1);
}

static uint32_t pool_add(pool_t *p, const char *s, size_t n)
{
	size_t off = p->len;
	if (p->len + n + 1 > p->cap) {
		p->cap = (p->cap + n + 1) * 2;
		p->p = realloc(p->p, p->cap);
		if (!p->p)
			die("out of memory", "");
	}
	memcpy(p->p + p->len, s, n);
	p->p[p->len + n] = 0;
	p->len += n + 1;
	if (p->len > 0xffffffffu)
		die("title pool exceeds 4 GiB", "");
	return (uint32_t)off;
}

static int rec_cmp(const void *a, const void *b)
{
	const rec_t *x = a, *y = b;
	int c = strcmp(keys.p + x->key, keys.p + y->key);
	if (c)
		return c;
	if (x->flags != y->flags)
		return (x->flags & IDX_FLAG_REDIRECT) ? 1 : -1;	/* articles first */
	c = strcmp(titles.p + x->title, titles.p + y->title);
	if (c)
		return c;
	return x->target < y->target ? -1 : x->target > y->target;
}

/* truncate a UTF-8 title to at most 255 bytes without splitting a char */
static size_t clip(const char *s)
{
	size_t n = strlen(s);
	if (n <= 255)
		return n;
	n = 255;
	while (n && ((unsigned char)s[n] & 0xc0) == 0x80)
		n--;
	return n;
}

static void progress(const char *what, uint64_t done, uint64_t total)
{
	static time_t last;
	time_t now = time(NULL);
	if (now == last && done != total)
		return;
	last = now;
	fprintf(stderr, "\r%s %3d%%", what, total ? (int)(done * 100 / total) : 100);
	if (done == total)
		fprintf(stderr, "\n");
}

static void add_record(const char *title, uint32_t target, uint8_t flags)
{
	char key[512];
	size_t tl = clip(title), kl;
	if (!tl)
		return;
	kl = fold_utf8(title, tl, key, sizeof(key));
	if (!kl)
		return;
	if (nrec == caprec) {
		caprec = caprec ? caprec * 2 : 1 << 16;
		recs = realloc(recs, caprec * sizeof(rec_t));
		if (!recs)
			die("out of memory", "");
	}
	recs[nrec].key = pool_add(&keys, key, kl);
	recs[nrec].title = pool_add(&titles, title, tl);
	recs[nrec].tlen = (uint8_t)tl;
	recs[nrec].target = target;
	recs[nrec].flags = flags;
	nrec++;
}

static int is_html(zim_t *z, uint16_t mime)
{
	return mime < z->nmime && strncmp(z->mime[mime], "text/html", 9) == 0;
}

static void write_index(zim_t *z, const char *path, const char *name,
			uint32_t chunks, uint32_t shift, uint64_t zim_size)
{
	FILE *f = fopen(path, "wb");
	uint8_t page[IDX_PAGE], hdr[IDX_PAGE];
	uint8_t *top;
	size_t i, pages = 0, cap_pages = 1024, off = 0, articles = 0, out = 0;
	uint32_t main_final = ZIM_NONE;
	char meta[256];

	if (!f)
		die("cannot write %s", path);
	top = malloc(cap_pages * IDX_TOP);
	memset(hdr, 0, sizeof(hdr));
	fwrite(hdr, 1, IDX_PAGE, f);	/* placeholder */

	memset(page, 0, sizeof(page));
	for (i = 0; i < nrec; ++i) {
		rec_t *r = &recs[i];
		size_t need = IDX_REC_HDR + r->tlen;
		/* drop exact duplicates: same folded key, same target */
		if (i && strcmp(keys.p + r->key, keys.p + recs[i - 1].key) == 0 &&
		    r->target == recs[i - 1].target)
			continue;
		if (off == 0 || off + need > IDX_PAGE) {
			if (off) {
				fwrite(page, 1, IDX_PAGE, f);
				memset(page, 0, sizeof(page));
			}
			if (pages == cap_pages) {
				cap_pages *= 2;
				top = realloc(top, cap_pages * IDX_TOP);
			}
			{
				uint8_t *t = top + pages * IDX_TOP;
				const char *k = keys.p + r->key;
				size_t kl = strlen(k);
				memset(t, 0, IDX_TOP);
				zw_wr32(t, (uint32_t)out);
				/* key prefix must not end in a split UTF-8 char:
				 * bytewise order is still correct, so allow it */
				t[4] = (uint8_t)(kl > IDX_KEY ? IDX_KEY : kl);
				memcpy(t + 5, k, t[4]);
			}
			pages++;
			off = 0;
		}
		page[off] = r->tlen;
		page[off + 1] = r->flags;
		zw_wr32(page + off + 2, renum(r->target));
		memcpy(page + off + IDX_REC_HDR, titles.p + r->title, r->tlen);
		off += need;
		out++;
		if (!(r->flags & IDX_FLAG_REDIRECT))
			articles++;
	}
	if (off)
		fwrite(page, 1, IDX_PAGE, f);

	{
		uint32_t top_off = (uint32_t)((pages + 1) * IDX_PAGE);
		uint32_t sum_count = (uint32_t)((pages + IDX_STRIDE - 1) / IDX_STRIDE);
		uint32_t sum_off = top_off + (uint32_t)(pages * IDX_TOP);
		if ((uint64_t)sum_off + (uint64_t)sum_count * IDX_TOP > 0xffffffffu)
			die("index larger than 4 GiB", "");
		fwrite(top, IDX_TOP, pages, f);
		for (i = 0; i < sum_count; ++i)
			fwrite(top + i * IDX_STRIDE * IDX_TOP, 1, IDX_TOP, f);

		if (z->main_page != ZIM_NONE) {
			zim_dirent_t d;
			if (zim_follow(z, z->main_page, &main_final, &d) < 0)
				main_final = ZIM_NONE;
			main_final = renum(main_final);
		}
		zw_wr32(hdr + IDX_H_MAGIC, IDX_MAGIC);
		zw_wr32(hdr + IDX_H_VERSION, IDX_VERSION);
		zw_wr32(hdr + IDX_H_PAGES, (uint32_t)pages);
		zw_wr32(hdr + IDX_H_RECORDS, (uint32_t)out);
		zw_wr32(hdr + IDX_H_ARTICLES, (uint32_t)articles);
		zw_wr32(hdr + IDX_H_TOP_OFF, top_off);
		zw_wr32(hdr + IDX_H_SUM_OFF, sum_off);
		zw_wr32(hdr + IDX_H_SUM_COUNT, sum_count);
		memcpy(hdr + IDX_H_UUID, z->uuid, 16);
		zw_wr32(hdr + IDX_H_ZIM_ENTRIES, g_map ? g_entries : z->entry_count);
		zw_wr32(hdr + IDX_H_CHUNKS, chunks);
		hdr[IDX_H_CHUNK_SHIFT] = (uint8_t)shift;
		zw_wr32(hdr + IDX_H_ZIM_SIZE, (uint32_t)zim_size);
		zw_wr32(hdr + IDX_H_ZIM_SIZE + 4, (uint32_t)(zim_size >> 32));
		zw_wr32(hdr + IDX_H_MAIN, main_final);
		if (zim_metadata(z, "Title", meta, sizeof(meta)) == 0)
			strncpy((char *)hdr + IDX_H_TITLE, meta, IDX_H_TITLE_LEN - 1);
		if (zim_metadata(z, "Language", meta, sizeof(meta)) == 0)
			strncpy((char *)hdr + IDX_H_LANG, meta, IDX_H_LANG_LEN - 1);
		strncpy((char *)hdr + IDX_H_NAME, name, IDX_H_NAME_LEN - 1);
		fseeko(f, 0, SEEK_SET);
		fwrite(hdr, 1, IDX_PAGE, f);
	}
	if (fclose(f) != 0)
		die("write failed: %s", path);
	fprintf(stderr, "index: %zu titles (%zu articles + redirects), %zu pages\n",
		out, articles, pages);
	if (articles < 10 || articles * 20 < z->entry_count)
		fprintf(stderr, "warning: only %zu readable articles in %u entries; this ZIM is mostly\n"
			"  PDFs, video or pictures, which the WikiReader cannot show\n",
			articles, z->entry_count);
	free(top);
}

static void write_chunks(host_files_t *in, const char *dir, const char *name,
			 uint32_t shift, uint32_t chunks)
{
	uint64_t size = in->io.size, done = 0, csize = (uint64_t)1 << shift;
	uint32_t c, bufsz = 4 << 20;
	uint8_t *buf = malloc(bufsz);
	char path[4096];

	for (c = 0; c < chunks; ++c) {
		uint64_t end = (c + 1) * csize < size ? (c + 1) * csize : size;
		FILE *f;
		snprintf(path, sizeof(path), "%s/%s.Z%02u", dir, name, c);
		f = fopen(path, "wb");
		if (!f)
			die("cannot write %s", path);
		while (done < end) {
			uint32_t n = (uint32_t)(end - done < bufsz ? end - done : bufsz);
			if (in->io.read(in->io.ctx, done, buf, n) < 0)
				die("read error on input", "");
			if (fwrite(buf, 1, n, f) != n)
				die("write failed: %s", path);
			done += n;
			progress("copying", done, size);
		}
		if (fclose(f) != 0)
			die("write failed: %s", path);
	}
	free(buf);
}

/* The WikiReader creates NAME.HIS (history) and NAME.EXT (file map) the
 * first time it uses a wiki.  Creating a file on a nearly full card makes
 * its FAT code search the whole FAT for free space, which takes long
 * enough to freeze or reset the unit, so they are made here, empty (the
 * device reads zeros as "nothing saved yet") and big enough that the
 * device only ever overwrites them. */
#define PLACEHOLDER_SIZE (64 * 1024)

static void placeholder(const char *dir, const char *name, const char *ext)
{
	char path[4096];
	FILE *f;
	static const char zero[4096];
	int i;
	snprintf(path, sizeof(path), "%s/%s%s", dir, name, ext);
	if ((f = fopen(path, "rb")) != NULL) {	/* keep an existing one */
		fclose(f);
		return;
	}
	if (!(f = fopen(path, "wb")))
		return;
	for (i = 0; i < PLACEHOLDER_SIZE / (int)sizeof(zero); ++i)
		fwrite(zero, 1, sizeof(zero), f);
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *name = "WIKI", *input = NULL, *outdir = NULL;
	uint32_t chunk_mb = 2048, shift, chunks, i;
	int index_only = 0, a, r, level = 19, threads = 0, keep_fulltext = 0, keep_images = 0;
	uint32_t recluster_kb = 0;
	host_files_t in;
	zim_t z;
	char path[4096], upname[16];

	for (a = 1; a < argc; ++a) {
		if (strcmp(argv[a], "--version") == 0) {
			printf("zimprep (zimwiki) %s\n", ZW_VERSION);
			return 0;
		} else if (strcmp(argv[a], "-n") == 0 && a + 1 < argc)
			name = argv[++a];
		else if (strcmp(argv[a], "--chunk-mb") == 0 && a + 1 < argc)
			chunk_mb = (uint32_t)atoi(argv[++a]);
		else if (strcmp(argv[a], "--index-only") == 0)
			index_only = 1;
		else if (strcmp(argv[a], "--recluster") == 0) {
			recluster_kb = 128;
			if (a + 1 < argc && atoi(argv[a + 1]) > 0)
				recluster_kb = (uint32_t)atoi(argv[++a]);
		} else if (strcmp(argv[a], "--level") == 0 && a + 1 < argc)
			level = atoi(argv[++a]);
		else if (strcmp(argv[a], "--keep-fulltext") == 0)
			keep_fulltext = 1;
		else if (strcmp(argv[a], "--keep-images") == 0)
			keep_images = 1;
		else if (strcmp(argv[a], "--threads") == 0 && a + 1 < argc)
			threads = atoi(argv[++a]);
		else if (!input)
			input = argv[a];
		else if (!outdir)
			outdir = argv[a];
		else
			die("unexpected argument %s", argv[a]);
	}
	if (!input || !outdir) {
		fprintf(stderr, "usage: zimprep [-n NAME] [--chunk-mb N] [--index-only] [--recluster [KB]] [--level N]\n"
			"               [--threads N] [--keep-fulltext] [--keep-images] INPUT.zim OUTDIR\n"
			"       zimprep --version\n");
		return 2;
	}
	if (strlen(name) < 1 || strlen(name) > 8)
		die("name must be 1-8 characters: %s", name);
	for (i = 0; name[i]; ++i) {
		if (!isalnum((unsigned char)name[i]))
			die("name must be letters and digits: %s", name);
		upname[i] = (char)toupper((unsigned char)name[i]);
	}
	upname[i] = 0;
	for (shift = 20; shift < 31 && (1u << (shift - 20)) < chunk_mb; ++shift)
		;
	if ((1u << (shift - 20)) != chunk_mb)
		die("chunk size must be a power of two MiB up to 2048", "");

	zw_log_enabled = 0;
	if ((r = host_files_open(&in, input)) < 0)
		die("cannot open %s", input);
	if ((r = zim_open(&z, &in.io)) < 0)
		die("%s is not a ZIM file this tool understands", input);
	chunks = (uint32_t)((in.io.size + ((uint64_t)1 << shift) - 1) >> shift);
	if (chunks > 100)
		die("too many chunks; use a bigger --chunk-mb", "");
	mkdir(outdir, 0777);

	fprintf(stderr, "%s: %u entries, namespace %c, %u chunk(s) of %u MiB\n",
		input, z.entry_count, z.content_ns, chunks, chunk_mb);

	for (i = 0; i < z.entry_count; ++i) {
		zim_dirent_t d;
		if (zim_dirent(&z, i, &d) < 0 || d.ns != z.content_ns)
			continue;
		if (d.mime == ZIM_MIME_REDIRECT) {
			zim_dirent_t t;
			uint32_t final;
			if (zim_follow(&z, i, &final, &t) == 0 && t.ns == z.content_ns &&
			    is_html(&z, t.mime))
				add_record(d.title, final, IDX_FLAG_REDIRECT);
		} else if (is_html(&z, d.mime)) {
			add_record(d.title, i, 0);
		}
		if ((i & 0xffff) == 0)
			progress("reading titles", i, z.entry_count);
	}
	progress("reading titles", z.entry_count, z.entry_count);
	fprintf(stderr, "sorting %zu titles\n", nrec);
	qsort(recs, nrec, sizeof(rec_t), rec_cmp);

	if (recluster_kb && !index_only) {
		uint64_t size;
		char base[4096];
		if (threads <= 0)
			threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
		if (threads < 1)
			threads = 1;
		if (threads > 64)
			threads = 64;
		snprintf(base, sizeof(base), "%s/%s", outdir, upname);
		fprintf(stderr, "reclustering to %u KiB clusters, zstd level %d, %d threads\n",
			recluster_kb, level, threads);
		recluster(&z, base, shift, recluster_kb, level, threads, keep_fulltext, keep_images,
			  &g_map, &g_entries, &size, &chunks);
		snprintf(path, sizeof(path), "%s/%s.IDX", outdir, upname);
		write_index(&z, path, upname, chunks, shift, size);
	} else {
		snprintf(path, sizeof(path), "%s/%s.IDX", outdir, upname);
		write_index(&z, path, upname, chunks, shift, in.io.size);
		if (!index_only)
			write_chunks(&in, outdir, upname, shift, chunks);
	}
	placeholder(outdir, upname, ".HIS");
	placeholder(outdir, upname, ".EXT");
	zim_close(&z);
	host_files_close(&in);
	fprintf(stderr, "done: copy %s/%s.* to the SD card (top folder or a folder of its own)\n", outdir, upname);
	return 0;
}
