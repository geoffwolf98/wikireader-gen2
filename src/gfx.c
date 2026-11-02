/*
 * zimwiki - 1 bit per pixel drawing
 */
#include "gfx.h"

static void plot_byte(uint8_t *d, uint8_t m, int colour)
{
	if (colour == BLACK)
		*d |= m;
	else if (colour == WHITE)
		*d &= (uint8_t)~m;
	else
		*d ^= m;
}

void gfx_blit(uint8_t *buf, int stride, int rows, int x, int y,
	      const uint8_t *bits, int w, int h, int colour)
{
	int nb = (w + 7) >> 3, r, maxx = stride * 8;

	for (r = 0; r < h; ++r) {
		const uint8_t *src = bits + r * nb;
		int yy = y + r, i;
		uint8_t *row;
		if (yy < 0 || yy >= rows)
			continue;
		row = buf + yy * stride;
		if (x >= 0 && x + w <= maxx) {
			/* fast path: shift whole source bytes into place */
			int sh = x & 7;
			uint8_t *d = row + (x >> 3);
			for (i = 0; i < nb; ++i) {
				uint8_t s = src[i];
				if (i == nb - 1 && (w & 7))
					s &= (uint8_t)(0xff << (8 - (w & 7)));
				if (!s)
					continue;
				plot_byte(d + i, (uint8_t)(s >> sh), colour);
				if (sh && x + i * 8 + 8 < maxx + 8 && (x >> 3) + i + 1 < stride)
					plot_byte(d + i + 1, (uint8_t)(s << (8 - sh)), colour);
			}
		} else {
			for (i = 0; i < w; ++i) {
				int xx = x + i;
				if (xx < 0 || xx >= maxx)
					continue;
				if (src[i >> 3] & (0x80 >> (i & 7)))
					plot_byte(row + (xx >> 3), (uint8_t)(0x80 >> (xx & 7)), colour);
			}
		}
	}
}

void gfx_hline(uint8_t *buf, int stride, int rows, int x, int y, int w, int colour)
{
	int maxx = stride * 8;
	uint8_t *row;
	if (y < 0 || y >= rows)
		return;
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (x + w > maxx)
		w = maxx - x;
	if (w <= 0)
		return;
	row = buf + y * stride;
	while (w > 0 && (x & 7)) {
		plot_byte(row + (x >> 3), (uint8_t)(0x80 >> (x & 7)), colour);
		x++;
		w--;
	}
	while (w >= 8) {
		plot_byte(row + (x >> 3), 0xff, colour);
		x += 8;
		w -= 8;
	}
	if (w > 0)
		plot_byte(row + (x >> 3), (uint8_t)(0xff << (8 - w)), colour);
}

void gfx_dotted(uint8_t *buf, int stride, int rows, int x, int y, int w)
{
	int i;
	if (y < 0 || y >= rows)
		return;
	for (i = 0; i < w; i += 2) {
		int xx = x + i;
		if (xx >= 0 && xx < stride * 8)
			buf[y * stride + (xx >> 3)] |= (uint8_t)(0x80 >> (xx & 7));
	}
}

void gfx_fill(uint8_t *buf, int stride, int rows, int x, int y, int w, int h, int colour)
{
	int r;
	for (r = 0; r < h; ++r)
		gfx_hline(buf, stride, rows, x, y + r, w, colour);
}

void gfx_rect(uint8_t *buf, int stride, int rows, int x, int y, int w, int h, int colour)
{
	int r;
	gfx_hline(buf, stride, rows, x, y, w, colour);
	gfx_hline(buf, stride, rows, x, y + h - 1, w, colour);
	for (r = 1; r < h - 1; ++r) {
		gfx_hline(buf, stride, rows, x, y + r, 1, colour);
		gfx_hline(buf, stride, rows, x + w - 1, y + r, 1, colour);
	}
}
