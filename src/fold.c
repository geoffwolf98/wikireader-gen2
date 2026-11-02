/*
 * zimwiki - search key folding (see fold.h)
 */
#include "fold.h"
#include "utf8.h"
#include "fold_table.h"

static const uint8_t *lookup(uint32_t cp)
{
	uint32_t lo = 0, hi = FOLD_COUNT;
	if (cp > 0xffff)
		return NULL;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (fold_cp[mid] < cp)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo < FOLD_COUNT && fold_cp[lo] == cp) ? fold_out[lo] : NULL;
}

size_t fold_utf8(const char *in, size_t inlen, char *out, size_t cap)
{
	const uint8_t *p = (const uint8_t *)in, *end = p + inlen;
	uint8_t *o = (uint8_t *)out, *oend = (uint8_t *)out + cap - 1;
	int last_space = 1;	/* drops leading spaces */

	while (p < end) {
		uint8_t tmp[8];
		const uint8_t *src;
		int n;
		uint32_t cp = utf8_next(&p, end);

		if (cp < 0x80) {
			if (cp >= 'A' && cp <= 'Z')
				cp += 32;
			else if (cp == '-' || cp == '_' || cp == '\t')
				cp = ' ';
			else if (cp == '\'' || cp == '`')
				continue;
			tmp[0] = (uint8_t)cp;
			src = tmp;
			n = 1;
		} else {
			const uint8_t *f = lookup(cp);
			if (f) {
				src = f + 1;
				n = f[0];
			} else {
				n = utf8_put(tmp, cp);
				src = tmp;
			}
		}
		if (o + n > oend)
			break;	/* never split a UTF-8 sequence */
		while (n--) {
			uint8_t c = *src++;
			if (c == ' ') {
				if (last_space)
					continue;
				last_space = 1;
			} else {
				last_space = 0;
			}
			*o++ = c;
		}
	}
	*o = 0;
	return (size_t)(o - (uint8_t *)out);
}
