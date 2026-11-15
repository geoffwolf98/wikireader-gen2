/*
 * zimprep --pictures: articles' pictures as 1-bit bitmaps in
 * OUTDIR/NAME.P00, NAME.P01, ... (format: src/pic.h)
 */
#ifndef ZW_PICTURES_H
#define ZW_PICTURES_H

#include "../src/zim.h"

/* mode: PIC_MODE_LEAD (the first picture of each article) or
 * PIC_MODE_ALL.  map: recluster's renumbering (NULL: none); entries: the
 * prepared ZIM's entry count (recorded, so another build is refused).
 * Returns the number of articles with pictures, or < 0 on error */
long make_pictures(zim_t *z, const char *outdir, const char *name, int mode, int threads,
		   const uint32_t *map, uint32_t entries, uint32_t shift);

/* 0 if this zimprep was built without picture support (no libwebp) */
int pictures_supported(void);

#endif
