#ifndef RECLUSTER_H
#define RECLUSTER_H
#include "../src/zim.h"
/* write z with ~target_kb KiB clusters to base.Z00, base.Z01, ... */
int recluster(zim_t *z, const char *base, uint32_t shift, uint32_t target_kb, int level,
	      int threads, int keep_fulltext, uint64_t *out_size, uint32_t *out_chunks);
#endif
