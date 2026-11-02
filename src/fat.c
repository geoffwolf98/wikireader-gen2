/*
 * zimwiki - read-only FAT16/FAT32 access for large files (see fat.h)
 */
#include "fat.h"
#include "blockdev.h"

#define FAT_EOC32  0x0ffffff8u
#define FAT_EOC16  0xfff8u
#define WINDOW_SECTORS 32	/* FAT sectors read per burst while walking a chain */

static int valid_bpb(const uint8_t *b)
{
	uint8_t spc = b[13];
	return (b[0] == 0xeb || b[0] == 0xe9) && zw_rd16(b + 11) == 512 &&
	       spc && !(spc & (spc - 1)) && b[16] >= 1 && zw_rd16(b + 14) >= 1;
}

int fat_mount(fat_vol_t *v)
{
	uint8_t b[512];
	uint32_t tot, fatsz, rsvd, root_ents, spc, data_sectors;
	int r, i;

	memset(v, 0, sizeof(*v));
	if ((r = bd_read(0, 0, b, 512)) < 0)
		return r;
	if (!valid_bpb(b)) {
		/* MBR: take the first FAT partition */
		if (b[510] != 0x55 || b[511] != 0xaa)
			return ZW_EFORMAT;
		for (i = 0; i < 4; ++i) {
			const uint8_t *p = b + 446 + 16 * i;
			uint8_t t = p[4];
			if (t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0b ||
			    t == 0x0c || t == 0x0e) {
				v->part_lba = zw_rd32(p + 8);
				break;
			}
		}
		if (i == 4)
			return ZW_EFORMAT;
		if ((r = bd_read(v->part_lba, 0, b, 512)) < 0)
			return r;
		if (!valid_bpb(b))
			return ZW_EFORMAT;
	}

	spc = b[13];
	while ((1u << v->spc_shift) < spc)
		v->spc_shift++;
	rsvd = zw_rd16(b + 14);
	root_ents = zw_rd16(b + 17);
	tot = zw_rd16(b + 19) ? zw_rd16(b + 19) : zw_rd32(b + 32);
	fatsz = zw_rd16(b + 22) ? zw_rd16(b + 22) : zw_rd32(b + 36);
	v->root_sectors = (root_ents * 32 + 511) / 512;
	v->fat_lba = v->part_lba + rsvd;
	v->fat_sectors = fatsz;
	v->root_lba = v->fat_lba + b[16] * fatsz;
	v->data_lba = v->root_lba + v->root_sectors;
	data_sectors = tot - (v->data_lba - v->part_lba);
	v->cluster_count = data_sectors >> v->spc_shift;
	/* FAT32 layout is identified by the BPB (no 16 bit FAT size, no
	 * fixed root); formatters do create small FAT32 volumes */
	v->fat32 = zw_rd16(b + 22) == 0 && root_ents == 0;
	if (!v->fat32 && v->cluster_count < 4085)
		return ZW_EUNSUPPORTED;	/* FAT12 */
	if (v->fat32)
		v->root_cluster = zw_rd32(b + 44);
	return ZW_OK;
}

static uint32_t cluster_lba(const fat_vol_t *v, uint32_t c)
{
	return v->data_lba + ((c - 2) << v->spc_shift);
}

static int fat_next(fat_vol_t *v, uint32_t c, uint32_t *next)
{
	uint8_t e[4];
	int r;
	if (v->fat32) {
		if ((r = bd_read(v->fat_lba, c * 4, e, 4)) < 0)
			return r;
		*next = zw_rd32(e) & 0x0fffffffu;
		if (*next >= FAT_EOC32)
			*next = 0;
	} else {
		if ((r = bd_read(v->fat_lba, c * 2, e, 2)) < 0)
			return r;
		*next = zw_rd16(e);
		if (*next >= FAT_EOC16)
			*next = 0;
	}
	if (*next && (*next < 2 || *next >= v->cluster_count + 2))
		return ZW_EFORMAT;
	return ZW_OK;
}

static int is_eoc(uint32_t c)
{
	return c == 0;
}

/* "name.ext" -> 11 byte padded upper case form */
static int to83(const char *s, size_t len, char out[11])
{
	size_t i = 0, k = 0;
	memset(out, ' ', 11);
	while (i < len && s[i] != '.') {
		if (k >= 8)
			return -1;
		out[k++] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
		i++;
	}
	if (i < len) {
		i++;
		k = 8;
		while (i < len) {
			if (k >= 11)
				return -1;
			out[k++] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
			i++;
		}
	}
	return 0;
}

static void from83(const uint8_t *e, char *out)
{
	int i, k = 0;
	for (i = 0; i < 8 && e[i] != ' '; ++i)
		out[k++] = (char)e[i];
	if (e[8] != ' ') {
		out[k++] = '.';
		for (i = 8; i < 11 && e[i] != ' '; ++i)
			out[k++] = (char)e[i];
	}
	out[k] = 0;
	if ((uint8_t)out[0] == 0x05)
		out[0] = (char)0xe5;
}

static void fill_file(const uint8_t *e, fat_file_t *f, int fat32)
{
	memset(f, 0, sizeof(*f));
	from83(e, f->name);
	f->attr = e[11];
	f->size = zw_rd32(e + 28);
	f->first_cluster = zw_rd16(e + 26) | (fat32 ? (uint32_t)zw_rd16(e + 20) << 16 : 0);
	f->stamp = zw_rd32(e + 22);
}

/* iterate the directory starting at cluster `dc` (0 = root) */
static int dir_iter(fat_vol_t *v, uint32_t dc, fat_dir_cb cb, void *arg)
{
	uint8_t sec[512];
	fat_file_t f;
	uint32_t c = dc, s, nsec, lba;
	int r, i, fixed_root = (dc == 0 && !v->fat32), guard = 0;

	if (dc == 0 && v->fat32)
		c = v->root_cluster;
	for (;;) {
		if (fixed_root) {
			lba = v->root_lba;
			nsec = v->root_sectors;
		} else {
			lba = cluster_lba(v, c);
			nsec = 1u << v->spc_shift;
		}
		for (s = 0; s < nsec; ++s) {
			if ((r = bd_read(lba + s, 0, sec, 512)) < 0)
				return r;
			for (i = 0; i < 512; i += 32) {
				const uint8_t *e = sec + i;
				if (e[0] == 0)
					return ZW_OK;
				if (e[0] == 0xe5 || e[11] == 0x0f || (e[11] & 0x08))
					continue;
				fill_file(e, &f, v->fat32);
				if (cb(arg, &f))
					return 1;
			}
		}
		if (fixed_root)
			return ZW_OK;
		if ((r = fat_next(v, c, &c)) < 0)
			return r;
		if (is_eoc(c) || ++guard > 65536)
			return ZW_OK;
	}
}

typedef struct find_ctx {
	char want[11];
	fat_file_t *out;
} find_ctx_t;

static int find_cb(void *arg, const fat_file_t *f)
{
	find_ctx_t *fc = arg;
	char n[11];
	if (to83(f->name, strlen(f->name), n) == 0 && memcmp(n, fc->want, 11) == 0) {
		*fc->out = *f;
		return 1;
	}
	return 0;
}

static int resolve_dir(fat_vol_t *v, const char *path, size_t len, uint32_t *dc)
{
	*dc = 0;
	while (len) {
		size_t k = 0;
		fat_file_t f;
		find_ctx_t fc;
		int r;
		while (k < len && path[k] != '/')
			k++;
		if (k) {
			if (to83(path, k, fc.want) < 0)
				return ZW_ENOTFOUND;
			fc.out = &f;
			r = dir_iter(v, *dc, find_cb, &fc);
			if (r < 0)
				return r;
			if (r == 0 || !(f.attr & 0x10))
				return ZW_ENOTFOUND;
			*dc = f.first_cluster;
		}
		path += k;
		len -= k;
		if (len) {
			path++;
			len--;
		}
	}
	return ZW_OK;
}

int fat_lookup(fat_vol_t *v, const char *path, fat_file_t *f)
{
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	uint32_t dc;
	find_ctx_t fc;
	int r;

	if ((r = resolve_dir(v, path, slash ? (size_t)(slash - path) : 0, &dc)) < 0)
		return r;
	if (to83(base, strlen(base), fc.want) < 0)
		return ZW_ENOTFOUND;
	fc.out = f;
	r = dir_iter(v, dc, find_cb, &fc);
	if (r < 0)
		return r;
	return r ? ZW_OK : ZW_ENOTFOUND;
}

int fat_list(fat_vol_t *v, const char *dir, fat_dir_cb cb, void *arg)
{
	uint32_t dc;
	int r = resolve_dir(v, dir, strlen(dir), &dc);
	if (r < 0)
		return r;
	r = dir_iter(v, dc, cb, arg);
	return r < 0 ? r : ZW_OK;
}

static int add_extent(fat_file_t *f, uint32_t *cap, uint32_t fc, uint32_t dc)
{
	if (f->n_ext) {
		fat_extent_t *e = &f->ext[f->n_ext - 1];
		if (e->disk_cluster + e->count == dc) {
			e->count++;
			return ZW_OK;
		}
	}
	if (f->n_ext == *cap) {
		uint32_t ncap = *cap ? *cap * 2 : 8;
		fat_extent_t *n = zw_realloc(f->ext, sizeof(fat_extent_t) * *cap,
					     sizeof(fat_extent_t) * ncap);
		if (!n)
			return ZW_ENOMEM;
		f->ext = n;
		*cap = ncap;
	}
	f->ext[f->n_ext].file_cluster = fc;
	f->ext[f->n_ext].disk_cluster = dc;
	f->ext[f->n_ext].count = 1;
	f->n_ext++;
	return ZW_OK;
}

int fat_build_extents(fat_vol_t *v, fat_file_t *f, fat_progress_cb cb, void *arg)
{
	uint32_t csize = 512u << v->spc_shift;
	uint32_t need = (f->size + csize - 1) / csize;
	uint32_t c = f->first_cluster, i, cap = 0;
	uint32_t per_sector = v->fat32 ? 128 : 256;
	uint32_t win_first = 0xffffffffu;	/* first FAT sector in window */
	uint8_t *win;
	int r = ZW_OK;

	zw_free(f->ext);
	f->ext = NULL;
	f->n_ext = 0;
	if (!need)
		return ZW_OK;
	if (!(win = zw_alloc(WINDOW_SECTORS * 512)))
		return ZW_ENOMEM;

	for (i = 0; i < need; ++i) {
		uint32_t sector, next;
		if (c < 2 || c >= v->cluster_count + 2) {
			r = ZW_EFORMAT;
			break;
		}
		if ((r = add_extent(f, &cap, i, c)) < 0)
			break;
		if (i + 1 == need)
			break;
		sector = c / per_sector;
		if (win_first == 0xffffffffu || sector < win_first ||
		    sector >= win_first + WINDOW_SECTORS) {
			uint32_t n = WINDOW_SECTORS;
			if (sector + n > v->fat_sectors)
				n = v->fat_sectors - sector;
			ZW_KEEPALIVE();
			if ((r = bd_read(v->fat_lba + sector, 0, win, n * 512)) < 0)
				break;
			win_first = sector;
		}
		{
			uint32_t idx = c - win_first * per_sector;
			next = v->fat32 ? zw_rd32(win + idx * 4) & 0x0fffffffu
					: zw_rd16(win + idx * 2);
		}
		c = next;
		if (cb && (i & 0x3fff) == 0)
			cb(arg, i, need);
	}
	zw_free(win);
	if (r < 0) {
		zw_free(f->ext);
		f->ext = NULL;
		f->n_ext = 0;
	}
	return r;
}

uint32_t fat_extents_size(const fat_file_t *f)
{
	return 4 + 12 * f->n_ext;
}

void fat_extents_save(const fat_file_t *f, uint8_t *out)
{
	uint32_t i;
	zw_wr32(out, f->n_ext);
	for (i = 0; i < f->n_ext; ++i) {
		zw_wr32(out + 4 + 12 * i, f->ext[i].file_cluster);
		zw_wr32(out + 8 + 12 * i, f->ext[i].disk_cluster);
		zw_wr32(out + 12 + 12 * i, f->ext[i].count);
	}
}

int fat_extents_load(fat_vol_t *v, fat_file_t *f, const uint8_t *in, uint32_t len)
{
	uint32_t n, i, expect = 0;
	uint32_t csize = 512u << v->spc_shift;
	if (len < 4)
		return ZW_EFORMAT;
	n = zw_rd32(in);
	if (len < 4 + 12 * n || n > 1000000)
		return ZW_EFORMAT;
	zw_free(f->ext);
	f->ext = zw_alloc(sizeof(fat_extent_t) * (n ? n : 1));
	if (!f->ext)
		return ZW_ENOMEM;
	for (i = 0; i < n; ++i) {
		fat_extent_t *e = &f->ext[i];
		e->file_cluster = zw_rd32(in + 4 + 12 * i);
		e->disk_cluster = zw_rd32(in + 8 + 12 * i);
		e->count = zw_rd32(in + 12 + 12 * i);
		if (e->file_cluster != expect || e->disk_cluster < 2 ||
		    e->disk_cluster + e->count > v->cluster_count + 2)
			goto bad;
		expect += e->count;
	}
	if (expect != (f->size + csize - 1) / csize ||
	    (n && f->ext[0].disk_cluster != f->first_cluster))
		goto bad;
	f->n_ext = n;
	return ZW_OK;
bad:
	zw_free(f->ext);
	f->ext = NULL;
	f->n_ext = 0;
	return ZW_EFORMAT;
}

int fat_read(fat_vol_t *v, const fat_file_t *f, uint32_t off, void *buf, uint32_t len)
{
	uint8_t *out = buf;
	uint32_t cshift = 9 + v->spc_shift;

	if (off > f->size || len > f->size - off)
		return ZW_ERANGE;
	while (len) {
		uint32_t fc = off >> cshift;
		uint32_t lo = 0, hi = f->n_ext, k;
		const fat_extent_t *e;
		uint32_t run_end, n, rel;
		int r;

		/* binary search for the extent containing file cluster fc */
		while (hi - lo > 1) {
			uint32_t mid = (lo + hi) / 2;
			if (f->ext[mid].file_cluster <= fc)
				lo = mid;
			else
				hi = mid;
		}
		if (lo >= f->n_ext)
			return ZW_EFORMAT;
		e = &f->ext[lo];
		k = fc - e->file_cluster;
		if (k >= e->count)
			return ZW_EFORMAT;
		rel = off - (fc << cshift);
		{
			/* 64 bit: a run may end exactly at 4 GiB */
			uint64_t end64 = (uint64_t)(e->file_cluster + e->count) << cshift;
			uint64_t avail = end64 - off;
			n = avail < len ? (uint32_t)avail : len;
		}
		(void)run_end;
		if ((r = bd_read(cluster_lba(v, e->disk_cluster + k), rel, out, n)) < 0)
			return r;
		out += n;
		off += n;
		len -= n;
	}
	return ZW_OK;
}

void fat_close(fat_file_t *f)
{
	zw_free(f->ext);
	f->ext = NULL;
	f->n_ext = 0;
}
