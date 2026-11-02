/*
 * zimwiki - 1 bit per pixel drawing.  Same layout as the WikiReader
 * frame buffer: rows of `stride` bytes, MSB is the leftmost pixel,
 * a set bit is black.
 */
#ifndef GFX_H
#define GFX_H

#include "zw.h"

#define SCREEN_W      240
#define SCREEN_H      208
#define SCREEN_STRIDE 32	/* bytes per row (256 pixel buffer width) */

enum { WHITE = 0, BLACK = 1, INVERT = 2 };

/* blit a glyph-style bitmap (rows of ceil(w/8) bytes) */
void gfx_blit(uint8_t *buf, int stride, int rows, int x, int y,
	      const uint8_t *bits, int w, int h, int colour);
void gfx_fill(uint8_t *buf, int stride, int rows, int x, int y, int w, int h, int colour);
void gfx_hline(uint8_t *buf, int stride, int rows, int x, int y, int w, int colour);
void gfx_dotted(uint8_t *buf, int stride, int rows, int x, int y, int w);
void gfx_rect(uint8_t *buf, int stride, int rows, int x, int y, int w, int h, int colour);

#endif
