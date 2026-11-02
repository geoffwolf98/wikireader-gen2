/*
 * render FILE.zim URL OUT.pbm [SCREENS]
 *   lays out an article and writes the first SCREENS screens (default 3)
 *   stacked vertically, 240 px wide, as a PBM image.  Also prints timing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/zim.h"
#include "../src/doc.h"
#include "../src/layout.h"
#include "../host/hostio.h"

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	host_files_t f;
	zim_t z;
	uint32_t idx, len;
	zim_dirent_t de;
	const uint8_t *p;
	doc_t d;
	layout_t l;
	int screens = argc > 4 ? atoi(argv[4]) : 3, s, y;
	double t0, t1, t2, t3;
	uint8_t *buf;
	FILE *o;

	if (argc < 4 || host_files_open(&f, argv[1]) < 0 || zim_open(&z, &f.io) < 0)
		return 1;
	if (zim_find(&z, z.content_ns, argv[2], &idx) < 0 || zim_follow(&z, idx, &idx, &de) < 0)
		return 1;
	t0 = now();
	if (zim_blob(&z, de.cluster, de.blob, &p, &len) < 0)
		return 1;
	t1 = now();
	doc_init(&d);
	html_to_doc(&d, (const char *)p, len, de.ns, de.url, z.content_ns == 'C');
	t2 = now();
	lay_build(&l, &d);
	t3 = now();
	if (screens <= 0)
		screens = (l.height + SCREEN_H - 1) / SCREEN_H;
	buf = calloc((size_t)screens * SCREEN_H, SCREEN_STRIDE);
	for (s = 0; s < screens; ++s)
		lay_blit(&l, s * SCREEN_H, buf, s * SCREEN_H, SCREEN_H);
	o = fopen(argv[3], "wb");
	fprintf(o, "P4\n%d %d\n", SCREEN_W, screens * SCREEN_H);
	for (y = 0; y < screens * SCREEN_H; ++y)
		fwrite(buf + y * SCREEN_STRIDE, 1, SCREEN_W / 8, o);
	fclose(o);
	fprintf(stderr, "%s: html %u B, decompress %.1f ms, convert %.2f ms, layout %.2f ms, "
		"%u lines, %u frags, height %d px (%.1f screens)\n", argv[2], len,
		(t1 - t0) * 1e3, (t2 - t1) * 1e3, (t3 - t2) * 1e3, l.nlines, l.nfrags,
		l.height, l.height / (double)SCREEN_H);
	return 0;
}
