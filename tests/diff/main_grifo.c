#include <grifo.h>
#include "../../src/zw.h"

zw_stats_t zw_stats;
void *zw_alloc(size_t n) { return memory_allocate(n ? n : 1, "t"); }
void *zw_calloc(size_t n) { void *p = zw_alloc(n); if (p) memset(p, 0, n); return p; }
void zw_free(void *p) { if (p) memory_free(p, "t"); }
void *zw_realloc(void *p, size_t o, size_t n)
{
	void *q = zw_alloc(n);
	if (q && p) {
		memcpy(q, p, o < n ? o : n);
		zw_free(p);
	}
	return q;
}
void *malloc(size_t n) { return zw_alloc(n); }
void *calloc(size_t a, size_t b) { return zw_calloc(a * b); }
void free(void *p) { zw_free(p); }
void zw_log(const char *fmt, ...) { (void)fmt; }
void out(const char *s) { debug_print(s); }

int load_file(const char *name, uint8_t **data, uint32_t *len)
{
	unsigned long size;
	int h;
	if (file_size(name, &size) != 0)
		return -1;
	h = file_open(name, FILE_OPEN_READ);
	if (h < 0)
		return -1;
	*data = zw_alloc(size);
	*len = (uint32_t)file_read(h, *data, size);
	file_close(h);
	return 0;
}

void difftest(void);
void zstdstage(void);
void shift64(void);
int grifo_main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	shift64();
	zstdstage();
	difftest();
	return 0;
}
