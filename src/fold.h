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

#endif
