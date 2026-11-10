/*
 * zimwiki - line layout and strip rendering (see layout.h)
 */
#include "layout.h"
#include "font.h"
#include "utf8.h"

#define MARGIN_L   4
#define MARGIN_R   5
#define INDENT_W   12

typedef struct builder {
	layout_t *l;
	int32_t y;
	int x0, right;		/* text area of the current block */
	int x;			/* next free x on the current line */
	int line_open;
	int pending_space;	/* width of a space waiting before the next word */
	uint32_t line_index;
	int err;
} builder_t;

static int grow(void **p, uint32_t *cap, uint32_t need, uint32_t elem)
{
	uint32_t ncap;
	void *n;
	if (need <= *cap)
		return 0;
	ncap = *cap ? *cap * 2 : 256;
	while (ncap < need)
		ncap *= 2;
	n = zw_realloc(*p, (size_t)*cap * elem, (size_t)ncap * elem);
	if (!n)
		return -1;
	*p = n;
	*cap = ncap;
	return 0;
}

static int space_before(int kind, int prev)
{
	switch (kind) {
	case BK_TITLE: return 2;
	case BK_H2: return 12;
	case BK_H3: return 9;
	case BK_H4: return 7;
	case BK_ITEM: return prev == BK_ITEM || prev == BK_PARA ? 2 : 5;
	case BK_DT: return 5;
	case BK_DD: return 1;
	case BK_MROW: return 0;
	case BK_ROW: return prev == BK_ROW || prev == BK_CAPTION ? 2 : 7;
	case BK_CAPTION: return 7;
	case BK_RULE: return 5;
	case BK_SMALL: return 10;
	}
	return prev >= BK_TITLE && prev <= BK_H4 ? 3 : 6;
}

static void new_line(builder_t *b, int kind)
{
	layout_t *l = b->l;
	lay_line_t *ln;
	if (grow((void **)&l->lines, &l->lines_cap, l->nlines + 1, sizeof(lay_line_t))) {
		b->err = ZW_ENOMEM;
		return;
	}
	ln = &l->lines[l->nlines];
	memset(ln, 0, sizeof(*ln));
	ln->y = b->y;
	ln->first = l->nfrags;
	ln->kind = (uint8_t)kind;
	b->line_index = l->nlines++;
	b->line_open = 1;
	b->x = b->x0;
	b->pending_space = 0;
}

static void end_line(builder_t *b)
{
	layout_t *l = b->l;
	lay_line_t *ln;
	uint32_t i;
	int asc = 0, desc = 0;
	if (!b->line_open)
		return;
	ln = &l->lines[b->line_index];
	for (i = 0; i < ln->nfrag; ++i) {
		lay_frag_t *f = &l->frags[ln->first + i];
		int a = font_ascent(f->font) - f->dy;
		int dsc = font_height(f->font) - font_ascent(f->font) + f->dy;
		if (a > asc)
			asc = a;
		if (dsc > desc)
			desc = dsc;
	}
	if (!ln->nfrag) {
		int ft = b->l->large ? FONT_TEXTL : FONT_TEXT;
		asc = font_ascent(ft);
		desc = font_height(ft) - asc;
	}
	ln->base = (uint16_t)asc;
	ln->h = (uint16_t)(asc + desc);
	b->y += ln->h;
	b->line_open = 0;
}

/* the font a run is drawn in: the large set when the Text size switch
 * was on when this layout began */
#define RFONT(b, r) ((b)->l->large ? font_big((r)->font) : (r)->font)

static int lay_large_now;

void lay_set_large(int on)
{
	lay_large_now = on != 0;
}

static void add_frag(builder_t *b, const doc_run_t *r, uint32_t off, uint32_t len, int w)
{
	layout_t *l = b->l;
	lay_line_t *ln = &l->lines[b->line_index];
	lay_frag_t *f;
	int dy = (r->flags & RF_SUP) ? (b->l->large ? -6 : -5) :
		 (r->flags & RF_SUB) ? (b->l->large ? 4 : 3) : 0;

	/* extend the previous fragment when contiguous and same style */
	if (ln->nfrag) {
		f = &l->frags[l->nfrags - 1];
		if (f->off + f->len == off && f->font == RFONT(b, r) && f->link == r->link &&
		    f->dy == dy && f->x + f->w == b->x && f->len + len < 65000) {
			f->len = (uint16_t)(f->len + len);
			f->w = (uint16_t)(f->w + w);
			b->x += w;
			return;
		}
	}
	if (grow((void **)&l->frags, &l->frags_cap, l->nfrags + 1, sizeof(lay_frag_t))) {
		b->err = ZW_ENOMEM;
		return;
	}
	f = &l->frags[l->nfrags++];
	f->x = (int16_t)b->x;
	f->w = (uint16_t)w;
	f->off = off;
	f->len = (uint16_t)len;
	f->link = r->link;
	f->font = (uint8_t)RFONT(b, r);
	f->dy = (int8_t)dy;
	f->pad = 0;
	ln->nfrag++;
	b->x += w;
}

static int is_break_cp(uint32_t cp)
{
	/* CJK and similar scripts may break between any two characters */
	return (cp >= 0x2e80 && cp <= 0x9fff) || (cp >= 0xac00 && cp <= 0xd7af) ||
	       (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xff00 && cp <= 0xffef);
}

typedef struct seg {
	const doc_run_t *r;
	uint32_t off, len;
	int w;
} seg_t;

#define MAX_SEGS 24

/* place one word (no spaces) of run r, possibly made of several
 * segments in different styles ("(" + link + ")") */
static void place_word(builder_t *b, int kind, seg_t *segs, int nseg)
{
	const char *t = b->l->doc->text;
	int w = 0, i;

	for (i = 0; i < nseg; ++i)
		w += segs[i].w;
	if (b->line_open && b->x + b->pending_space + w > b->right && b->x > b->x0) {
		end_line(b);
		new_line(b, kind);
	}
	if (!b->line_open)
		new_line(b, kind);
	if (b->err)
		return;
	if (b->pending_space && b->x > b->x0) {
		lay_line_t *ln = &b->l->lines[b->line_index];
		lay_frag_t *f = ln->nfrag ? &b->l->frags[b->l->nfrags - 1] : NULL;
		uint32_t off = segs[0].off;
		/* fold the space into the previous fragment when it sits
		 * between the two words in the text, so a line of plain text
		 * stays one fragment */
		if (f && f->off + f->len + 1 == off && t[off - 1] == ' ' &&
		    f->x + f->w == b->x && f->len < 65000) {
			f->len++;
			f->w = (uint16_t)(f->w + b->pending_space);
		}
		b->x += b->pending_space;
	}
	b->pending_space = 0;

	if (b->x + w <= b->right) {
		for (i = 0; i < nseg; ++i)
			add_frag(b, segs[i].r, segs[i].off, segs[i].len, segs[i].w);
		return;
	}
	/* too long for a line on its own: break between characters */
	for (i = 0; i < nseg && !b->err; ++i) {
		const doc_run_t *r = segs[i].r;
		const uint8_t *p = (const uint8_t *)t + segs[i].off, *end = p + segs[i].len;
		while (p < end && !b->err) {
			const uint8_t *s0 = p;
			int cw = 0;
			while (p < end) {
				const uint8_t *q = p;
				glyph_t g;
				uint32_t cp = utf8_next(&q, end);
				font_glyph(RFONT(b, r), cp, &g);
				if (b->x + cw + g.adv > b->right && (cw || b->x > b->x0))
					break;
				cw += g.adv;
				p = q;
			}
			if (p > s0)
				add_frag(b, r, (uint32_t)((const char *)s0 - t), (uint32_t)(p - s0), cw);
			if (p < end) {
				end_line(b);
				new_line(b, kind);
			}
		}
	}
}

static void layout_block(builder_t *b, const doc_block_t *bl, int prev_kind)
{
	const doc_t *d = b->l->doc;
	uint32_t i;
	int indent = bl->indent * INDENT_W;
	int kind = bl->kind;
	uint32_t first_line;

	if (indent > 72)
		indent = 72;
	b->y += space_before(kind, prev_kind);
	b->x0 = MARGIN_L + indent;
	b->right = LAYOUT_WIDTH - MARGIN_R;
	if (kind == BK_ITEM)
		b->x0 += b->l->large ? (bl->number ? 17 : 11) : (bl->number ? 14 : 9);
	if (kind == BK_QUOTE)
		b->right -= 8;
	first_line = b->l->nlines;

	if (kind == BK_RULE) {
		new_line(b, kind);
		if (b->err)
			return;
		b->l->lines[b->line_index].deco = LD_RULE;
		end_line(b);
		b->l->lines[b->line_index].h = 5;
		b->l->lines[b->line_index].base = 2;
		b->y = b->l->lines[b->line_index].y + 5;
		return;
	}

	{
		seg_t segs[MAX_SEGS];
		int nseg = 0;
		for (i = 0; i < bl->nruns && !b->err; ++i) {
			const doc_run_t *r = &d->runs[bl->first_run + i];
			const uint8_t *p = (const uint8_t *)d->text + r->start;
			const uint8_t *end = p + r->len;
			glyph_t sp;
			const uint8_t *adv = font_ascii_adv(RFONT(b, r));
			font_glyph(RFONT(b, r), ' ', &sp);
			while (p < end && !b->err) {
				const uint8_t *w0;
				int brk = 0, aw = 0;
				if (*p == ' ') {
					if (nseg) {
						place_word(b, kind, segs, nseg);
						nseg = 0;
					}
					b->pending_space = sp.adv;
					p++;
					continue;
				}
				w0 = p;
				/* plain ASCII needs no decoding and never breaks, and
				 * its width comes straight from the table */
				while (p < end && *p != ' ' && *p < 0x80) {
					aw += adv ? adv[*p] : 0;
					p++;
				}
				if (p == end || *p == ' ') {
					if (nseg == MAX_SEGS) {
						place_word(b, kind, segs, nseg);
						nseg = 0;
					}
					segs[nseg].r = r;
					segs[nseg].off = (uint32_t)((const char *)w0 - d->text);
					segs[nseg].len = (uint32_t)(p - w0);
					segs[nseg].w = adv ? aw : font_width(RFONT(b, r), (const char *)w0, (size_t)(p - w0));
					nseg++;
					continue;
				}
				while (p < end && *p != ' ') {
					const uint8_t *q = p;
					uint32_t cp = utf8_next(&q, end);
					if (is_break_cp(cp) && p > w0) {
						brk = 1;
						break;
					}
					p = q;
					if (is_break_cp(cp)) {
						brk = 1;
						break;
					}
				}
				if (nseg == MAX_SEGS) {
					place_word(b, kind, segs, nseg);
					nseg = 0;
				}
				segs[nseg].r = r;
				segs[nseg].off = (uint32_t)((const char *)w0 - d->text);
				segs[nseg].len = (uint32_t)(p - w0);
				segs[nseg].w = font_width(RFONT(b, r), (const char *)w0, (size_t)(p - w0));
				nseg++;
				if (brk) {
					place_word(b, kind, segs, nseg);
					nseg = 0;
				}
			}
		}
		if (nseg && !b->err)
			place_word(b, kind, segs, nseg);
	}
	end_line(b);
	if (b->l->nlines > first_line) {
		lay_line_t *ln = &b->l->lines[first_line];
		if (kind == BK_ITEM) {
			ln->deco = bl->number ? LD_NUMBER : LD_BULLET;
			ln->number = bl->number;
			ln->deco_x = (int16_t)(MARGIN_L + indent);
		}
		if (kind == BK_H2 || kind == BK_TITLE) {
			lay_line_t *last = &b->l->lines[b->l->nlines - 1];
			last->deco = LD_HEAD_RULE;
			b->y += 3;
			last->h += 3;
		}
	}
}

int lay_begin(layout_t *l, const doc_t *d)
{
	uint32_t i;
	memset(l, 0, sizeof(*l));
	l->doc = d;
	l->large = (uint8_t)lay_large_now;
	for (i = 0; i < STRIP_CACHE; ++i)
		l->strip_id[i] = -1;
	l->prefetched_top = -1;
	l->prev_kind = BK_TITLE;
	l->strip_mem = zw_alloc(STRIP_CACHE * STRIP_H * SCREEN_STRIDE);
	if (!l->strip_mem)
		return ZW_ENOMEM;
	if (!d->nblocks) {
		l->done = 1;
		l->height = 12;
	}
	return ZW_OK;
}

int lay_more(layout_t *l, int32_t until_y, uint32_t max_blocks)
{
	const doc_t *d = l->doc;
	builder_t b;
	uint32_t n = 0, i;
	int32_t old_y = l->built_y;

	if (l->done)
		return ZW_OK;
	memset(&b, 0, sizeof(b));
	b.l = l;
	b.y = l->built_y;
	while (l->next_block < d->nblocks && !b.err &&
	       (until_y < 0 || b.y < until_y) && (!max_blocks || n < max_blocks)) {
		i = l->next_block;
		if ((i & 63) == 63)
			ZW_PROGRESS(3, i, d->nblocks);
		layout_block(&b, &d->blocks[i], i ? l->prev_kind : -1);
		l->prev_kind = d->blocks[i].kind;
		l->next_block++;
		n++;
	}
	l->built_y = b.y;
	if (l->next_block >= d->nblocks || b.err)
		l->done = 1;	/* out of memory: keep what fits */
	l->height = l->done ? b.y + 12 : b.y;
	/* strips drawn over the old, unfinished end must be drawn again */
	for (i = 0; i < STRIP_CACHE; ++i)
		if (l->strip_id[i] >= 0 && (l->strip_id[i] + 1) * STRIP_H > old_y)
			l->strip_id[i] = -1;
	if (n)
		l->prefetched_top = -1;
	return b.err;
}

int lay_build(layout_t *l, const doc_t *d)
{
	int r = lay_begin(l, d);
	if (r < 0)
		return r;
	return lay_more(l, -1, 0);
}

void lay_free(layout_t *l)
{
	zw_free(l->lines);
	zw_free(l->frags);
	zw_free(l->strip_mem);
	memset(l, 0, sizeof(*l));
}

/* first line whose bottom is below y */
static uint32_t line_at(const layout_t *l, int32_t y)
{
	uint32_t lo = 0, hi = l->nlines;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (l->lines[mid].y + l->lines[mid].h <= y)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void draw_number(uint8_t *buf, int rows, int x_right, int base, int n, int font)
{
	char s[8];
	int k = 0, i;
	char t[8];
	if (n <= 0)
		return;
	if (n & 0x4000) {
		/* notes: a. ... z. aa. ab. ... */
		n &= 0x3fff;
		while (n && k < 6) {
			n--;
			t[k++] = (char)('a' + n % 26);
			n /= 26;
		}
	} else
	while (n && k < 6) {
		t[k++] = (char)('0' + n % 10);
		n /= 10;
	}
	for (i = 0; i < k; ++i)
		s[i] = t[k - 1 - i];
	s[k++] = '.';
	font_draw(buf, SCREEN_STRIDE, rows, x_right - font_width(font, s, (size_t)k),
		  base, font, s, (size_t)k, BLACK);
}

static void render_strip(layout_t *l, int32_t sid, uint8_t *buf)
{
	int32_t top = sid * STRIP_H;
	uint32_t i = line_at(l, top);
	const char *t = l->doc->text;

	memset(buf, 0, STRIP_H * SCREEN_STRIDE);
	for (; i < l->nlines; ++i) {
		const lay_line_t *ln = &l->lines[i];
		int y = ln->y - top;
		int base = y + ln->base;
		uint32_t k;
		if (ln->y >= top + STRIP_H)
			break;
		for (k = 0; k < ln->nfrag; ++k) {
			const lay_frag_t *f = &l->frags[ln->first + k];
			font_draw(buf, SCREEN_STRIDE, STRIP_H, f->x, base + f->dy, f->font,
				  t + f->off, f->len, BLACK);
			if (f->link != 0xffff)
				gfx_dotted(buf, SCREEN_STRIDE, STRIP_H, f->x, base + f->dy + 2, f->w);
		}
		switch (ln->deco) {
		case LD_BULLET:
			if (l->large)
				gfx_fill(buf, SCREEN_STRIDE, STRIP_H, ln->deco_x + 2, base - 7, 4, 4, BLACK);
			else
				gfx_fill(buf, SCREEN_STRIDE, STRIP_H, ln->deco_x + 2, base - 6, 3, 3, BLACK);
			break;
		case LD_NUMBER:
			draw_number(buf, STRIP_H, ln->deco_x + (l->large ? 15 : 12), base, ln->number,
				    l->large ? FONT_TEXTL : FONT_TEXT);
			break;
		case LD_RULE:
			gfx_hline(buf, SCREEN_STRIDE, STRIP_H, MARGIN_L + 20, y + 2,
				  LAYOUT_WIDTH - MARGIN_L - MARGIN_R - 40, BLACK);
			break;
		case LD_HEAD_RULE:
			gfx_hline(buf, SCREEN_STRIDE, STRIP_H, MARGIN_L, y + ln->h - 2,
				  LAYOUT_WIDTH - MARGIN_L - MARGIN_R, BLACK);
			break;
		}
		if (ln->kind == BK_QUOTE)
			gfx_fill(buf, SCREEN_STRIDE, STRIP_H, MARGIN_L + 2, y, 2, ln->h, BLACK);
	}
}

static uint8_t *strip(layout_t *l, int32_t sid)
{
	int i, victim = 0;
	for (i = 0; i < STRIP_CACHE; ++i) {
		if (l->strip_id[i] == sid) {
			l->strip_use[i] = ++l->clock;
			return l->strip_mem + i * STRIP_H * SCREEN_STRIDE;
		}
		if (l->strip_use[i] < l->strip_use[victim])
			victim = i;
	}
	render_strip(l, sid, l->strip_mem + victim * STRIP_H * SCREEN_STRIDE);
	l->strip_id[victim] = sid;
	l->strip_use[victim] = ++l->clock;
	return l->strip_mem + victim * STRIP_H * SCREEN_STRIDE;
}

int lay_blit(layout_t *l, int32_t top, uint8_t *buf, int y0, int rows)
{
	int r = 0;
	if (!l->strip_mem)
		return ZW_ENOMEM;
	while (r < rows) {
		int32_t y = top + r;
		if (y < 0 || y >= l->height) {
			memset(buf + (y0 + r) * SCREEN_STRIDE, 0, SCREEN_STRIDE);
			r++;
			continue;
		}
		{
			int32_t sid = y / STRIP_H;
			int off = (int)(y - sid * STRIP_H);
			int n = STRIP_H - off;
			const uint8_t *s = strip(l, sid);
			if (n > rows - r)
				n = rows - r;
			if (y + n > l->height)
				n = (int)(l->height - y);
			memcpy(buf + (y0 + r) * SCREEN_STRIDE, s + off * SCREEN_STRIDE,
			       (size_t)n * SCREEN_STRIDE);
			r += n;
		}
	}
	return ZW_OK;
}

void lay_prefetch(layout_t *l, int32_t top, int rows)
{
	int32_t a = (top - STRIP_H * 2) / STRIP_H, z = (top + rows + STRIP_H * 2) / STRIP_H, s;
	if (!l->strip_mem || top == l->prefetched_top)
		return;
	if (a < 0)
		a = 0;
	for (s = a; s <= z && s * STRIP_H < l->height; ++s) {
		int i, have = 0;
		for (i = 0; i < STRIP_CACHE; ++i)
			if (l->strip_id[i] == s)
				have = 1;
		if (!have) {
			(void)strip(l, s);
			return;	/* one strip per call keeps idle work short */
		}
	}
	l->prefetched_top = top;	/* nothing left to do here */
}

int lay_hit(const layout_t *l, int x, int32_t y, int *rx, int32_t *ry, int *rw, int *rh)
{
	uint32_t i = line_at(l, y - 6);
	int best = -1, best_d = 1 << 20;
	for (; i < l->nlines && l->lines[i].y <= y + 6; ++i) {
		const lay_line_t *ln = &l->lines[i];
		uint32_t k;
		int dy = y < ln->y ? ln->y - y : y >= ln->y + ln->h ? y - (ln->y + ln->h) + 1 : 0;
		for (k = 0; k < ln->nfrag; ++k) {
			const lay_frag_t *f = &l->frags[ln->first + k];
			int dx;
			if (f->link == 0xffff)
				continue;
			dx = x < f->x ? f->x - x : x >= f->x + f->w ? x - (f->x + f->w) + 1 : 0;
			if (dx > 6)
				continue;
			if (dx + dy * 2 < best_d) {
				best_d = dx + dy * 2;
				best = f->link;
				*rx = f->x;
				*ry = ln->y;
				*rw = f->w;
				*rh = ln->h;
			}
		}
	}
	return best;
}

int32_t lay_next_heading(const layout_t *l, int32_t y, int dir)
{
	uint32_t i = line_at(l, y);
	if (dir > 0) {
		for (i = i + 1; i < l->nlines; ++i)
			if (l->lines[i].kind == BK_H2 || l->lines[i].kind == BK_H3)
				if (l->lines[i].y > y + 2)
					return l->lines[i].y - 4;
	} else {
		while (i > 0) {
			--i;
			if ((l->lines[i].kind == BK_H2 || l->lines[i].kind == BK_H3 ||
			     l->lines[i].kind == BK_TITLE) && l->lines[i].y < y - 4)
				return l->lines[i].y - 4 < 0 ? 0 : l->lines[i].y - 4;
		}
		return 0;
	}
	return -1;
}
