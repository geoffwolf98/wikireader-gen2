/*
 * zimwiki - article document model
 *
 * html_to_doc() turns mwoffliner HTML into blocks of styled text
 * runs; layout.c wraps them into lines for the 240 pixel screen.
 */
#ifndef DOC_H
#define DOC_H

#include "zw.h"

enum block_kind {
	BK_PARA, BK_TITLE, BK_H2, BK_H3, BK_H4, BK_ITEM, BK_DT, BK_DD,
	BK_QUOTE, BK_ROW, BK_CAPTION, BK_RULE, BK_SMALL,
	BK_MROW,		/* 2nd and later rows of a display matrix: no gap above */
};

enum run_flags {
	RF_LINK = 1,
	RF_SUB = 2,
	RF_SUP = 4,
};

typedef struct doc_run {
	uint32_t start, len;	/* bytes in doc.text */
	uint16_t link;		/* index into links, 0xffff = none */
	uint8_t font;
	uint8_t flags;
} doc_run_t;

typedef struct doc_block {
	uint32_t first_run, nruns;
	uint8_t kind;
	uint8_t indent;		/* list / quote nesting */
	uint16_t number;	/* ordered list item number, 0 = bullet */
} doc_block_t;

typedef struct doc {
	char *text;
	uint32_t text_len, text_cap;
	doc_run_t *runs;
	uint32_t nruns, runs_cap;
	doc_block_t *blocks;
	uint32_t nblocks, blocks_cap;
	/* resolved link targets: "nsURL\0" strings, e.g. "CGreenhouse_gas" */
	char *links;
	uint32_t links_len, links_cap;
	uint32_t *link_off;
	uint32_t nlinks, link_off_cap;
	char title[256];
	/* a page that only sends the browser on (<meta http-equiv="refresh">,
	 * used for redirects to a section): its target link, or 0xffff, and
	 * the section it names ("Left quasi-reflexivity") */
	uint16_t refresh_link;
	char refresh_frag[96];
	/* footnote anchors: u32 text offset + name + NUL, packed */
	char *anchors;
	uint32_t anchors_len, anchors_cap;
} doc_t;

/* what html_to_doc shows besides the article: explanatory notes and
 * their [a] markers, citations and their [1] markers */
#define HTML_NOTES 1
#define HTML_CITES 2
void html_set_options(int opts);
void doc_add_anchor(doc_t *d, const char *id, uint32_t n);
int doc_find_anchor(const doc_t *d, const char *name, uint32_t *off);

void doc_init(doc_t *d);
void doc_free(doc_t *d);

/* convert one article.  base_ns/base_url locate the article so that
 * relative links can be resolved; new_ns is true for ZIM 6.1+ (C ns). */
int html_to_doc(doc_t *d, const char *html, uint32_t len,
		char base_ns, const char *base_url, int new_ns);

/* link target: *ns and NUL terminated url (ns 0, url "": this page) */
const char *doc_link(const doc_t *d, uint32_t i, char *ns);
/* the section the link names ("History"), or "" */
const char *doc_link_section(const doc_t *d, uint32_t i);

#endif
