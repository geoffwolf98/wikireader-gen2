/*
 * zimwiki - line layout and strip rendering for the article view
 *
 * The document is wrapped into lines once.  Pixels are produced on
 * demand in 64-row strips that are kept in a small cache, so scrolling
 * is a copy of already rendered rows and only newly exposed strips are
 * drawn.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include "doc.h"
#include "gfx.h"

#define STRIP_H       64
#define STRIP_CACHE   24
#define LAYOUT_WIDTH  SCREEN_W

enum line_deco {
	LD_NONE, LD_BULLET, LD_NUMBER, LD_RULE, LD_HEAD_RULE,
};

typedef struct lay_frag {
	int16_t x;
	uint16_t w;
	uint32_t off;		/* bytes in doc text */
	uint16_t len;
	uint16_t link;		/* 0xffff = none */
	uint8_t font;
	int8_t dy;		/* baseline shift (sup/sub) */
	uint16_t pad;
} lay_frag_t;

typedef struct lay_line {
	int32_t y;
	uint16_t h;
	uint16_t base;		/* baseline offset from y */
	uint32_t first;
	uint16_t nfrag;
	uint8_t deco;
	uint8_t kind;
	int16_t deco_x;
	uint16_t number;
} lay_line_t;

typedef struct layout {
	const doc_t *doc;
	lay_line_t *lines;
	uint32_t nlines, lines_cap;
	lay_frag_t *frags;
	uint32_t nfrags, frags_cap;
	int32_t height;		/* total pixel height */
	/* strip cache */
	uint8_t *strip_mem;
	int32_t strip_id[STRIP_CACHE];
	uint32_t strip_use[STRIP_CACHE];
	uint32_t clock;
	int32_t prefetched_top;	/* prefetch finished for this position */
	/* incremental build: long pages are laid out a slice at a time */
	uint32_t next_block;
	int32_t built_y;
	int prev_kind;
	int done;
} layout_t;

/* lay out the whole document */
int lay_build(layout_t *l, const doc_t *d);
/* or start, then add blocks until the laid out height reaches until_y
 * (-1: to the end) or max_blocks were added (0: no limit).  height is
 * what is laid out so far until done is set */
int lay_begin(layout_t *l, const doc_t *d);
int lay_more(layout_t *l, int32_t until_y, uint32_t max_blocks);
void lay_free(layout_t *l);

/* copy rows [top, top+rows) of the rendered document into buf
 * (SCREEN_STRIDE bytes per row, starting at row y0 of buf) */
int lay_blit(layout_t *l, int32_t top, uint8_t *buf, int y0, int rows);

/* make sure the strips around [top, top+rows) are rendered; cheap
 * to call from idle time */
void lay_prefetch(layout_t *l, int32_t top, int rows);

/* link under document point (x, y), with a little slack; returns link
 * index or -1 and the fragment rectangle */
int lay_hit(const layout_t *l, int x, int32_t y, int *rx, int32_t *ry, int *rw, int *rh);

/* y of the first line at or after document position y with kind
 * H2/H3 (for section jumping); returns -1 if none */
int32_t lay_next_heading(const layout_t *l, int32_t y, int dir);

#endif
