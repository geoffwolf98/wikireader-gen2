#ifndef RECLUSTER_H
#define RECLUSTER_H
#include "../src/zim.h"
/* write z with ~target_kb KiB clusters to base.Z00, base.Z01, ...
 * Unless keep_images, pictures (image/... entries) and redirects to them
 * are left out altogether and the remaining entries renumbered: then
 * *out_map (malloc'd) gives each old entry number its new one (ZIM_NONE
 * if left out) and *out_entries the new count; *out_map is NULL when
 * nothing was renumbered */
/* the renumbering recluster uses when pictures are left out: old entry
 * -> new (ZIM_NONE: left out), malloc'd; NULL when nothing is left out.
 * *out_entries: the new entry count */
uint32_t *image_map(zim_t *z, uint32_t *out_entries, uint32_t *out_pics, uint32_t *out_redirs);
int recluster(zim_t *z, const char *base, uint32_t shift, uint32_t target_kb, int level,
	      int threads, int keep_fulltext, int keep_images, uint32_t **out_map,
	      uint32_t *out_entries, uint64_t *out_size, uint32_t *out_chunks);
#endif
