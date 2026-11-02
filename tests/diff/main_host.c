#include <stdio.h>
#include <stdlib.h>
#include "../../src/zw.h"

zw_stats_t zw_stats;
void *zw_alloc(size_t n) { return malloc(n ? n : 1); }
void *zw_calloc(size_t n) { return calloc(1, n ? n : 1); }
void zw_free(void *p) { free(p); }
void *zw_realloc(void *p, size_t o, size_t n) { (void)o; return realloc(p, n); }
void zw_log(const char *fmt, ...) { (void)fmt; }
void out(const char *s) { fputs(s, stdout); }

int load_file(const char *name, uint8_t **data, uint32_t *len)
{
	char path[256];
	FILE *f;
	long n;
	snprintf(path, sizeof(path), "tests/diff/%s", name);
	f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	*data = malloc((size_t)n);
	*len = (uint32_t)fread(*data, 1, (size_t)n, f);
	fclose(f);
	return 0;
}

void difftest(void);
void zstdstage(void);
void shift64(void);
int main(void)
{
	shift64();
	zstdstage();
	difftest();
	return 0;
}
