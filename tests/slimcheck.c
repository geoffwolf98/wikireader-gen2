/*
 * slimcheck: does slimming (tools/slim.c) change any article?
 *
 *   slimcheck ZIM [EVERY]
 *   slimcheck --pair ORIGINAL.zim PREPARED.zim   (PREPARED: NAME.Z* joined)
 *
 * Converts each HTML article (every EVERYth one, default all) with the
 * WikiReader's own converter, as it is and slimmed, with each setting of
 * Footnotes and Citations, without pictures and with all its pictures,
 * and compares the documents: text, styles, links, blocks, anchors.
 * Prints the sizes and the articles that differ; exit status 1 if any.
 * --pair compares each article of the original with the same article
 * (found by path) in a ZIM zimprep made: what zimprep actually wrote.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/zim.h"
#include "../src/doc.h"
#include "../src/pic.h"
#include "../host/hostio.h"
#include "../tools/slim.h"

static int same_doc(const doc_t *a, const doc_t *b, char *why, size_t wl)
{
	uint32_t i;
#define DIFF(...) do { snprintf(why, wl, __VA_ARGS__); return 0; } while (0)
	if (a->text_len != b->text_len || memcmp(a->text, b->text, a->text_len))
		DIFF("text (%u / %u bytes)", a->text_len, b->text_len);
	if (a->nruns != b->nruns)
		DIFF("runs %u / %u", a->nruns, b->nruns);
	for (i = 0; i < a->nruns; ++i)
		if (a->runs[i].start != b->runs[i].start || a->runs[i].len != b->runs[i].len ||
		    a->runs[i].link != b->runs[i].link || a->runs[i].font != b->runs[i].font ||
		    a->runs[i].flags != b->runs[i].flags)
			DIFF("run %u", i);
	if (a->nblocks != b->nblocks)
		DIFF("blocks %u / %u", a->nblocks, b->nblocks);
	for (i = 0; i < a->nblocks; ++i)
		if (a->blocks[i].first_run != b->blocks[i].first_run ||
		    a->blocks[i].nruns != b->blocks[i].nruns || a->blocks[i].kind != b->blocks[i].kind ||
		    a->blocks[i].indent != b->blocks[i].indent || a->blocks[i].number != b->blocks[i].number)
			DIFF("block %u", i);
	if (a->links_len != b->links_len || memcmp(a->links, b->links, a->links_len) ||
	    a->nlinks != b->nlinks)
		DIFF("links");
	if (strcmp(a->title, b->title))
		DIFF("title");
	if (a->refresh_link != b->refresh_link || strcmp(a->refresh_frag, b->refresh_frag))
		DIFF("refresh");
	if (a->anchors_len != b->anchors_len || memcmp(a->anchors, b->anchors, a->anchors_len))
		DIFF("anchors");
	return 1;
#undef DIFF
}

/* every <img> src / data-src hash in the article: "all its pictures" */
static void all_pictures(const char *h, uint32_t len, pic_set_t *s)
{
	const char *p = h, *end = h + len;
	memset(s, 0, sizeof(*s));
	while ((p = memchr(p, '<', (size_t)(end - p))) && s->n < PIC_MAX) {
		const char *gt;
		if (end - p < 5 || strncasecmp(p, "<img", 4)) {
			p++;
			continue;
		}
		gt = memchr(p, '>', (size_t)(end - p));
		if (!gt)
			break;
		{
			const char *names[2] = { " src=\"", " data-src=\"" };
			int k;
			for (k = 0; k < 2 && s->n < PIC_MAX; ++k) {
				const char *a = p;
				size_t nl = strlen(names[k]);
				while (a + nl < gt && memcmp(a, names[k], nl))
					a++;
				if (a + nl < gt) {
					const char *v = a + nl, *q = memchr(v, '"', (size_t)(gt - v));
					if (q && q > v && memcmp(v, "data:", 5)) {
						uint32_t hh = pic_src_hash(v, (uint32_t)(q - v));
						s->hash[s->n] = hh;
						s->w[s->n] = 10;
						s->h[s->n] = 10;
						s->off[s->n] = s->n;
						s->n++;
					}
				}
			}
		}
		p = gt;
	}
	/* pic_find wants them sorted? it searches linearly; keep order */
}

static int compare(const char *orig, uint32_t olen, const char *slim, uint32_t slen,
		   const zim_dirent_t *d, int new_ns)
{
	int opts, pics;
	pic_set_t ps;
	all_pictures(orig, olen, &ps);
	for (pics = 0; pics < 2; ++pics) {
		html_set_pictures(pics && ps.n ? &ps : NULL);
		for (opts = 0; opts < 4; ++opts) {
			doc_t a, b;
			char why[128];
			int same;
			html_set_options(opts);
			doc_init(&a);
			doc_init(&b);
			html_to_doc(&a, orig, olen, d->ns, d->url, new_ns);
			html_to_doc(&b, slim, slen, d->ns, d->url, new_ns);
			same = same_doc(&a, &b, why, sizeof(why));
			doc_free(&a);
			doc_free(&b);
			if (!same) {
				printf("DIFF %c/%s (footnotes %d, citations %d, pictures %d): %s\n",
				       d->ns, d->url, opts & 1, (opts >> 1) & 1, pics, why);
				return 0;
			}
		}
	}
	return 1;
}

static int pair(const char *orig, const char *prep)
{
	host_files_t f1, f2;
	zim_t a, b;
	uint32_t i, n = 0, bad = 0, missing = 0;
	uint64_t before = 0, after = 0;
	if (host_files_open(&f1, orig) < 0 || zim_open(&a, &f1.io) < 0 ||
	    host_files_open(&f2, prep) < 0 || zim_open(&b, &f2.io) < 0) {
		fprintf(stderr, "slimcheck: cannot open the ZIMs\n");
		return 2;
	}
	for (i = 0; i < a.entry_count; ++i) {
		zim_dirent_t d, e;
		if (isatty(2) && i % 65536 == 0)
			fprintf(stderr, "\rchecking %3u%%  (%u articles, %u differ) ",
				(unsigned)((uint64_t)i * 100 / a.entry_count), n, bad);
		const uint8_t *data;
		uint32_t len, j, len2;
		char *copy;
		if (zim_dirent(&a, i, &d) < 0 || d.mime >= a.nmime ||
		    strncmp(a.mime[d.mime], "text/html", 9) != 0)
			continue;
		if (zim_blob(&a, d.cluster, d.blob, &data, &len) < 0)
			continue;
		copy = malloc(len + 1);
		memcpy(copy, data, len);
		if (zim_find(&b, d.ns, d.url, &j) < 0 || zim_dirent(&b, j, &e) < 0 ||
		    zim_blob(&b, e.cluster, e.blob, &data, &len2) < 0) {
			if (missing++ < 5)
				printf("MISSING %c/%s\n", d.ns, d.url);
			free(copy);
			continue;
		}
		before += len;
		after += len2;
		n++;
		if (!compare(copy, len, (const char *)data, len2, &d, a.content_ns == 'C'))
			bad++;
		free(copy);
	}
	if (isatty(2))
		fprintf(stderr, "\r%40s\r", "");
	printf("%u articles, HTML %llu -> %llu bytes (%.0f%% smaller), %u differ, %u missing\n", n,
	       (unsigned long long)before, (unsigned long long)after,
	       before ? 100.0 - 100.0 * (double)after / (double)before : 0.0, bad, missing);
	return bad || missing ? 1 : 0;
}

int main(int argc, char **argv)
{
	host_files_t f;
	zim_t z;
	uint32_t i, every = argc > 2 ? (uint32_t)atoi(argv[2]) : 1, n = 0, bad = 0;
	uint64_t before = 0, after = 0;
	char *buf = NULL;
	size_t cap = 0;
	if (argc < 2) {
		fprintf(stderr, "usage: slimcheck ZIM [EVERY]\n       slimcheck --pair ORIGINAL.zim PREPARED.zim\n");
		return 2;
	}
	if (!strcmp(argv[1], "--pair") && argc == 4)
		return pair(argv[2], argv[3]);
	if (host_files_open(&f, argv[1]) < 0 || zim_open(&z, &f.io) < 0) {
		fprintf(stderr, "slimcheck: cannot open %s\n", argv[1]);
		return 2;
	}
	if (!every)
		every = 1;
	for (i = 0; i < z.entry_count; i += every) {
		zim_dirent_t d;
		if (isatty(2) && (i / every) % 256 == 0)
			fprintf(stderr, "\rchecking %3u%%  (%u articles, %u differ) ",
				(unsigned)((uint64_t)i * 100 / z.entry_count), n, bad);
		const uint8_t *data;
		uint32_t len;
		char *copy;
		size_t sl;
		int opts, pics;
		if (zim_dirent(&z, i, &d) < 0 || d.mime >= z.nmime ||
		    strncmp(z.mime[d.mime], "text/html", 9) != 0)
			continue;
		if (zim_blob(&z, d.cluster, d.blob, &data, &len) < 0)
			continue;
		copy = malloc(len + 1);
		memcpy(copy, data, len);
		if (cap < 2 * (size_t)len + 256) {
			cap = 2 * (size_t)len + 256;
			buf = realloc(buf, cap);
		}
		sl = html_slim(copy, len, buf);
		before += len;
		after += sl;
		n++;
		for (pics = 0; pics < 2; ++pics) {
			pic_set_t ps;
			all_pictures(copy, len, &ps);
			html_set_pictures(pics && ps.n ? &ps : NULL);
			for (opts = 0; opts < 4; ++opts) {
				doc_t a, b;
				char why[128];
				html_set_options(opts);
				doc_init(&a);
				doc_init(&b);
				html_to_doc(&a, copy, len, d.ns, d.url, z.content_ns == 'C');
				html_to_doc(&b, buf, (uint32_t)sl, d.ns, d.url, z.content_ns == 'C');
				if (!same_doc(&a, &b, why, sizeof(why))) {
					if (bad < 20)
						printf("DIFF %c/%s (footnotes %d, citations %d, pictures %d): %s\n",
						       d.ns, d.url, opts & 1, (opts >> 1) & 1, pics, why);
					bad++;
					opts = 4;
					pics = 2;
				}
				doc_free(&a);
				doc_free(&b);
			}
		}
		free(copy);
	}
	if (isatty(2))
		fprintf(stderr, "\r%40s\r", "");
	printf("%u articles, HTML %llu -> %llu bytes (%.0f%% smaller), %u differ\n", n,
	       (unsigned long long)before, (unsigned long long)after,
	       before ? 100.0 - 100.0 * (double)after / (double)before : 0.0, bad);
	return bad ? 1 : 0;
}
