#ifndef HOSTIO_H
#define HOSTIO_H

#include <stdio.h>
#include "../src/zw.h"

#define HOST_MAX_FILES 64

typedef struct host_files {
	zw_io_t io;
	int n;
	FILE *fp[HOST_MAX_FILES];
	uint64_t start[HOST_MAX_FILES];
	uint64_t size[HOST_MAX_FILES];
} host_files_t;

/* open a ZIM file, or a split set when `path` is the name without the
 * trailing "aa"/"ab"... (e.g. "wiki.zim" for wiki.zimaa, wiki.zimab) */
int host_files_open(host_files_t *f, const char *path);
void host_files_close(host_files_t *f);

extern int zw_log_enabled;

#endif
