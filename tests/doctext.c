/*
 * doctext FILE.zim [URL]     print the converted document as text
 * doctext --all FILE.zim     convert every article, report problems
 *                            and link resolution success
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/zim.h"
#include "../src/doc.h"
#include "../host/hostio.h"

static const char *kinds[] = { "P", "TITLE", "H2", "H3", "H4", "ITEM", "DT", "DD",
			       "QUOTE", "ROW", "CAPTION", "RULE", "SMALL" };

static void print_doc(const doc_t *d)
{
	uint32_t b, r;
	for (b = 0; b < d->nblocks; ++b) {
		const doc_block_t *bl = &d->blocks[b];
		printf("%-7s%*s", kinds[bl->kind], bl->indent * 2, "");
		if (bl->kind == BK_ITEM)
			printf(bl->number ? "%u. " : "* ", bl->number);
		for (r = 0; r < bl->nruns; ++r) {
			const doc_run_t *ru = &d->runs[bl->first_run + r];
			int f = ru->font;
			const char *o = f == 1 ? "**" : f == 2 ? "_" : f == 3 ? "**_" : "";
			if (ru->flags & RF_LINK)
				printf("[");
			printf("%s%.*s%s", o, (int)ru->len, d->text + ru->start, o);
			if (ru->flags & RF_LINK) {
				char ns;
				printf("](%s)", doc_link(d, ru->link, &ns));
			}
		}
		printf("\n");
	}
}

int main(int argc, char **argv)
{
	host_files_t f;
	zim_t z;
	int all = argc > 1 && strcmp(argv[1], "--all") == 0;
	const char *file = argv[all ? 2 : 1];
	uint32_t idx, i;
	zim_dirent_t de;
	const uint8_t *p;
	uint32_t len;

	if (getenv("ZW_OPTS"))		/* 1 footnotes, 2 citations, 3 both */
		html_set_options(atoi(getenv("ZW_OPTS")));
	if (host_files_open(&f, file) < 0 || zim_open(&z, &f.io) < 0)
		return 1;
	if (all) {
		uint32_t arts = 0, links = 0, ok = 0, bytes_in = 0, bytes_txt = 0, blocks = 0;
		for (i = 0; i < z.entry_count; ++i) {
			doc_t d;
			uint32_t k;
			if (zim_dirent(&z, i, &de) < 0 || de.ns != z.content_ns ||
			    de.mime != z.html_mime)
				continue;
			if (zim_blob(&z, de.cluster, de.blob, &p, &len) < 0)
				continue;
			doc_init(&d);
			if (html_to_doc(&d, (const char *)p, len, de.ns, de.url,
					z.content_ns == 'C') < 0) {
				printf("convert failed: %s\n", de.url);
			}
			arts++;
			bytes_in += len;
			bytes_txt += d.text_len;
			blocks += d.nblocks;
			for (k = 0; k < d.nlinks; ++k) {
				char ns;
				const char *u = doc_link(&d, k, &ns);
				uint32_t t;
				links++;
				if (zim_find(&z, ns, u, &t) == 0)
					ok++;
				else if (links - ok <= 15)
					printf("unresolved link in %s: %c/%s\n", de.url, ns, u);
			}
			doc_free(&d);
		}
		printf("%u articles, %u KB html -> %u KB text, %u blocks, %u links, %u resolve (%.2f%%)\n",
		       arts, bytes_in / 1024, bytes_txt / 1024, blocks, links, ok,
		       links ? 100.0 * ok / links : 100.0);
		return 0;
	}
	if (argc > 2) {
		if (zim_find(&z, z.content_ns, argv[2], &idx) < 0) {
			fprintf(stderr, "no such entry\n");
			return 1;
		}
	} else {
		idx = z.main_page;
	}
	if (zim_follow(&z, idx, &idx, &de) < 0 || zim_blob(&z, de.cluster, de.blob, &p, &len) < 0)
		return 1;
	{
		doc_t d;
		doc_init(&d);
		html_to_doc(&d, (const char *)p, len, de.ns, de.url, z.content_ns == 'C');
		print_doc(&d);
		fprintf(stderr, "html %u bytes -> text %u bytes, %u blocks, %u runs, %u links\n",
			len, d.text_len, d.nblocks, d.nruns, d.nlinks);
		doc_free(&d);
	}
	return 0;
}
