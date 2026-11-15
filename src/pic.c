/*
 * zimwiki - pictures (see pic.h)
 */
#include <string.h>
#include "pic.h"

uint32_t pic_src_hash(const char *s, uint32_t n)
{
	/* FNV-1a over the src with the common entities decoded */
	uint32_t h = 2166136261u, i = 0;
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		if (c == '&') {
			static const struct { const char *e; uint8_t len; char c; } ent[] = {
				{ "&amp;", 5, '&' }, { "&#38;", 5, '&' }, { "&quot;", 6, '"' },
				{ "&#39;", 5, '\'' }, { "&apos;", 6, '\'' }, { "&lt;", 4, '<' },
				{ "&gt;", 4, '>' },
			};
			uint32_t k;
			for (k = 0; k < sizeof(ent) / sizeof(ent[0]); ++k)
				if (n - i >= ent[k].len && !memcmp(s + i, ent[k].e, ent[k].len))
					break;
			if (k < sizeof(ent) / sizeof(ent[0])) {
				c = (unsigned char)ent[k].c;
				i += ent[k].len;
			} else {
				i++;
			}
		} else {
			i++;
		}
		h ^= c;
		h *= 16777619u;
	}
	return h;
}

int pic_open(pic_file_t *p, const zw_io_t *io, const uint8_t *uuid)
{
	uint8_t hdr[PIC_HDR];
	int r;
	memset(p, 0, sizeof(*p));
	p->io = *io;
	if ((r = zw_io_read(io, 0, hdr, PIC_HDR)) < 0)
		return r;
	if (zw_rd32(hdr) != PIC_MAGIC || zw_rd32(hdr + 4) != PIC_VERSION)
		return ZW_EFORMAT;
	if (uuid && memcmp(hdr + 28, uuid, 16) != 0)
		return ZW_EFORMAT;	/* made from another ZIM */
	p->articles = zw_rd32(hdr + 8);
	p->mode = zw_rd32(hdr + 44);
	if ((uint64_t)PIC_HDR + (uint64_t)p->articles * 12 > io->size)
		return ZW_EFORMAT;
	return ZW_OK;
}

int pic_article(pic_file_t *p, uint32_t entry, pic_set_t *s)
{
	uint32_t lo = 0, hi = p->articles, i, n;
	uint8_t b[16 * PIC_MAX + 4];
	uint64_t rec = 0;
	int r, found = 0;
	s->n = 0;
	/* binary search the index */
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2, e;
		if ((r = zw_io_read(&p->io, PIC_HDR + (uint64_t)mid * 12, b, 12)) < 0)
			return r;
		e = zw_rd32(b);
		if (e == entry) {
			rec = zw_rd64(b + 4);
			found = 1;
			break;
		}
		if (e < entry)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!found)
		return ZW_OK;
	if ((r = zw_io_read(&p->io, rec, b, 4)) < 0)
		return r;
	n = b[0];
	if (n > PIC_MAX)
		return ZW_EFORMAT;
	if (n && (r = zw_io_read(&p->io, rec + 4, b, n * 16)) < 0)
		return r;
	for (i = 0; i < n; ++i) {
		const uint8_t *q = b + i * 16;
		s->hash[s->n] = zw_rd32(q);
		s->w[s->n] = (uint16_t)(q[4] | q[5] << 8);
		s->h[s->n] = (uint16_t)(q[6] | q[7] << 8);
		s->off[s->n] = zw_rd64(q + 8);
		if (!s->w[s->n] || !s->h[s->n] || s->w[s->n] > PIC_MAX_W || s->h[s->n] > PIC_MAX_H)
			continue;	/* not converted, or nonsense */
		s->n++;
	}
	return ZW_OK;
}

int pic_find(const pic_set_t *s, uint32_t hash)
{
	uint32_t i;
	for (i = 0; i < s->n; ++i)
		if (s->hash[i] == hash)
			return (int)i;
	return -1;
}

int pic_bits(pic_file_t *p, const pic_set_t *s, int k, uint8_t *out)
{
	if (k < 0 || (uint32_t)k >= s->n)
		return ZW_ERANGE;
	return zw_io_read(&p->io, s->off[k], out, pic_bitmap_size(s->w[k], s->h[k]));
}
