/*
 * zimwiki - TeX maths to linear text
 *
 * Wikipedia keeps the TeX source of every formula (the alttext of the
 * MathML, or the alt of the fallback image).  The WikiReader cannot
 * typeset it in two dimensions, but it can show it the way people write
 * maths in plain text:  \sum_{n=1}^{\infty} a_n  ->  ∑ (n=1 below, ∞ above) aₙ
 * with real symbols, sub and superscripts, italic variables and upright
 * function names, and fractions as a/b.
 */
#ifndef TEX_H
#define TEX_H

#include "zw.h"

enum {
	TEX_SUP = 1,
	TEX_SUB = 2,
	TEX_UPRIGHT = 4,	/* not italic */
	TEX_BOLD = 8,
};

/* called with UTF-8 text pieces; a piece never contains two styles */
typedef void (*tex_out_fn)(void *ctx, const char *s, uint32_t n, int style);

void tex_render(const char *src, uint32_t n, tex_out_fn out, void *ctx);

#endif
