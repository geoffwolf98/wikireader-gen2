/*
 * zimwiki - pictures: the NAME.P00, NAME.P01, ... files made by
 * `zimprep --pictures` and read by the WikiReader.
 *
 * The pictures are converted once on the PC to 1-bit bitmaps (1 = black,
 * rows padded to whole bytes, most significant bit first) that fit the
 * text column.  An article finds its pictures by the src attribute of
 * its <img> tags: zimprep stores a hash of each src, and the WikiReader's
 * HTML parser hashes every <img> it meets the same way (pic_src_hash).
 *
 * File layout, little-endian, split into pieces of 2^shift bytes like
 * the ZIM (NAME.P00 ...):
 *   header (64 bytes):
 *     0  "ZWPI"       4  version (1 or 2) 8  articles N
 *     12 chunk shift  16 pieces           20 total size (u64)
 *     28 ZIM uuid (16 bytes)              44 mode (1 lead, 2 all)
 *     48 pictures (bitmaps stored)
 *     52 (version 2) entries in the prepared ZIM: the pictures belong to
 *        that build's numbering, so a different build is refused
 *     56..63 reserved
 *   index (from 64): N x 12 bytes, sorted by entry:
 *     u32 article entry (the prepared ZIM's numbering), u64 record offset
 *   record: u8 count, 3 reserved, then count x 16 bytes:
 *     u32 src hash, u16 width, u16 height, u64 bitmap offset
 *     (width 0: a picture zimprep could not convert).  Version 2: the
 *     offset's low 40 bits; the top 24 bits, when not 0, are the size of
 *     the bitmap compressed as one zstd frame (else it is stored as is)
 *   bitmaps: shared by every article that shows the same picture
 */
#ifndef ZW_PIC_H
#define ZW_PIC_H

#include "zw.h"

#define PIC_MAGIC     0x4950575au	/* "ZWPI" */
#define PIC_VERSION   2		/* reads 1 too */
#define PIC_OFF_MASK  ((((uint64_t)1) << 40) - 1)
#define PIC_HDR       64
#define PIC_MAX       32		/* pictures per article */
#define PIC_MAX_W     228		/* fits the text column */
#define PIC_MAX_H     200		/* under a screen */
#define PIC_MODE_LEAD 1
#define PIC_MODE_ALL  2

/* hash of an <img src="..."> value as written in the HTML (only &amp;
 * and the like decoded): the same in zimprep and on the WikiReader */
uint32_t pic_src_hash(const char *src, uint32_t len);

static inline uint32_t pic_bitmap_size(uint32_t w, uint32_t h)
{
	return ((w + 7) >> 3) * h;
}

typedef struct pic_file {
	zw_io_t io;
	uint32_t articles;
	uint32_t mode;
	uint32_t version;
} pic_file_t;

typedef struct pic_set {
	uint32_t n;
	uint32_t hash[PIC_MAX];
	uint16_t w[PIC_MAX], h[PIC_MAX];
	uint64_t off[PIC_MAX];
} pic_set_t;

/* check the header; uuid is the ZIM's (pictures made from another ZIM
 * are refused) */
/* uuid and entries: the prepared ZIM's (version 2 files are refused for
 * another build of the same ZIM) */
int pic_open(pic_file_t *p, const zw_io_t *io, const uint8_t *uuid, uint32_t entries);
/* the pictures of one article (none: n = 0) */
int pic_article(pic_file_t *p, uint32_t entry, pic_set_t *s);
/* index of the picture with this src hash, or -1 */
int pic_find(const pic_set_t *s, uint32_t hash);
/* read picture k (pic_bitmap_size bytes) */
int pic_bits(pic_file_t *p, const pic_set_t *s, int k, uint8_t *out);

#endif
