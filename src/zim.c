/*
 * zimwiki - minimal ZIM file reader (see zim.h)
 */
#include "zim.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "xz.h"


#define ZIM_CLUSTER_MAX (24u * 1024u * 1024u)	/* trust frame sizes up to this */
#define IN_CHUNK       (16 * 1024)	/* compressed bytes fed per decode step */
#ifndef OUT_INITIAL
#define OUT_INITIAL    (2304u * 1024u)	/* libzim clusters are ~2 MiB */
#endif
#define DIRENT_READ    600
#define XZ_DICT_MAX    (64u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* zstd memory hooks */

static void *zstd_alloc(void *opaque, size_t size)
{
	(void)opaque;
	return zw_alloc(size);
}

static void zstd_free(void *opaque, void *p)
{
	(void)opaque;
	zw_free(p);
}

static const ZSTD_customMem zstd_mem = { zstd_alloc, zstd_free, NULL };

/* ------------------------------------------------------------------ */

static int rd(zim_t *z, uint64_t off, void *buf, uint32_t len)
{
	return zw_io_read(&z->io, off, buf, len);
}

int zim_open(zim_t *z, const zw_io_t *io)
{
	uint8_t h[80];
	uint32_t i;
	int r;

	memset(z, 0, sizeof(*z));
	z->io = *io;
	for (i = 0; i < ZIM_CACHE_SLOTS; ++i)
		z->slot[i].cluster = ZIM_NONE;

	if (z->io.size < 80)
		return ZW_EFORMAT;
	if ((r = rd(z, 0, h, 80)) < 0)
		return r;
	if (zw_rd32(h) != 72173914)
		return ZW_EFORMAT;
	z->major = zw_rd16(h + 4);
	z->minor = zw_rd16(h + 6);
	if (z->major != 5 && z->major != 6)
		return ZW_EUNSUPPORTED;
	memcpy(z->uuid, h + 8, 16);
	z->entry_count = zw_rd32(h + 24);
	z->cluster_count = zw_rd32(h + 28);
	z->url_ptr_pos = zw_rd64(h + 32);
	z->title_ptr_pos = zw_rd64(h + 40);
	z->cluster_ptr_pos = zw_rd64(h + 48);
	z->mime_list_pos = zw_rd64(h + 56);
	z->main_page = zw_rd32(h + 64);
	z->checksum_pos = zw_rd64(h + 72);
	if (z->checksum_pos == 0 || z->checksum_pos > z->io.size)
		z->checksum_pos = z->io.size;

	if (z->url_ptr_pos > z->io.size ||
	    (uint64_t)z->entry_count * 8 > z->io.size - z->url_ptr_pos ||
	    z->cluster_ptr_pos > z->io.size ||
	    (uint64_t)z->cluster_count * 8 > z->io.size - z->cluster_ptr_pos ||
	    z->mime_list_pos >= z->io.size)
		return ZW_EFORMAT;

	/* MIME type list: NUL terminated strings, ended by an empty one */
	{
		uint64_t end = z->url_ptr_pos > z->mime_list_pos ?
			       z->url_ptr_pos : z->io.size;
		uint32_t len = (uint32_t)ZW_MIN(end - z->mime_list_pos, (uint64_t)4096);
		char *p;

		z->mime_buf = zw_alloc(len + 1);
		if (!z->mime_buf)
			return ZW_ENOMEM;
		if ((r = rd(z, z->mime_list_pos, z->mime_buf, len)) < 0)
			return r;
		z->mime_buf[len] = 0;
		z->html_mime = 0xffff;
		p = z->mime_buf;
		while (*p && z->nmime < ZIM_MAX_MIME) {
			if (strncmp(p, "text/html", 9) == 0 && z->html_mime == 0xffff)
				z->html_mime = z->nmime;
			z->mime[z->nmime++] = p;
			p += strlen(p) + 1;
			if (p >= z->mime_buf + len)
				break;
		}
	}

	/* namespace scheme: v6.1+ uses C for content */
	z->content_ns = (z->major >= 6 && z->minor >= 1) ? 'C' : 'A';
	return ZW_OK;
}

static void slot_release_decoder(zim_slot_t *s)
{
	if (s->zstd) {
		ZSTD_freeDStream((ZSTD_DStream *)s->zstd);
		s->zstd = NULL;
	}
	if (s->xz) {
		xz_dec_end((struct xz_dec *)s->xz);
		s->xz = NULL;
	}
}

static void slot_free(zim_slot_t *s)
{
	slot_release_decoder(s);
	zw_free(s->in_buf);
	zw_free(s->out);
	memset(s, 0, sizeof(*s));
	s->cluster = ZIM_NONE;
}

void zim_flush_cache(zim_t *z)
{
	int i;
	for (i = 0; i < ZIM_CACHE_SLOTS; ++i)
		slot_free(&z->slot[i]);
}

void zim_close(zim_t *z)
{
	zim_flush_cache(z);
	zw_free(z->mime_buf);
	zw_free(z->raw);
	memset(z, 0, sizeof(*z));
}

/* ------------------------------------------------------------------ */
/* directory entries */

static int url_ptr(zim_t *z, uint32_t idx, uint64_t *off)
{
	uint8_t b[8];
	int r;
	if (idx >= z->entry_count)
		return ZW_ERANGE;
	if ((r = rd(z, z->url_ptr_pos + (uint64_t)idx * 8, b, 8)) < 0)
		return r;
	*off = zw_rd64(b);
	return ZW_OK;
}

static void copy_str(char *dst, size_t cap, const char *src, size_t len)
{
	if (len >= cap)
		len = cap - 1;
	memcpy(dst, src, len);
	dst[len] = 0;
}

int zim_dirent(zim_t *z, uint32_t idx, zim_dirent_t *d)
{
	uint8_t buf[DIRENT_READ + 1];
	uint64_t off;
	uint32_t len, hdr;
	const char *url, *title, *end;
	int r;

	if ((r = url_ptr(z, idx, &off)) < 0)
		return r;
	if (off >= z->io.size)
		return ZW_EFORMAT;
	len = (uint32_t)ZW_MIN(z->io.size - off, (uint64_t)DIRENT_READ);
	if (len < 12)
		return ZW_EFORMAT;
	if ((r = rd(z, off, buf, len)) < 0)
		return r;
	buf[len] = 0;

	d->mime = zw_rd16(buf);
	d->ns = (char)buf[3];
	d->cluster = d->blob = d->redirect = ZIM_NONE;
	if (d->mime == ZIM_MIME_REDIRECT) {
		d->redirect = zw_rd32(buf + 8);
		hdr = 12;
	} else if (d->mime == ZIM_MIME_LINKTARGET || d->mime == ZIM_MIME_DELETED) {
		hdr = 8;
	} else {
		if (len < 16)
			return ZW_EFORMAT;
		d->cluster = zw_rd32(buf + 8);
		d->blob = zw_rd32(buf + 12);
		hdr = 16;
	}
	end = (const char *)buf + len;
	url = (const char *)buf + hdr;
	title = url + strlen(url) + 1;	/* buf is NUL terminated */
	if (title > end)
		title = end;
	copy_str(d->url, sizeof(d->url), url, strlen(url));
	if (title < end && *title)
		copy_str(d->title, sizeof(d->title), title, strlen(title));
	else
		strcpy(d->title, d->url);
	return ZW_OK;
}

/* compare (ns,url) of entry idx against key; result like strcmp */
static int cmp_entry_url(zim_t *z, uint32_t idx, char ns, const char *url, int *res)
{
	uint8_t buf[DIRENT_READ + 1];
	uint64_t off;
	uint32_t len, hdr;
	uint16_t mime;
	int r;

	if ((r = url_ptr(z, idx, &off)) < 0)
		return r;
	if (off >= z->io.size)
		return ZW_EFORMAT;
	len = (uint32_t)ZW_MIN(z->io.size - off, (uint64_t)DIRENT_READ);
	if ((r = rd(z, off, buf, len)) < 0)
		return r;
	buf[len] = 0;
	mime = zw_rd16(buf);
	hdr = mime == ZIM_MIME_REDIRECT ? 12 :
	      (mime == ZIM_MIME_LINKTARGET || mime == ZIM_MIME_DELETED) ? 8 : 16;
	if ((unsigned char)buf[3] != (unsigned char)ns)
		*res = (unsigned char)buf[3] < (unsigned char)ns ? -1 : 1;
	else
		*res = strcmp((const char *)buf + hdr, url);
	return ZW_OK;
}

int zim_find(zim_t *z, char ns, const char *url, uint32_t *idx)
{
	uint32_t lo = 0, hi = z->entry_count;
	int r, c;

	while (lo < hi) {
		uint32_t mid = lo + ((hi - lo) >> 1);
		if ((r = cmp_entry_url(z, mid, ns, url, &c)) < 0)
			return r;
		if (c == 0) {
			*idx = mid;
			return ZW_OK;
		}
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return ZW_ENOTFOUND;
}

int zim_follow(zim_t *z, uint32_t idx, uint32_t *final_idx, zim_dirent_t *d)
{
	int hops, r;
	for (hops = 0; hops < 8; ++hops) {
		if ((r = zim_dirent(z, idx, d)) < 0)
			return r;
		if (d->mime != ZIM_MIME_REDIRECT) {
			if (final_idx)
				*final_idx = idx;
			return ZW_OK;
		}
		idx = d->redirect;
	}
	return ZW_EFORMAT;
}

/* ------------------------------------------------------------------ */
/* clusters */

static int cluster_range(zim_t *z, uint32_t c, uint64_t *start, uint64_t *end)
{
	uint8_t b[16];
	int r;
	if (c >= z->cluster_count)
		return ZW_ERANGE;
	if (c + 1 < z->cluster_count) {
		if ((r = rd(z, z->cluster_ptr_pos + (uint64_t)c * 8, b, 16)) < 0)
			return r;
		*start = zw_rd64(b);
		*end = zw_rd64(b + 8);
	} else {
		if ((r = rd(z, z->cluster_ptr_pos + (uint64_t)c * 8, b, 8)) < 0)
			return r;
		*start = zw_rd64(b);
		*end = z->checksum_pos;
	}
	if (*start >= *end || *end > z->io.size)
		return ZW_EFORMAT;
	return ZW_OK;
}

static int slot_start(zim_t *z, zim_slot_t *s, uint32_t c, uint8_t info,
		      uint64_t start, uint64_t end)
{
	slot_release_decoder(s);
	s->cluster = c;
	s->comp = info & 0x0f;
	s->ext = (info & 0x10) ? 1 : 0;
	s->done = 0;
	s->in_start = start + 1;
	s->in_pos = start + 1;
	s->in_end = end;
	s->in_len = s->in_off = 0;
	s->out_len = 0;
	if (!s->in_buf && !(s->in_buf = zw_alloc(IN_CHUNK)))
		return ZW_ENOMEM;
	if (!s->out) {
		s->out_cap = OUT_INITIAL;
		if (!(s->out = zw_alloc(s->out_cap)))
			return ZW_ENOMEM;
	}
	if (s->comp == 5) {
		ZSTD_DStream *ds = ZSTD_createDStream_advanced(zstd_mem);
		if (!ds)
			return ZW_ENOMEM;
		/* decode straight into s->out and use it as the window, so no
		 * separate 8 MiB window buffer is needed */
		if (ZSTD_isError(ZSTD_DCtx_setParameter(ds, ZSTD_d_stableOutBuffer, 1))) {
			ZSTD_freeDStream(ds);
			return ZW_EUNSUPPORTED;
		}
		s->zstd = ds;
	} else if (s->comp == 4) {
		static int crc_ready;
		struct xz_dec *x;
		if (!crc_ready) {
			xz_crc32_init();
			crc_ready = 1;
		}
		x = xz_dec_init(XZ_DYNALLOC, XZ_DICT_MAX);
		if (!x)
			return ZW_ENOMEM;
		s->xz = x;
	} else {
		return ZW_EUNSUPPORTED;
	}
	zw_stats.clusters_started++;
	return ZW_OK;
}

/* make sure at least `need` bytes of the cluster are decoded */
static int slot_fill(zim_t *z, zim_slot_t *s, uint32_t need)
{
	int r;

	if (s->out_len >= need)
		return ZW_OK;
	if (s->done)
		return ZW_EFORMAT;	/* asked beyond the end of the cluster */
	if (s->out_len)
		zw_stats.clusters_resumed++;

	while (s->out_len < need) {
		uint32_t before = s->out_len;

		if (s->in_off == s->in_len) {
			uint64_t left = s->in_end - s->in_pos;
			uint32_t n = (uint32_t)ZW_MIN(left, (uint64_t)IN_CHUNK);
			if (n == 0)
				return ZW_EFORMAT;	/* truncated cluster */
			if ((r = rd(z, s->in_pos, s->in_buf, n)) < 0)
				return r;
			s->in_pos += n;
			s->in_len = n;
			s->in_off = 0;
		}

		if (s->comp == 5) {
			ZSTD_inBuffer in;
			ZSTD_outBuffer out;
			size_t ret;

			if (s->out_len == s->out_cap) {
				/* cluster larger than the buffer: grow and start
				 * again (the zstd window lives in the buffer) */
				zw_free(s->out);
				s->out_cap += s->out_cap / 2;
				if (!(s->out = zw_alloc(s->out_cap))) {
					s->out_cap = 0;
					return ZW_ENOMEM;
				}
				ZSTD_DCtx_reset((ZSTD_DStream *)s->zstd, ZSTD_reset_session_only);
				s->in_pos = s->in_start;
				s->in_len = s->in_off = 0;
				s->out_len = 0;
				continue;
			}
			if (s->out_len == 0 && s->in_off == 0 && s->in_pos == s->in_start + s->in_len) {
				/* first step: the frame header usually says how big
				 * the cluster is (zimprep always writes it), so size
				 * the buffer once instead of growing and restarting,
				 * which on the WikiReader means reading it again */
				unsigned long long fcs = ZSTD_getFrameContentSize(s->in_buf, s->in_len);
				if (fcs != ZSTD_CONTENTSIZE_UNKNOWN && fcs != ZSTD_CONTENTSIZE_ERROR &&
				    fcs > s->out_cap && fcs <= ZIM_CLUSTER_MAX) {
					zw_free(s->out);
					s->out_cap = (uint32_t)fcs;
					if (!(s->out = zw_alloc(s->out_cap))) {
						s->out_cap = 0;
						return ZW_ENOMEM;
					}
				}
			}
			in.src = s->in_buf;
			in.size = s->in_len;
			in.pos = s->in_off;
			out.dst = s->out;
			out.size = s->out_cap;
			out.pos = s->out_len;
			ZW_PROGRESS(1, (uint32_t)((s->in_pos - s->in_start) >> 10),
				    (uint32_t)((s->in_end - s->in_start) >> 10));
			ret = ZSTD_decompressStream((ZSTD_DStream *)s->zstd, &out, &in);
			if (ZSTD_isError(ret)) {
				if (ZSTD_getErrorCode(ret) == ZSTD_error_dstSize_tooSmall) {
					s->out_len = s->out_cap;	/* grow next round (normal) */
					continue;
				}
				zw_log("zstd error %d at in %u/%u out %u/%u", (int)ZSTD_getErrorCode(ret),
				       (unsigned)in.pos, (unsigned)in.size, (unsigned)out.pos, (unsigned)out.size);
				return ZW_EFORMAT;
			}
			s->in_off = (uint32_t)in.pos;
			s->out_len = (uint32_t)out.pos;
			if (ret == 0)
				s->done = 1;
		} else {
			struct xz_buf b;
			enum xz_ret ret;

			if (s->out_len == s->out_cap) {
				/* xz copies out of its own dictionary, so the
				 * output buffer may move */
				uint32_t ncap = s->out_cap + s->out_cap / 2;
				uint8_t *n = zw_realloc(s->out, s->out_cap, ncap);
				if (!n)
					return ZW_ENOMEM;
				s->out = n;
				s->out_cap = ncap;
			}
			b.in = s->in_buf;
			b.in_pos = s->in_off;
			b.in_size = s->in_len;
			b.out = s->out;
			b.out_pos = s->out_len;
			b.out_size = s->out_cap;
			ZW_PROGRESS(1, (uint32_t)((s->in_pos - s->in_start) >> 10),
				    (uint32_t)((s->in_end - s->in_start) >> 10));
			ret = xz_dec_run((struct xz_dec *)s->xz, &b);
			s->in_off = (uint32_t)b.in_pos;
			s->out_len = (uint32_t)b.out_pos;
			if (ret == XZ_STREAM_END)
				s->done = 1;
			else if (ret != XZ_OK && ret != XZ_UNSUPPORTED_CHECK) {
				zw_log("xz error %d", (int)ret);
				return ZW_EFORMAT;
			}
		}
		zw_stats.bytes_decompressed += s->out_len - before;

		if (s->done) {
			slot_release_decoder(s);
			if (s->out_len < need)
				return ZW_EFORMAT;
			break;
		}
	}
	return ZW_OK;
}

static zim_slot_t *slot_for(zim_t *z, uint32_t c)
{
	zim_slot_t *best = NULL;
	int i;
	for (i = 0; i < ZIM_CACHE_SLOTS; ++i) {
		zim_slot_t *s = &z->slot[i];
		if (s->cluster == c)
			return s;
	}
	/* evict: prefer an empty slot, else least recently used */
	for (i = 0; i < ZIM_CACHE_SLOTS; ++i) {
		zim_slot_t *s = &z->slot[i];
		if (s->cluster == ZIM_NONE)
			return s;
		if (!best || s->last_use < best->last_use)
			best = s;
	}
	return best;
}

static uint32_t rd_off(const uint8_t *p, int ext)
{
	/* offsets above 4 GiB inside one cluster are not supported */
	return ext ? (zw_rd32(p + 4) ? 0xffffffffu : zw_rd32(p)) : zw_rd32(p);
}

static int blob_uncompressed(zim_t *z, uint64_t start, uint64_t end, int ext,
			     uint32_t blob, const uint8_t **data, uint32_t *len)
{
	uint8_t b[16];
	uint32_t osz = ext ? 8 : 4, first, o1, o2, n;
	uint64_t base = start + 1;
	int r;

	if ((r = rd(z, base, b, osz)) < 0)
		return r;
	first = rd_off(b, ext);
	if (first < osz || first % osz)
		return ZW_EFORMAT;
	n = first / osz - 1;
	if (blob >= n)
		return ZW_ERANGE;
	if ((r = rd(z, base + (uint64_t)blob * osz, b, 2 * osz)) < 0)
		return r;
	o1 = rd_off(b, ext);
	o2 = rd_off(b + osz, ext);
	if (o2 < o1 || base + o2 > end)
		return ZW_EFORMAT;
	n = o2 - o1;
	if (n + 1 > z->raw_cap) {
		zw_free(z->raw);
		z->raw_cap = n + 1;
		if (!(z->raw = zw_alloc(z->raw_cap))) {
			z->raw_cap = 0;
			return ZW_ENOMEM;
		}
	}
	if (n && (r = rd(z, base + o1, z->raw, n)) < 0)
		return r;
	z->raw[n] = 0;
	*data = z->raw;
	*len = n;
	return ZW_OK;
}

int zim_blob(zim_t *z, uint32_t cluster, uint32_t blob,
	     const uint8_t **data, uint32_t *len)
{
	uint64_t start, end;
	uint8_t info;
	uint32_t osz, first, o1, o2, nblobs;
	zim_slot_t *s;
	int r;

	if ((r = cluster_range(z, cluster, &start, &end)) < 0)
		return r;

	s = slot_for(z, cluster);
	if (s->cluster == cluster) {
		zw_stats.cluster_cache_hits++;
	} else {
		if ((r = rd(z, start, &info, 1)) < 0)
			return r;
		if ((info & 0x0f) <= 1)
			return blob_uncompressed(z, start, end, info & 0x10, blob, data, len);
		if ((r = slot_start(z, s, cluster, info, start, end)) < 0) {
			zw_log("slot_start: cluster %u info %u failed (%d)", (unsigned)cluster, info, r);
			slot_free(s);
			return r;
		}
	}
	s->last_use = ++z->clock;

	osz = s->ext ? 8 : 4;
	if ((r = slot_fill(z, s, osz)) < 0)
		goto fail;
	first = rd_off(s->out, s->ext);
	if (first < osz || first % osz) {
		r = ZW_EFORMAT;
		goto fail;
	}
	nblobs = first / osz - 1;
	if (blob >= nblobs)
		return ZW_ERANGE;
	if ((r = slot_fill(z, s, (blob + 2) * osz)) < 0)
		goto fail;
	o1 = rd_off(s->out + blob * osz, s->ext);
	o2 = rd_off(s->out + (blob + 1) * osz, s->ext);
	if (o2 < o1 || o1 < first) {
		r = ZW_EFORMAT;
		goto fail;
	}
	if ((r = slot_fill(z, s, o2)) < 0)
		goto fail;
	*data = s->out + o1;
	*len = o2 - o1;
	return ZW_OK;

fail:
	zw_log("zim_blob: cluster %u blob %u failed (%d)", (unsigned)cluster, (unsigned)blob, r);
	slot_free(s);
	return r;
}

int zim_metadata(zim_t *z, const char *name, char *out, size_t cap)
{
	zim_dirent_t d;
	uint32_t idx, len;
	const uint8_t *p;
	int r;

	out[0] = 0;
	if ((r = zim_find(z, 'M', name, &idx)) < 0)
		return r;
	if ((r = zim_follow(z, idx, NULL, &d)) < 0)
		return r;
	if (d.cluster == ZIM_NONE)
		return ZW_EFORMAT;
	if ((r = zim_blob(z, d.cluster, d.blob, &p, &len)) < 0)
		return r;
	copy_str(out, cap, (const char *)p, len);
	return ZW_OK;
}
