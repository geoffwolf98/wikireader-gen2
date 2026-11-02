/*
 * zimwiki - bitmap fonts
 */
#include "font.h"
#include "gfx.h"
#include "utf8.h"

static font_t fallback;
/* the fallback font is loaded the first time a character needs it, so
 * start-up does not wait for it (most pages never do) */
static int (*fallback_loader)(void);
static int fallback_tried;

void font_set_fallback_loader(int (*fn)(void))
{
	fallback_loader = fn;
	fallback_tried = 0;
}

int font_set_fallback(const uint8_t *data, uint32_t len)
{
	uint32_t n;
	if (len < 12 || memcmp(data, "ZWFT", 4) != 0)
		return ZW_EFORMAT;
	n = zw_rd32(data + 4);
	if (n > 65536 || 12 + n * GLYPH_REC > len)
		return ZW_EFORMAT;
	fallback.glyphs = data + 12;
	fallback.bits = data + 12 + n * GLYPH_REC;
	fallback.count = (uint16_t)(n > 65535 ? 65535 : n);
	fallback.ascent = data[8];
	fallback.descent = data[9];
	return ZW_OK;
}

/* direct index for Latin-1, filled on first use (0 = not looked up,
 * 0xffff = absent, else index + 1) */
static uint16_t latin1[FONT_COUNT][256];

static const uint8_t *find_slow(const font_t *f, uint32_t cp);

static const uint8_t *find(const font_t *f, uint32_t cp)
{
	int fi = (int)(f - fonts);
	if (cp < 256 && fi >= 0 && fi < FONT_COUNT) {
		uint16_t v = latin1[fi][cp];
		if (!v) {
			const uint8_t *r = find_slow(f, cp);
			v = r ? (uint16_t)((r - f->glyphs) / GLYPH_REC + 1) : 0xffff;
			latin1[fi][cp] = v;
		}
		return v == 0xffff ? NULL : f->glyphs + (uint32_t)(v - 1) * GLYPH_REC;
	}
	return find_slow(f, cp);
}

static const uint8_t *find_slow(const font_t *f, uint32_t cp)
{
	uint32_t lo = 0, hi = f->count;
	if (cp > 0xffff || !f->count)
		return NULL;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		uint32_t c = zw_rd16(f->glyphs + mid * GLYPH_REC);
		if (c < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < f->count && zw_rd16(f->glyphs + lo * GLYPH_REC) == cp)
		return f->glyphs + lo * GLYPH_REC;
	return NULL;
}

static void fill(const font_t *f, const uint8_t *r, glyph_t *g)
{
	g->adv = r[2];
	g->xoff = (int8_t)r[3];
	g->top = (int8_t)r[4];
	g->w = r[5];
	g->h = r[6];
	g->bits = f->bits + zw_rd32(r + 8);
}

/* full line height bracket pieces for display matrices, drawn rather
 * than taken from a font so the rows join up: FONT_PIECE + 0..13 is
 * [ top, middle, bottom, ] top, middle, bottom, ( top, middle, bottom,
 * ) top, middle, bottom, |, || */
#define PIECE_MAX_H 32
static uint8_t piece_bits[14][PIECE_MAX_H];
static int piece_h;

static void make_pieces(void)
{
	int h = fonts[FONT_TEXT].ascent + fonts[FONT_TEXT].descent, k, y;
	if (h > PIECE_MAX_H)
		h = PIECE_MAX_H;
	piece_h = h;
	memset(piece_bits, 0, sizeof(piece_bits));
	for (y = 0; y < h; ++y) {
		/* square brackets: bar plus a tick at the end */
		for (k = 0; k < 3; ++k) {
			int top = k == 0 && y < 1, bot = k == 2 && y >= h - 1;
			if (top || bot)
				continue;
			piece_bits[k][y] = 0x40;		/* x = 1 */
			piece_bits[3 + k][y] = 0x20;		/* x = 2 */
			if ((k == 0 && y == 1) || (k == 2 && y == h - 2)) {
				piece_bits[k][y] |= 0x70;	/* x = 1..3 */
				piece_bits[3 + k][y] |= 0xe0;	/* x = 0..2 */
			}
		}
		/* round brackets: curve in at the two ends */
		piece_bits[6][y] = y < 3 ? (y == 0 ? 0x10 : 0x20) : 0x40;
		piece_bits[7][y] = 0x40;
		piece_bits[8][y] = y >= h - 3 ? (y == h - 1 ? 0x10 : 0x20) : 0x40;
		piece_bits[9][y] = y < 3 ? (y == 0 ? 0x40 : 0x20) : 0x10;
		piece_bits[10][y] = 0x10;
		piece_bits[11][y] = y >= h - 3 ? (y == h - 1 ? 0x40 : 0x20) : 0x10;
		piece_bits[12][y] = 0x40;
		piece_bits[13][y] = 0xa0;
	}
}

void font_glyph(int fi, uint32_t cp, glyph_t *g)
{
	static const uint8_t box_bits[] = {
		0xf8, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0xf8
	};
	const uint8_t *r;

	if (cp >= FONT_PIECE && cp < FONT_PIECE + 14) {
		if (!piece_h)
			make_pieces();
		g->bits = piece_bits[cp - FONT_PIECE];
		g->w = 4;
		g->h = (uint8_t)piece_h;
		g->adv = 5;
		g->xoff = 0;
		g->top = (int8_t)fonts[FONT_TEXT].ascent;
		return;
	}
	if (cp == 0x200a) {	/* hair space: 1 px, used to line up columns */
		memset(g, 0, sizeof(*g));
		g->bits = box_bits;
		g->adv = 1;
		return;
	}

	if ((r = find(&fonts[fi], cp)) != NULL) {
		fill(&fonts[fi], r, g);
		return;
	}
	/* sub and superscripts: small symbols before full size ones */
	if (fi == FONT_SMALL && (r = find(&fonts[FONT_MATHSMALL], cp)) != NULL) {
		fill(&fonts[FONT_MATHSMALL], r, g);
		return;
	}
	/* styled fonts fall back to the regular text font first */
	if (fi != FONT_TEXT && (r = find(&fonts[FONT_TEXT], cp)) != NULL) {
		fill(&fonts[FONT_TEXT], r, g);
		return;
	}
	if ((r = find(&fonts[FONT_EXTRA], cp)) != NULL) {
		fill(&fonts[FONT_EXTRA], r, g);
		return;
	}
	if ((r = find(&fonts[FONT_MATH], cp)) != NULL) {
		fill(&fonts[FONT_MATH], r, g);
		return;
	}
	if (!fallback.count && fallback_loader && !fallback_tried && cp >= 0x370 &&
	    !(cp >= 0x2000 && cp <= 0x200f) && cp != 0x202f && cp != 0xfeff &&
	    !(cp >= 0xfe00 && cp <= 0xfe0f) && !(cp >= 0x202a && cp <= 0x202e) &&
	    !(cp >= 0x2060 && cp <= 0x206f) && cp != 0x061c) {
		fallback_tried = 1;
		fallback_loader();
	}
	if (fallback.count && (r = find(&fallback, cp)) != NULL) {
		fill(&fallback, r, g);
		return;
	}
	if (cp == 0xa0 || cp == 0x2009 || cp == 0x202f || cp == 0x2002 || cp == 0x2003) {
		font_glyph(fi, ' ', g);
		return;
	}
	/* zero width: combining marks, joiners, soft hyphen, controls */
	if ((cp >= 0x300 && cp < 0x370) || cp == 0xad || (cp >= 0x200b && cp <= 0x200f) ||
	    cp == 0xfeff || cp < 0x20 || (cp >= 0xfe00 && cp <= 0xfe0f) ||
	    (cp >= 0x202a && cp <= 0x202e) || (cp >= 0x2060 && cp <= 0x206f) ||
	    cp == 0x061c || (cp >= 0x180b && cp <= 0x180f) || (cp >= 0xfff9 && cp <= 0xfffb) ||
	    (cp >= 0x7f && cp < 0xa0)) {
		memset(g, 0, sizeof(*g));
		return;
	}
	g->adv = 7;
	g->xoff = 1;
	g->top = 9;
	g->w = 5;
	g->h = 8;
	g->bits = box_bits;
}

int font_height(int f)
{
	return fonts[f].ascent + fonts[f].descent + 1;
}

int font_ascent(int f)
{
	return fonts[f].ascent;
}

/* ASCII advances per font, filled on first use */
static uint8_t ascii_adv[FONT_COUNT][128];
static uint8_t ascii_ready[FONT_COUNT];

/* advance of each ASCII character in font f, filled on first use */
const uint8_t *font_ascii_adv(int f)
{
	if (f < 0 || f >= FONT_COUNT)
		return NULL;
	if (!ascii_ready[f]) {
		uint8_t c;
		glyph_t g;
		for (c = 0; c < 128; ++c) {
			font_glyph(f, c, &g);
			ascii_adv[f][c] = g.adv < 0xff ? (uint8_t)g.adv : 0xfe;
		}
		ascii_ready[f] = 1;
	}
	return ascii_adv[f];
}

int font_width(int f, const char *s, size_t len)
{
	const uint8_t *p = (const uint8_t *)s, *end = p + len;
	const uint8_t *adv = font_ascii_adv(f);
	int w = 0;
	while (p < end) {
		glyph_t g;
		uint32_t cp;
		if (*p < 0x80 && adv) {
			w += adv[*p++];
			continue;
		}
		cp = *p < 0x80 ? *p++ : utf8_next(&p, end);
		font_glyph(f, cp, &g);
		w += g.adv;
	}
	return w;
}

int font_draw(uint8_t *buf, int stride, int rows, int x, int baseline,
	      int f, const char *s, size_t len, int colour)
{
	const uint8_t *p = (const uint8_t *)s, *end = p + len;
	while (p < end) {
		glyph_t g;
		uint32_t cp = *p < 0x80 ? *p++ : utf8_next(&p, end);
		font_glyph(f, cp, &g);
		if (g.h)
			gfx_blit(buf, stride, rows, x + g.xoff, baseline - g.top,
				 g.bits, g.w, g.h, colour);
		x += g.adv;
	}
	return x;
}
