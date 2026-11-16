/*
 * zimprep - slimming an article's HTML for the WikiReader
 *
 * Keeps only what the WikiReader's converter (src/html.c) reads: its
 * known tags, the attributes it looks at (class words it checks, href,
 * src...), the text; drops comments, scripts, styles, MathML (the TeX is
 * in alttext), unknown tags, and the content of boxes it never shows
 * (navboxes, hidden elements...).  The article converts to the same
 * document either way (tests/slimcheck checks this), with a third or
 * less of the HTML to read.
 */
#ifndef SLIM_H
#define SLIM_H
#include <stddef.h>

/* out: at least 2 * len + 256 bytes (unquoted values gain quotes).  Returns the slimmed length */
size_t html_slim(const char *in, size_t len, char *out);

#endif
