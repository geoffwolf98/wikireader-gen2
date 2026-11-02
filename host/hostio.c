/*
 * zimwiki - host-side support: memory, logging and a zw_io over one or
 * more ordinary files (split ZIMs: name.zimaa, name.zimab, ...).
 */
#define _FILE_OFFSET_BITS 64
#include "hostio.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

zw_stats_t zw_stats;
int zw_log_enabled = 1;

void *zw_alloc(size_t size) { return malloc(size ? size : 1); }
void *zw_calloc(size_t size) { return calloc(1, size ? size : 1); }
void zw_free(void *p) { free(p); }
void *zw_realloc(void *p, size_t old_size, size_t new_size)
{
	(void)old_size;
	return realloc(p, new_size);
}

void zw_log(const char *fmt, ...)
{
	va_list ap;
	if (!zw_log_enabled)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static int files_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
	host_files_t *f = ctx;
	uint8_t *p = buf;
	int i;

	for (i = 0; i < f->n && len; ++i) {
		uint64_t start = f->start[i], size = f->size[i];
		if (off >= start + size)
			continue;
		{
			uint64_t rel = off - start;
			uint32_t n = (uint32_t)(size - rel < len ? size - rel : len);
			if (fseeko(f->fp[i], (off_t)rel, SEEK_SET) != 0 ||
			    fread(p, 1, n, f->fp[i]) != n)
				return ZW_EIO;
			p += n;
			off += n;
			len -= n;
		}
	}
	return len ? ZW_EIO : ZW_OK;
}

int host_files_open(host_files_t *f, const char *path)
{
	struct stat st;
	char name[4096];
	uint64_t total = 0;

	memset(f, 0, sizeof(*f));
	if (stat(path, &st) == 0) {
		f->fp[0] = fopen(path, "rb");
		if (!f->fp[0])
			return ZW_EIO;
		f->size[0] = (uint64_t)st.st_size;
		f->n = 1;
		total = f->size[0];
	} else {
		/* split set: path is the base name, parts are path + "aa".. */
		int i;
		for (i = 0; i < HOST_MAX_FILES; ++i) {
			snprintf(name, sizeof(name), "%sa%c", path, 'a' + i);
			if (stat(name, &st) != 0)
				break;
			f->fp[i] = fopen(name, "rb");
			if (!f->fp[i])
				return ZW_EIO;
			f->start[i] = total;
			f->size[i] = (uint64_t)st.st_size;
			total += f->size[i];
			f->n = i + 1;
		}
		if (!f->n)
			return ZW_ENOTFOUND;
	}
	f->io.read = files_read;
	f->io.ctx = f;
	f->io.size = total;
	return ZW_OK;
}

void host_files_close(host_files_t *f)
{
	int i;
	for (i = 0; i < f->n; ++i)
		if (f->fp[i])
			fclose(f->fp[i]);
	memset(f, 0, sizeof(*f));
}

/* see zw.h: nothing to feed on the PC */
void (*zw_progress)(int stage, uint32_t done, uint32_t total);
