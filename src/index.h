/*
 * zimwiki - title search index (NAME.IDX, written by zimprep)
 *
 * Layout, all little-endian:
 *
 *   page 0          header (see IDX_H_* offsets)
 *   pages 1..N      records sorted by folded title:
 *                     u8 title_len (0 = end of page), u8 flags,
 *                     u32 target entry, title bytes
 *   top table       N entries of 20 bytes: u32 first record number,
 *                     u8 key_len, 15 bytes folded key prefix
 *   summary         every IDX_STRIDE-th top entry (loaded into RAM)
 *
 * A lookup binary-searches the summary in RAM, reads one slice of the
 * top table, then scans one or two 4 KiB pages.
 */
#ifndef INDEX_H
#define INDEX_H

#include "zw.h"

#define IDX_MAGIC      0x58495a57u	/* "WZIX" */
#define IDX_VERSION    1
#define IDX_PAGE       4096
#define IDX_KEY        15
#define IDX_TOP        20
#define IDX_STRIDE     128
#define IDX_REC_HDR    6
#define IDX_FLAG_REDIRECT 1

/* header field offsets */
enum {
	IDX_H_MAGIC = 0, IDX_H_VERSION = 4, IDX_H_PAGES = 8, IDX_H_RECORDS = 12,
	IDX_H_ARTICLES = 16, IDX_H_TOP_OFF = 20, IDX_H_SUM_OFF = 24,
	IDX_H_SUM_COUNT = 28, IDX_H_UUID = 32, IDX_H_ZIM_ENTRIES = 48,
	IDX_H_CHUNKS = 52, IDX_H_CHUNK_SHIFT = 56, IDX_H_ZIM_SIZE = 64,
	IDX_H_MAIN = 72, IDX_H_TITLE = 80, IDX_H_LANG = 208, IDX_H_NAME = 224,
	IDX_H_TITLE_LEN = 128, IDX_H_LANG_LEN = 16, IDX_H_NAME_LEN = 64,
};

typedef struct idx_record {
	char title[256];
	uint8_t flags;
	uint32_t target;
	uint32_t number;	/* record number (0 based) */
} idx_record_t;

typedef struct idx_cursor {
	uint32_t page;		/* 1 based record page */
	uint16_t off;		/* byte offset in page */
	uint32_t number;	/* record number at this position */
} idx_cursor_t;

typedef struct idx {
	zw_io_t io;
	uint32_t pages, records, articles;
	uint32_t top_off, sum_off, sum_count;
	uint8_t uuid[16];
	uint32_t zim_entries, chunks, chunk_shift;
	uint64_t zim_size;
	uint32_t main_entry;
	char title[IDX_H_TITLE_LEN];
	char lang[IDX_H_LANG_LEN];
	char name[IDX_H_NAME_LEN];
	uint8_t *summary;	/* sum_count * IDX_TOP bytes */
	uint8_t page[IDX_PAGE];
	uint32_t page_no;	/* page currently in page[], 0 = none */
} idx_t;

/* parse just the header page (used to find the ZIM chunks) */
int idx_parse_header(idx_t *x, const uint8_t *hdr);
int idx_open(idx_t *x, const zw_io_t *io);
void idx_close(idx_t *x);

/* position at the first record whose folded title is >= folded_query */
int idx_seek(idx_t *x, const char *folded_query, idx_cursor_t *c);

/* position at record number n (0 based) */
int idx_seek_number(idx_t *x, uint32_t n, idx_cursor_t *c);

/* read the record at the cursor and advance; returns 0 at the end */
int idx_next(idx_t *x, idx_cursor_t *c, idx_record_t *r);

/* step the cursor back one record; returns 0 at the start */
int idx_prev(idx_t *x, idx_cursor_t *c);

/* compare two folded keys bytewise (like strcmp on unsigned bytes) */
int idx_keycmp(const char *a, size_t alen, const char *b, size_t blen);

#endif
