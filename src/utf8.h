#ifndef UTF8_H
#define UTF8_H

#include "zw.h"

/* decode one code point; invalid bytes decode to U+FFFD and advance 1 */
static inline uint32_t utf8_next(const uint8_t **pp, const uint8_t *end)
{
	const uint8_t *p = *pp;
	uint32_t c = *p++;
	if (c >= 0x80) {
		int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : -1;
		uint32_t cp = n == 3 ? (c & 7) : n == 2 ? (c & 15) : (c & 31);
		int i;
		if (n < 0 || p + n > end) {
			*pp = p;
			return 0xfffd;
		}
		for (i = 0; i < n; ++i) {
			if ((p[i] & 0xc0) != 0x80) {
				*pp = p;
				return 0xfffd;
			}
			cp = (cp << 6) | (p[i] & 0x3f);
		}
		p += n;
		c = cp;
	}
	*pp = p;
	return c;
}

static inline int utf8_put(uint8_t *out, uint32_t c)
{
	if (c < 0x80) {
		out[0] = (uint8_t)c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = (uint8_t)(0xc0 | (c >> 6));
		out[1] = (uint8_t)(0x80 | (c & 0x3f));
		return 2;
	}
	if (c < 0x10000) {
		out[0] = (uint8_t)(0xe0 | (c >> 12));
		out[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
		out[2] = (uint8_t)(0x80 | (c & 0x3f));
		return 3;
	}
	out[0] = (uint8_t)(0xf0 | (c >> 18));
	out[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3f));
	out[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3f));
	out[3] = (uint8_t)(0x80 | (c & 0x3f));
	return 4;
}

/* length of the UTF-8 sequence starting with byte b */
static inline int utf8_len(uint8_t b)
{
	return b < 0x80 ? 1 : b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : b >= 0xc0 ? 2 : 1;
}

#endif
