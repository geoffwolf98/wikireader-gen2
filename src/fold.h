/*
 * zimwiki - search key folding (case, accents, punctuation)
 * Used identically by zimprep when building the index and by the
 * device when matching what the user types.
 */
#ifndef FOLD_H
#define FOLD_H

#include "zw.h"

/* fold UTF-8 `in` into `out`; returns bytes written (out is NUL
 * terminated, cap includes the NUL).  Runs of spaces collapse to one
 * and leading spaces are dropped. */
size_t fold_utf8(const char *in, size_t inlen, char *out, size_t cap);

/* one character's folded bytes (0 to 8) into out, before the collapsing
 * of spaces: what fold_utf8 does to each character (find in article
 * folds the text as it goes with this) */
int fold_one(uint32_t cp, uint8_t *out);

#endif
