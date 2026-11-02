/*
 * zimwiki - libraries on the card (see library.h)
 */
#include "library.h"
#include "blockdev.h"
#include "fold.h"
#include "plat.h"

#define EXT_MAGIC 0x58455a57u	/* "WZEX" */

int lib_mount(fat_vol_t *v)
{
	int r = bd_init(256);	/* 1 MiB of 4 KiB blocks */
	if (r < 0)
		return r;
	return fat_mount(v);
}

/* ------------------------------------------------------------------ */
/* scanning */

#define SCAN_DIRS 32

typedef struct scan {
	fat_vol_t *v;
	lib_info_t *out;
	int max, n;
	const char *dir;		/* folder being scanned */
	char zims[LIB_MAX][9];
	char zim_dirs[LIB_MAX][9];
	int nzims;
	char dirs[SCAN_DIRS][13];
	int ndirs;
} scan_t;

/* "DIR/NAME.EXT" or "NAME.EXT" */
void lib_path(const lib_info_t *li, const char *ext, char *out)
{
	size_t n = 0, k;
	if (li->dir[0]) {
		k = strlen(li->dir);
		memcpy(out, li->dir, k);
		n = k;
		out[n++] = '/';
	}
	k = strlen(li->name);
	memcpy(out + n, li->name, k);
	n += k;
	k = strlen(ext);
	memcpy(out + n, ext, k + 1);
}

static void set_ids(lib_info_t *li, const char *dir, const char *name)
{
	memset(li->name, 0, sizeof(li->name));
	memset(li->dir, 0, sizeof(li->dir));
	memset(li->key, 0, sizeof(li->key));
	strncpy(li->name, name, 8);
	strncpy(li->dir, dir, 8);
	strncpy(li->key, dir[0] ? dir : name, 8);
}

static int ends_with(const char *s, const char *ext)
{
	size_t a = strlen(s), b = strlen(ext);
	return a > b && strcmp(s + a - b, ext) == 0;
}

static void base_name(const char *s, char *out)
{
	int i = 0;
	while (s[i] && s[i] != '.' && i < 8) {
		out[i] = s[i];
		i++;
	}
	out[i] = 0;
}

static int scan_cb(void *arg, const fat_file_t *f)
{
	scan_t *s = arg;
	if (f->attr & 0x10) {
		/* remember top level folders to look in afterwards */
		if (!s->dir[0] && f->name[0] != '.' && s->ndirs < SCAN_DIRS && !strchr(f->name, '.'))
			memcpy(s->dirs[s->ndirs++], f->name, 13);
		return 0;
	}
	if (ends_with(f->name, ".IDX") && s->n < s->max) {
		uint8_t h[512];
		fat_file_t ff = *f;
		idx_t x;
		lib_info_t *li = &s->out[s->n];
		ff.ext = NULL;
		ff.n_ext = 0;
		if (f->size < IDX_PAGE || fat_build_extents(s->v, &ff, NULL, NULL) < 0)
			return 0;
		if (fat_read(s->v, &ff, 0, h, sizeof(h)) == 0 && idx_parse_header(&x, h) == 0) {
			char nm[9];
			memset(li, 0, sizeof(*li));
			base_name(f->name, nm);
			set_ids(li, s->dir, nm);
			memcpy(li->title, x.title, sizeof(li->title) - 1);
			if (!li->title[0])
				memcpy(li->title, li->name, sizeof(li->name));
			li->articles = x.articles;
			li->prepared = 1;
			s->n++;
		}
		fat_close(&ff);
	} else if (ends_with(f->name, ".ZIM") && s->nzims < LIB_MAX) {
		memset(s->zim_dirs[s->nzims], 0, 9);
		strncpy(s->zim_dirs[s->nzims], s->dir, 8);
		base_name(f->name, s->zims[s->nzims++]);
	}
	return 0;
}

int lib_scan(fat_vol_t *v, lib_info_t *out, int max)
{
	scan_t s;
	int i, k, r;
	memset(&s, 0, sizeof(s));
	s.v = v;
	s.out = out;
	s.max = max;
	s.dir = "";
	if ((r = fat_list(v, "", scan_cb, &s)) < 0)
		return r;
	for (i = 0; i < s.ndirs && s.n < max; ++i) {
		s.dir = s.dirs[i];
		fat_list(v, s.dirs[i], scan_cb, &s);	/* unreadable folders are skipped */
	}
	/* plain ZIM files that have no index of the same name and folder */
	for (i = 0; i < s.nzims && s.n < max; ++i) {
		int dup = 0;
		lib_info_t li;
		set_ids(&li, s.zim_dirs[i], s.zims[i]);
		for (k = 0; k < s.n; ++k)
			if (strcmp(out[k].key, li.key) == 0)
				dup = 1;
		if (dup)
			continue;
		memset(&out[s.n], 0, sizeof(out[s.n]));
		set_ids(&out[s.n], s.zim_dirs[i], s.zims[i]);
		memcpy(out[s.n].title, out[s.n].key, 9);
		out[s.n].prepared = 0;
		s.n++;
	}
	/* the same key twice (WIKI.IDX at the top and in folder WIKI) would
	 * share state files: keep the first */
	for (i = 0; i < s.n; ++i)
		for (k = i + 1; k < s.n; ++k)
			if (strcmp(out[i].key, out[k].key) == 0) {
				memmove(&out[k], &out[k + 1], sizeof(out[0]) * (size_t)(s.n - k - 1));
				s.n--;
				k--;
			}
	return s.n;
}

/* ------------------------------------------------------------------ */
/* ZIM access across chunk files */

static int zim_read_cb(void *ctx, uint64_t off, void *buf, uint32_t len)
{
	library_t *L = ctx;
	uint8_t *p = buf;
	while (len) {
		uint32_t c = (uint32_t)(off >> L->shift);
		uint64_t base = (uint64_t)c << L->shift;
		uint32_t in = (uint32_t)(off - base), n;
		int r;
		if (c >= L->nchunks)
			return ZW_ERANGE;
		if (!L->chunk_ready[c]) {
			if ((r = fat_build_extents(L->vol, &L->chunk[c], NULL, NULL)) < 0)
				return r;
			L->chunk_ready[c] = 1;
			L->ext_dirty = 1;
		}
		n = L->chunk[c].size - in;
		if (n > len)
			n = len;
		if (!n)
			return ZW_EIO;
		if ((r = fat_read(L->vol, &L->chunk[c], in, p, n)) < 0)
			return r;
		p += n;
		off += n;
		len -= n;
	}
	return ZW_OK;
}

static int idx_read_cb(void *ctx, uint64_t off, void *buf, uint32_t len)
{
	library_t *L = ctx;
	if (off > 0xffffffffu)
		return ZW_ERANGE;
	return fat_read(L->vol, &L->idx_file, (uint32_t)off, buf, len);
}

static void ext_name(const library_t *L, char *out)
{
	lib_path(&L->info, ".EXT", out);	/* next to the wiki's own files */
}

static void load_extents(library_t *L)
{
	char name[32];
	uint8_t *data;
	uint32_t len, off = 8, i, n;
	ext_name(L, name);
	if (plat_load(name, &data, &len) < 0)
		return;
	if (len < 8 || zw_rd32(data) != EXT_MAGIC || (n = zw_rd32(data + 4)) != L->nchunks)
		goto done;
	for (i = 0; i < n; ++i) {
		uint32_t size, first, stamp, bytes;
		if (off + 16 > len)
			break;
		size = zw_rd32(data + off);
		first = zw_rd32(data + off + 4);
		stamp = zw_rd32(data + off + 8);
		bytes = zw_rd32(data + off + 12);
		off += 16;
		if (off + bytes > len)
			break;
		if (size == L->chunk[i].size && first == L->chunk[i].first_cluster &&
		    stamp == L->chunk[i].stamp &&
		    fat_extents_load(L->vol, &L->chunk[i], data + off, bytes) == 0)
			L->chunk_ready[i] = 1;
		off += bytes;
	}
done:
	zw_free(data);
}

void lib_save_extents(library_t *L)
{
	uint32_t i, len = 8, off = 8;
	uint8_t *data;
	char name[32];
	if (!L->ext_dirty)
		return;
	for (i = 0; i < L->nchunks; ++i)
		len += 16 + (L->chunk_ready[i] ? fat_extents_size(&L->chunk[i]) : 0);
	if (!(data = zw_alloc(len)))
		return;
	zw_wr32(data, EXT_MAGIC);
	zw_wr32(data + 4, L->nchunks);
	for (i = 0; i < L->nchunks; ++i) {
		uint32_t b = L->chunk_ready[i] ? fat_extents_size(&L->chunk[i]) : 0;
		zw_wr32(data + off, L->chunk[i].size);
		zw_wr32(data + off + 4, L->chunk[i].first_cluster);
		zw_wr32(data + off + 8, L->chunk[i].stamp);
		zw_wr32(data + off + 12, b);
		off += 16;
		if (b)
			fat_extents_save(&L->chunk[i], data + off);
		off += b;
	}
	ext_name(L, name);
	if (plat_save(name, data, len) != 0)
		zw_log("file map not saved");
	L->ext_dirty = 0;	/* no retry loop, see app.c save_history */
	zw_free(data);
	bd_invalidate();	/* the write changed FAT and directory sectors */
}

int lib_open(library_t *L, fat_vol_t *v, const lib_info_t *info)
{
	char name[32];
	uint64_t total = 0;
	uint32_t i;
	int r;

	memset(L, 0, sizeof(*L));
	L->info = *info;
	L->vol = v;

	if (info->prepared) {
		lib_path(info, ".IDX", name);
		if ((r = fat_lookup(v, name, &L->idx_file)) < 0 ||
		    (r = fat_build_extents(v, &L->idx_file, NULL, NULL)) < 0)
			return r;
		L->idx_io.read = idx_read_cb;
		L->idx_io.ctx = L;
		L->idx_io.size = L->idx_file.size;
		if ((r = idx_open(&L->idx, &L->idx_io)) < 0)
			return r;
		L->nchunks = L->idx.chunks;
		L->shift = L->idx.chunk_shift;
		if (L->nchunks > LIB_MAX_CHUNK)
			return ZW_EUNSUPPORTED;
		for (i = 0; i < L->nchunks; ++i) {
			char ext[5] = ".Z00";
			ext[2] = (char)('0' + i / 10);
			ext[3] = (char)('0' + i % 10);
			lib_path(info, ext, name);
			if ((r = fat_lookup(v, name, &L->chunk[i])) < 0) {
				zw_log("missing %s", name);
				return ZW_ENOTFOUND;
			}
			if (i + 1 < L->nchunks && L->chunk[i].size != (1u << L->shift))
				return ZW_EFORMAT;
			total += L->chunk[i].size;
		}
		if (total != L->idx.zim_size)
			return ZW_EFORMAT;
		L->total = L->idx.records;
	} else {
		lib_path(info, ".ZIM", name);
		if ((r = fat_lookup(v, name, &L->chunk[0])) < 0)
			return r;
		L->nchunks = 1;
		L->shift = 32;
		total = L->chunk[0].size;
	}
	load_extents(L);
	L->zim_io.read = zim_read_cb;
	L->zim_io.ctx = L;
	L->zim_io.size = total;
	if ((r = zim_open(&L->zim, &L->zim_io)) < 0)
		return r;
	if (info->prepared && memcmp(L->zim.uuid, L->idx.uuid, 16) != 0)
		return ZW_EFORMAT;	/* index made from a different ZIM */
	if (!info->prepared) {
		char t[128];
		if (zim_metadata(&L->zim, "Title", t, sizeof(t)) == 0 && t[0])
			memcpy(L->info.title, t, sizeof(t));
		L->info.articles = L->zim.entry_count;
		L->total = L->zim.entry_count;
	}
	return ZW_OK;
}

void lib_close(library_t *L)
{
	uint32_t i;
	lib_save_extents(L);
	zim_close(&L->zim);
	if (L->info.prepared)
		idx_close(&L->idx);
	fat_close(&L->idx_file);
	for (i = 0; i < L->nchunks; ++i)
		fat_close(&L->chunk[i]);
	memset(L, 0, sizeof(*L));
}

/* ------------------------------------------------------------------ */
/* searching a plain ZIM through its title pointer list */

static int title_entry(library_t *L, uint32_t pos, uint32_t *entry)
{
	uint8_t b[4];
	int r = zw_io_read(&L->zim_io, L->zim.title_ptr_pos + (uint64_t)pos * 4, b, 4);
	if (r < 0)
		return r;
	*entry = zw_rd32(b);
	return ZW_OK;
}

/* compare entry at title position with (ns, title prefix) */
static int title_cmp(library_t *L, uint32_t pos, char ns, const char *q, int *c)
{
	zim_dirent_t d;
	uint32_t e;
	int r;
	if ((r = title_entry(L, pos, &e)) < 0 || (r = zim_dirent(&L->zim, e, &d)) < 0)
		return r;
	if (d.ns != ns)
		*c = (unsigned char)d.ns < (unsigned char)ns ? -1 : 1;
	else
		*c = strcmp(d.title, q);
	return ZW_OK;
}

static int plain_seek(library_t *L, const char *q, uint32_t *pos)
{
	uint32_t lo = 0, hi = L->zim.entry_count;
	char key[256];
	size_t n = strlen(q);
	int r, c;

	if (n >= sizeof(key))
		n = sizeof(key) - 1;
	memcpy(key, q, n);
	key[n] = 0;
	/* Wikipedia titles start with a capital letter */
	if (key[0] >= 'a' && key[0] <= 'z')
		key[0] = (char)(key[0] - 32);
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if ((r = title_cmp(L, mid, L->zim.content_ns, key, &c)) < 0)
			return r;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	*pos = lo;
	return ZW_OK;
}

int lib_seek(library_t *L, const char *query, uint32_t *pos)
{
	if (L->info.prepared) {
		char key[256];
		idx_cursor_t c;
		int r;
		fold_utf8(query, strlen(query), key, sizeof(key));
		if ((r = idx_seek(&L->idx, key, &c)) < 0)
			return r;
		*pos = c.number;
		return ZW_OK;
	}
	return plain_seek(L, query, pos);
}

int lib_results(library_t *L, uint32_t pos, lib_hit_t *out, int max)
{
	int n = 0, r;
	if (L->info.prepared) {
		idx_cursor_t c;
		idx_record_t rec;
		if (pos >= L->idx.records)
			return 0;
		if ((r = idx_seek_number(&L->idx, pos, &c)) < 0)
			return r;
		while (n < max && (r = idx_next(&L->idx, &c, &rec)) == 1) {
			memcpy(out[n].title, rec.title, sizeof(out[n].title));
			out[n].entry = rec.target;
			out[n].pos = rec.number;
			out[n].redirect = rec.flags & IDX_FLAG_REDIRECT;
			n++;
		}
		return r < 0 ? r : n;
	}
	/* plain ZIM: walk the title list, keeping content articles */
	while (n < max && pos < L->zim.entry_count) {
		zim_dirent_t d;
		uint32_t e, fe;
		if ((r = title_entry(L, pos, &e)) < 0 || (r = zim_dirent(&L->zim, e, &d)) < 0)
			return r;
		if (d.ns != L->zim.content_ns)
			break;
		memcpy(out[n].title, d.title, sizeof(out[n].title));
		out[n].pos = pos;
		out[n].redirect = d.mime == ZIM_MIME_REDIRECT;
		pos++;
		if (zim_follow(&L->zim, e, &fe, &d) < 0 || d.mime != L->zim.html_mime)
			continue;
		out[n].entry = fe;
		n++;
	}
	return n;
}

int lib_random(library_t *L, uint32_t seed, uint32_t *entry)
{
	uint32_t x = seed ? seed : 0x2545f491u;
	int tries;
	for (tries = 0; tries < 40; ++tries) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		if (L->info.prepared) {
			lib_hit_t h;
			if (!L->idx.records)
				return ZW_ENOTFOUND;
			if (lib_results(L, x % L->idx.records, &h, 1) == 1 &&
			    (!h.redirect || tries > 30)) {
				*entry = h.entry;
				return ZW_OK;
			}
		} else {
			zim_dirent_t d;
			uint32_t e = x % L->zim.entry_count;
			if (zim_dirent(&L->zim, e, &d) == 0 && d.ns == L->zim.content_ns &&
			    d.mime == L->zim.html_mime) {
				*entry = e;
				return ZW_OK;
			}
		}
	}
	return ZW_ENOTFOUND;
}
