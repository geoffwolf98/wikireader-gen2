/*
 * zimwiki - mwoffliner HTML to document conversion
 *
 * Streaming, tolerant parser with no DOM.  It keeps what reads well on
 * a 240x208 monochrome screen (text, headings, lists, infobox rows,
 * simple tables, links) and drops the rest (scripts, styles, images,
 * navboxes, citation markers, reference lists, edit links).
 */
#include "doc.h"
#include "tex.h"
#include "font.h"
#include "utf8.h"

#define STACK_MAX 96
#define MAX_INDENT 6

enum tag {
	T_OTHER, T_A, T_B, T_STRONG, T_I, T_EM, T_CITE, T_VAR, T_DFN, T_BR, T_HR,
	T_P, T_DIV, T_SPAN, T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_UL, T_OL, T_LI,
	T_DL, T_DT, T_DD, T_BLOCKQUOTE, T_TABLE, T_TR, T_TD, T_TH, T_CAPTION,
	T_SUP, T_SUB, T_SMALL, T_SCRIPT, T_STYLE, T_HEAD, T_TITLE, T_NOSCRIPT,
	T_TEMPLATE, T_MATH, T_IMG, T_FIGURE, T_SUMMARY, T_SECTION, T_PRE,
	T_LINK, T_META, T_INPUT, T_WBR, T_SVG, T_BUTTON, T_AUDIO, T_VIDEO,
	T_OBJECT, T_IFRAME, T_SOURCE, T_TRACK, T_COL, T_AREA, T_BASE, T_EMBED,
	T_PARAM, T_CENTER, T_MAP, T_DETAILS, T_ASIDE, T_NAV, T_HEADER, T_FOOTER,
	T_MAIN, T_ARTICLE, T_FIGCAPTION,
};

static const struct {
	const char *name;
	uint8_t tag;
} tags[] = {
	{"a", T_A}, {"b", T_B}, {"strong", T_STRONG}, {"i", T_I}, {"em", T_EM},
	{"cite", T_CITE}, {"var", T_VAR}, {"dfn", T_DFN}, {"br", T_BR},
	{"hr", T_HR}, {"p", T_P}, {"div", T_DIV}, {"span", T_SPAN}, {"h1", T_H1},
	{"h2", T_H2}, {"h3", T_H3}, {"h4", T_H4}, {"h5", T_H5}, {"h6", T_H6},
	{"ul", T_UL}, {"ol", T_OL}, {"li", T_LI}, {"dl", T_DL}, {"dt", T_DT},
	{"dd", T_DD}, {"blockquote", T_BLOCKQUOTE}, {"table", T_TABLE},
	{"tr", T_TR}, {"td", T_TD}, {"th", T_TH}, {"caption", T_CAPTION},
	{"sup", T_SUP}, {"sub", T_SUB}, {"small", T_SMALL}, {"script", T_SCRIPT},
	{"style", T_STYLE}, {"head", T_HEAD}, {"title", T_TITLE},
	{"noscript", T_NOSCRIPT}, {"template", T_TEMPLATE}, {"math", T_MATH},
	{"img", T_IMG}, {"figure", T_FIGURE}, {"summary", T_SUMMARY},
	{"section", T_SECTION}, {"pre", T_PRE}, {"link", T_LINK},
	{"meta", T_META}, {"input", T_INPUT}, {"wbr", T_WBR}, {"svg", T_SVG},
	{"button", T_BUTTON}, {"audio", T_AUDIO}, {"video", T_VIDEO},
	{"object", T_OBJECT}, {"iframe", T_IFRAME}, {"source", T_SOURCE},
	{"track", T_TRACK}, {"col", T_COL}, {"area", T_AREA}, {"base", T_BASE},
	{"embed", T_EMBED}, {"param", T_PARAM}, {"center", T_CENTER},
	{"map", T_MAP}, {"details", T_DETAILS}, {"aside", T_ASIDE}, {"nav", T_NAV},
	{"header", T_HEADER}, {"footer", T_FOOTER}, {"main", T_MAIN},
	{"article", T_ARTICLE}, {"figcaption", T_FIGCAPTION},
};

/* elements whose content is never shown */
static int tag_hidden(int t)
{
	switch (t) {
	case T_SCRIPT: case T_STYLE: case T_HEAD: case T_TITLE: case T_NOSCRIPT:
	case T_TEMPLATE: case T_IMG: case T_FIGURE: case T_SVG: case T_BUTTON:
	case T_NAV: case T_FIGCAPTION:
	case T_AUDIO: case T_VIDEO: case T_OBJECT: case T_IFRAME: case T_MAP:
		return 1;
	}
	return 0;
}

static int tag_void(int t)
{
	switch (t) {
	case T_BR: case T_HR: case T_IMG: case T_LINK: case T_META: case T_INPUT:
	case T_WBR: case T_SOURCE: case T_TRACK: case T_COL: case T_AREA:
	case T_BASE: case T_EMBED: case T_PARAM:
		return 1;
	}
	return 0;
}

static int html_opts;	/* HTML_NOTES, HTML_CITES */

void html_set_options(int o)
{
	html_opts = o;
}

static const char *const skip_classes[] = {
	"navbox", "vertical-navbox", "navbox-inner", "sidebar", "nomobile",
	"noprint", "mw-editsection", "reference",
	"references", "refbegin", "metadata", "ambox", "mbox-small",
	"thumb", "thumbinner", "gallery", "tmulti", "toc", "portal",
	"portalbox", "sistersitebox", "catlinks", "printfooter", "mw-jump-link",
	"authority-control", "navigation-not-searchable", "mw-empty-elt",
	"shortdescription", "mw-cite-backlink", "infobox-image", "mw-ref",
	"mw-kartographer-maplink", "locmap", "mwe-math-fallback-image-inline",
	"mwe-math-fallback-image-display", "noexcerpt", "box-Multiple_issues",
	"side-box", "plainlinks metadata", "mw-indicators", "magnify",
	"infobox-below", "navbar", "hlist navbar", "mw-heading-anchor",
	"pcs-collapse-table-collapsed-container", "pcs-edit-section-link-container",
	NULL
};

/* whole sections that are not worth the screen space */
static const char *const skip_sections[] = {
	"references", "external links", "further reading",
	"bibliography", "sources", "citations",
	"works cited", "general and cited sources", "cited sources", NULL
};

typedef struct elem {
	uint8_t tag;
	uint8_t skip;
	uint8_t kind, indent;	/* block context to restore on close */
	uint16_t number;
	uint16_t link;
	uint8_t bold, italic, small, sup, sub, cell_mode;
	uint16_t ol_count;
	uint16_t cells;
	uint8_t prev_th;
	uint8_t mark;		/* a footnote marker [a] / [1] */
	uint8_t alpha;		/* <ol> of notes: a. b. c. */
} elem_t;

typedef struct parser {
	doc_t *d;
	elem_t st[STACK_MAX];
	int depth;
	/* inline style */
	uint8_t bold, italic, small, sup, sub;
	uint16_t link;
	/* block context for the next text */
	uint8_t kind, indent;
	uint16_t number;
	int block_open;		/* a block has been started for this context */
	int space;		/* last output was white space */
	/* tables */
	int cell_mode;		/* inside a table row: blocks become separators */
	int cells;		/* cells emitted in the current row */
	int prev_th;
	uint32_t cell_start;	/* text_len when the current cell began */
	/* sections */
	int heading_level;	/* collecting a heading of this level, else 0 */
	uint32_t heading_block;
	int skip_level;		/* skipping a section up to this heading level */
	int small_footer;
	int pre;
	uint32_t last_math_end, last_math_hash;	/* MathML + its fallback image: show once */
	uint32_t file_mark;		/* text_len + 1 at the last mw:File span, or 0 */
	int rtl_seen;			/* text may hold right-to-left script */
	uint32_t mark_text, mark_runs;	/* where the open footnote marker began */
	int mark_space;
	const char *tag_end, *html_end;	/* just after the tag being opened */
	/* link resolution */
	char base_ns;
	const char *base_url;
	int new_ns;
	int err;
} parser_t;

void doc_init(doc_t *d)
{
	memset(d, 0, sizeof(*d));
	d->refresh_link = 0xffff;
}

void doc_free(doc_t *d)
{
	zw_free(d->text);
	zw_free(d->runs);
	zw_free(d->blocks);
	zw_free(d->links);
	zw_free(d->link_off);
	zw_free(d->anchors);
	doc_init(d);
}

static int grow(void **p, uint32_t *cap, uint32_t need, uint32_t elem)
{
	uint32_t ncap;
	void *n;
	if (need <= *cap)
		return 0;
	ncap = *cap ? *cap : 64;
	while (ncap < need)
		ncap *= 2;
	n = zw_realloc(*p, (size_t)*cap * elem, (size_t)ncap * elem);
	if (!n)
		return -1;
	*p = n;
	*cap = ncap;
	return 0;
}

const char *doc_link(const doc_t *d, uint32_t i, char *ns)
{
	const char *s;
	if (i >= d->nlinks)
		return NULL;
	s = d->links + d->link_off[i];
	*ns = s[0];
	return s + 1;
}

/* name ("cite_note-9", kept with '_' as space like link sections) and
 * the text offset where it is */
void doc_add_anchor(doc_t *d, const char *id, uint32_t n)
{
	uint32_t k;
	if (n > 80)
		return;
	if (grow((void **)&d->anchors, &d->anchors_cap, d->anchors_len + n + 5, 1))
		return;
	zw_wr32((uint8_t *)d->anchors + d->anchors_len, d->text_len);
	for (k = 0; k < n; ++k)
		d->anchors[d->anchors_len + 4 + k] = id[k] == '_' ? ' ' : id[k];
	d->anchors[d->anchors_len + 4 + n] = 0;
	d->anchors_len += n + 5;
}

int doc_find_anchor(const doc_t *d, const char *name, uint32_t *off)
{
	uint32_t i = 0;
	while (i + 4 < d->anchors_len) {
		const char *nm = d->anchors + i + 4;
		if (!strcmp(nm, name)) {
			*off = zw_rd32((const uint8_t *)d->anchors + i);
			return 0;
		}
		i += 4 + (uint32_t)strlen(nm) + 1;
	}
	return -1;
}

const char *doc_link_section(const doc_t *d, uint32_t i)
{
	const char *s;
	if (i >= d->nlinks)
		return "";
	s = d->links + d->link_off[i] + 1;
	return s + strlen(s) + 1;
}

/* ------------------------------------------------------------------ */
/* output */

static int cur_font(parser_t *p)
{
	switch (p->kind) {
	case BK_TITLE:
		return FONT_TITLE;
	case BK_H2: case BK_H3:
		return FONT_HEADING;
	case BK_H4: case BK_DT: case BK_CAPTION:
		return p->italic ? FONT_BOLDITALIC : FONT_BOLD;
	case BK_SMALL:
		return FONT_SMALL;
	}
	if (p->small_footer || (p->small && !p->bold))
		return FONT_SMALL;
	if (p->bold && p->italic)
		return FONT_BOLDITALIC;
	if (p->bold)
		return FONT_BOLD;
	if (p->italic)
		return FONT_ITALIC;
	return FONT_TEXT;
}

static void open_block(parser_t *p)
{
	doc_t *d = p->d;
	doc_block_t *b;
	if (p->block_open)
		return;
	if (grow((void **)&d->blocks, &d->blocks_cap, d->nblocks + 1, sizeof(doc_block_t))) {
		p->err = ZW_ENOMEM;
		return;
	}
	b = &d->blocks[d->nblocks++];
	b->first_run = d->nruns;
	b->nruns = 0;
	b->kind = p->small_footer && p->kind == BK_PARA ? BK_SMALL : p->kind;
	b->indent = p->indent > MAX_INDENT ? MAX_INDENT : p->indent;
	b->number = p->number;
	p->block_open = 1;
	p->space = 1;
	/* a list item's bullet only belongs to its first block */
	if (p->kind == BK_ITEM) {
		p->kind = BK_PARA;
		p->number = 0;
	}
}

static void end_block(parser_t *p)
{
	doc_t *d = p->d;
	if (p->block_open && d->nblocks) {
		doc_block_t *b = &d->blocks[d->nblocks - 1];
		/* trim a trailing space */
		while (b->nruns) {
			doc_run_t *r = &d->runs[b->first_run + b->nruns - 1];
			while (r->len && d->text[r->start + r->len - 1] == ' ')
				r->len--;
			if (r->len)
				break;
			b->nruns--;
			d->nruns--;
		}
		if (!b->nruns && b->kind != BK_RULE)
			d->nblocks--;	/* nothing was written */
	}
	p->block_open = 0;
	p->space = 1;
}

static void emit(parser_t *p, const char *s, uint32_t n)
{
	doc_t *d = p->d;
	doc_block_t *b;
	doc_run_t *r;
	int font, flags;

	if (!n || p->err || (p->depth && p->st[p->depth - 1].skip) || p->skip_level)
		return;
	if (!p->block_open)
		open_block(p);
	if (p->err)
		return;
	b = &d->blocks[d->nblocks - 1];
	font = cur_font(p);
	flags = (p->link != 0xffff ? RF_LINK : 0) | (p->sup ? RF_SUP : 0) | (p->sub ? RF_SUB : 0);
	if (flags & (RF_SUP | RF_SUB))
		font = FONT_SMALL;

	if (grow((void **)&d->text, &d->text_cap, d->text_len + n + 1, 1)) {
		p->err = ZW_ENOMEM;
		return;
	}
	r = b->nruns ? &d->runs[d->nruns - 1] : NULL;
	if (!r || r->font != font || r->flags != flags || r->link != p->link ||
	    r->start + r->len != d->text_len) {
		if (grow((void **)&d->runs, &d->runs_cap, d->nruns + 1, sizeof(doc_run_t))) {
			p->err = ZW_ENOMEM;
			return;
		}
		r = &d->runs[d->nruns++];
		r->start = d->text_len;
		r->len = 0;
		r->font = (uint8_t)font;
		r->flags = (uint8_t)flags;
		r->link = p->link;
		b->nruns++;
	}
	memcpy(d->text + d->text_len, s, n);
	d->text_len += n;
	r->len += n;
	d->text[d->text_len] = 0;
	p->space = s[n - 1] == ' ';
}

static void emit_space(parser_t *p)
{
	if (!p->space && p->block_open)
		emit(p, " ", 1);
}

/* right-to-left scripts (Hebrew, Arabic...): the WikiReader draws left to
 * right, so runs of them are stored in reverse, a base letter keeping its
 * marks after it.  Good enough for names and short phrases. */
static int is_rtl(uint32_t c)
{
	return (c >= 0x0590 && c <= 0x08ff) || (c >= 0xfb1d && c <= 0xfdff) ||
	       (c >= 0xfe70 && c <= 0xfeff);
}

static int is_rtl_mark(uint32_t c)
{
	return (c >= 0x0591 && c <= 0x05bd) || c == 0x05bf || c == 0x05c1 || c == 0x05c2 ||
	       c == 0x05c4 || c == 0x05c5 || c == 0x05c7 || (c >= 0x0610 && c <= 0x061a) ||
	       (c >= 0x064b && c <= 0x065f) || c == 0x0670 || (c >= 0x06d6 && c <= 0x06ed);
}

static void rtl_runs(char *buf, uint32_t k)
{
	uint32_t i = 0;
	while (i < k) {
		const uint8_t *q = (const uint8_t *)buf + i;
		uint32_t start, end, c, j;
		if ((uint8_t)buf[i] < 0xc0) {
			i++;
			continue;
		}
		c = utf8_next(&q, (const uint8_t *)buf + k);
		if (!is_rtl(c)) {
			i = (uint32_t)((const char *)q - buf);
			continue;
		}
		/* the run: RTL letters and marks, and spaces between them */
		start = i;
		end = j = (uint32_t)((const char *)q - buf);
		while (j < k) {
			const uint8_t *r = (const uint8_t *)buf + j;
			uint32_t d = utf8_next(&r, (const uint8_t *)buf + k);
			if (is_rtl(d)) {
				j = (uint32_t)((const char *)r - buf);
				end = j;
			} else if (d == ' ') {
				j++;
			} else {
				break;
			}
		}
		if (end - start > 2) {
			char tmp[512];
			uint32_t o = end - start, a = start;
			/* copy clusters (letter + its marks) from the front of the
			 * run to the back of tmp */
			while (a < end) {
				const uint8_t *r = (const uint8_t *)buf + a;
				uint32_t b, n;
				utf8_next(&r, (const uint8_t *)buf + end);
				b = (uint32_t)((const char *)r - buf);
				while (b < end) {
					const uint8_t *m = (const uint8_t *)buf + b;
					if (!is_rtl_mark(utf8_next(&m, (const uint8_t *)buf + end)))
						break;
					b = (uint32_t)((const char *)m - buf);
				}
				n = b - a;
				o -= n;
				memcpy(tmp + o, buf + a, n);
				a = b;
			}
			memcpy(buf + start, tmp, end - start);
		}
		i = end;
	}
}

/* text with white space collapsing.  Collapses into a small buffer
 * and emits it in pieces: one emit per word made long books slow */
static void text(parser_t *p, const char *s, uint32_t n)
{
	char buf[512];
	uint32_t i = 0, k = 0;
	int sp;
	if ((p->depth && p->st[p->depth - 1].skip) || p->skip_level)
		return;
	if (p->pre) {
		while (i < n) {
			uint32_t start;
			if (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r') {
				if (s[i] == '\n')
					end_block(p);
				else if (!p->space)
					emit(p, " ", 1);
				i++;
				continue;
			}
			start = i;
			while (i < n && s[i] != ' ' && s[i] != '\n' && s[i] != '\t' && s[i] != '\r')
				i++;
			emit(p, s + start, i - start);
		}
		return;
	}
	sp = p->space;
	for (i = 0; i < n; ++i) {
		char c = s[i];
		if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
			if (sp)
				continue;
			c = ' ';
			sp = 1;
		} else {
			sp = 0;
		}
		buf[k++] = c;
		if (k == sizeof(buf)) {
			if (p->rtl_seen)
				rtl_runs(buf, k);
			emit(p, buf, k);
			k = 0;
		}
		if ((uint8_t)c >= 0xd6 && (uint8_t)c <= 0xdf)
			p->rtl_seen = 1;	/* lead byte of U+0580..U+07FF */
		else if ((uint8_t)c == 0xef)
			p->rtl_seen = 1;	/* U+F000..U+FFFF: presentation forms */
		else if ((uint8_t)c == 0xe0)
			p->rtl_seen = 1;	/* U+0800..U+0FFF */
	}
	if (k) {
		if (p->rtl_seen)
			rtl_runs(buf, k);
		emit(p, buf, k);
	}
}

/* ------------------------------------------------------------------ */
/* entities */

static const struct {
	const char *name;
	uint16_t cp;
} entities[] = {
	{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
	{"nbsp", 0xa0}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026},
	{"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201c}, {"rdquo", 0x201d},
	{"laquo", 0xab}, {"raquo", 0xbb}, {"minus", 0x2212}, {"times", 0xd7},
	{"divide", 0xf7}, {"deg", 0xb0}, {"plusmn", 0xb1}, {"middot", 0xb7},
	{"bull", 0x2022}, {"copy", 0xa9}, {"reg", 0xae}, {"trade", 0x2122},
	{"euro", 0x20ac}, {"pound", 0xa3}, {"cent", 0xa2}, {"yen", 0xa5},
	{"sect", 0xa7}, {"para", 0xb6}, {"frac12", 0xbd}, {"frac14", 0xbc},
	{"frac34", 0xbe}, {"sup2", 0xb2}, {"sup3", 0xb3}, {"micro", 0xb5},
	{"prime", 0x2032}, {"Prime", 0x2033}, {"larr", 0x2190}, {"rarr", 0x2192},
	{"uarr", 0x2191}, {"darr", 0x2193}, {"harr", 0x2194}, {"thinsp", 0x2009},
	{"ensp", 0x2002}, {"emsp", 0x2003}, {"zwj", 0x200d}, {"zwnj", 0x200c},
	{"shy", 0xad}, {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260},
	{"asymp", 0x2248}, {"infin", 0x221e}, {"iexcl", 0xa1}, {"iquest", 0xbf},
	{"szlig", 0xdf}, {"eacute", 0xe9}, {"egrave", 0xe8}, {"aacute", 0xe1},
	{"agrave", 0xe0}, {"ouml", 0xf6}, {"uuml", 0xfc}, {"auml", 0xe4},
	{"ccedil", 0xe7}, {"ntilde", 0xf1}, {"oacute", 0xf3}, {"iacute", 0xed},
	{"uacute", 0xfa}, {"alpha", 0x3b1}, {"beta", 0x3b2}, {"gamma", 0x3b3},
	{"delta", 0x3b4}, {"pi", 0x3c0}, {"sigma", 0x3c3}, {"mu", 0x3bc},
	{"lambda", 0x3bb}, {"omega", 0x3c9}, {"theta", 0x3b8},
};

/* decode entities in s[0..n) into out (out may be s); returns new length */
static uint32_t decode_entities(const char *s, uint32_t n, char *out)
{
	uint32_t i = 0, o = 0;
	while (i < n) {
		if (s[i] == '&') {
			uint32_t j = i + 1, cp = 0;
			int ok = 0;
			while (j < n && j - i < 12 && s[j] != ';' && s[j] != '&' && s[j] != ' ')
				j++;
			if (j < n && s[j] == ';') {
				if (s[i + 1] == '#') {
					uint32_t k = i + 2;
					int hex = k < j && (s[k] == 'x' || s[k] == 'X');
					if (hex)
						k++;
					for (; k < j; ++k) {
						char c = s[k];
						int v = c >= '0' && c <= '9' ? c - '0' :
							hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 :
							hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
						if (v < 0 || cp > 0x10ffff)
							break;
						cp = cp * (hex ? 16 : 10) + (uint32_t)v;
					}
					ok = k == j && cp && cp <= 0x10ffff;
				} else {
					uint32_t e, len = j - i - 1;
					for (e = 0; e < ZW_COUNT(entities); ++e) {
						if (strlen(entities[e].name) == len &&
						    memcmp(entities[e].name, s + i + 1, len) == 0) {
							cp = entities[e].cp;
							ok = 1;
							break;
						}
					}
				}
			}
			if (ok) {
				uint8_t tmp[4];
				int k, m = utf8_put(tmp, cp);
				for (k = 0; k < m; ++k)
					out[o++] = (char)tmp[k];
				i = j + 1;
				continue;
			}
		}
		out[o++] = s[i++];
	}
	return o;
}

/* ------------------------------------------------------------------ */
/* attributes */

typedef struct attrs {
	const char *cls; uint32_t cls_len;
	const char *href; uint32_t href_len;
	const char *style; uint32_t style_len;
	const char *alt; uint32_t alt_len;
	const char *id; uint32_t id_len;
	const char *disp; uint32_t disp_len;	/* <math display="block"> */
	const char *content; uint32_t content_len;	/* <meta content=...> */
	const char *typ; uint32_t typ_len;	/* typeof="mw:File" */
	const char *grp; uint32_t grp_len;	/* data-mw-group: a notes list */
} attrs_t;

static void parse_attrs(const char *s, const char *end, attrs_t *a)
{
	memset(a, 0, sizeof(*a));
	while (s < end) {
		const char *name, *val = NULL;
		uint32_t nlen, vlen = 0;
		while (s < end && (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r' || *s == '/'))
			s++;
		name = s;
		while (s < end && *s != '=' && *s != ' ' && *s != '>' && *s != '\n' && *s != '\t' && *s != '/')
			s++;
		nlen = (uint32_t)(s - name);
		if (!nlen) {
			s++;
			continue;
		}
		while (s < end && (*s == ' ' || *s == '\n'))
			s++;
		if (s < end && *s == '=') {
			s++;
			while (s < end && (*s == ' ' || *s == '\n'))
				s++;
			if (s < end && (*s == '"' || *s == '\'')) {
				char q = *s++;
				val = s;
				while (s < end && *s != q)
					s++;
				vlen = (uint32_t)(s - val);
				if (s < end)
					s++;
			} else {
				val = s;
				while (s < end && *s != ' ' && *s != '>' && *s != '\n')
					s++;
				vlen = (uint32_t)(s - val);
			}
		}
#define ATTR(nm, field) \
		if (nlen == sizeof(nm) - 1 && strncasecmp(name, nm, nlen) == 0) { \
			a->field = val; a->field##_len = vlen; }
		ATTR("class", cls)
		else ATTR("href", href)
		else ATTR("style", style)
		else ATTR("alttext", alt)
		else if (nlen == 3 && strncasecmp(name, "alt", 3) == 0 && !a->alt) {
			a->alt = val;
			a->alt_len = vlen;
		}
		else ATTR("id", id)
		else ATTR("display", disp)
		else ATTR("content", content)
		else ATTR("typeof", typ)
		else ATTR("data-mw-group", grp)
#undef ATTR
	}
}

static int has_class(const attrs_t *a, const char *c)
{
	size_t n = strlen(c);
	const char *s = a->cls, *end = a->cls ? a->cls + a->cls_len : NULL;
	if (!s)
		return 0;
	while (s < end) {
		const char *w;
		while (s < end && *s == ' ')
			s++;
		w = s;
		while (s < end && *s != ' ')
			s++;
		if ((size_t)(s - w) == n && memcmp(w, c, n) == 0)
			return 1;
	}
	return 0;
}

static int contains(const char *s, uint32_t n, const char *needle)
{
	size_t k = strlen(needle);
	uint32_t i;
	for (i = 0; i + k <= n; ++i)
		if (strncasecmp(s + i, needle, k) == 0)
			return 1;
	return 0;
}

static int skip_by_attrs(const attrs_t *a)
{
	int i;
	/* the hidden MathML wrapper holds the TeX (alttext); it is the only
	 * copy in nopic ZIMs, which drop the fallback image */
	if (a->cls && contains(a->cls, a->cls_len, "mwe-math-mathml"))
		return 0;
	if (a->style && (contains(a->style, a->style_len, "display:none") ||
			 contains(a->style, a->style_len, "display: none")))
		return 1;
	/* explanatory notes ({{efn}}, {{notelist}}): a reference list with
	 * a group (lower-alpha...) is shown; citations (no group) are not */
	if (a->grp && a->grp_len)
		return !(html_opts & HTML_NOTES);
	if (a->cls && html_opts) {
		/* markers and the ^ links back to them; a marker of a kind that
		 * is switched off is removed when it closes */
		if (contains(a->cls, a->cls_len, "reference") && !contains(a->cls, a->cls_len, "references") &&
		    !contains(a->cls, a->cls_len, "reference-text"))
			return 0;
		if (contains(a->cls, a->cls_len, "mw-cite-backlink"))
			return 0;
		if (contains(a->cls, a->cls_len, "references") && (html_opts & HTML_CITES))
			return 0;
	}
	if (a->cls) {
		/* each class word against the skip list, by length first */
		static uint8_t slen[64], ready;
		const char *c = a->cls, *end = a->cls + a->cls_len;
		if (!ready) {
			for (i = 0; skip_classes[i] && i < 64; ++i)
				slen[i] = (uint8_t)strlen(skip_classes[i]);
			ready = 1;
		}
		while (c < end) {
			const char *w;
			uint32_t n;
			while (c < end && *c == ' ')
				c++;
			w = c;
			while (c < end && *c != ' ')
				c++;
			n = (uint32_t)(c - w);
			if (!n)
				continue;
			for (i = 0; skip_classes[i] && i < 64; ++i)
				if (slen[i] == n && w[0] == skip_classes[i][0] &&
				    memcmp(w, skip_classes[i], n) == 0)
					return 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* links */

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* the "#Section" part of href, decoded ('_' as space) into out (cap
 * bytes with the NUL); returns its length */
static uint32_t link_frag(const char *href, uint32_t n, char *out, uint32_t cap)
{
	uint32_t i = 0, o = 0;
	while (i < n && href[i] != '#')
		i++;
	for (i++; i < n && o + 1 < cap; ++i) {
		char ch = href[i];
		if (ch == '%' && i + 2 < n && hexval(href[i + 1]) >= 0 && hexval(href[i + 2]) >= 0) {
			ch = (char)(hexval(href[i + 1]) * 16 + hexval(href[i + 2]));
			i += 2;
		}
		out[o++] = ch == '_' ? ' ' : ch;
	}
	out[o] = 0;
	return o;
}

/* resolve href against the article; returns link index or 0xffff.
 * Stored as ns, url, NUL, section, NUL; a link within this page has ns 0
 * and an empty url */
static uint16_t add_link(parser_t *p, const char *href, uint32_t n)
{
	doc_t *d = p->d;
	char path[1024], dec[600];
	uint32_t i, o = 0, k;
	char *seg[64];
	int nseg = 0, s;
	char ns;
	size_t need;

	char frag[96];
	uint32_t flen;

	if (!n || d->nlinks >= 0xfffe)
		return 0xffff;
	flen = link_frag(href, n, frag, sizeof(frag));
	if (href[0] == '#') {
		/* within this page: only worth keeping with a section name */
		if (!flen || grow((void **)&d->links, &d->links_cap, d->links_len + flen + 4, 1) ||
		    grow((void **)&d->link_off, &d->link_off_cap, d->nlinks + 1, sizeof(uint32_t)))
			return 0xffff;
		d->link_off[d->nlinks] = d->links_len;
		d->links[d->links_len++] = 0;
		d->links[d->links_len++] = 0;
		memcpy(d->links + d->links_len, frag, flen + 1);
		d->links_len += flen + 1;
		return (uint16_t)d->nlinks++;
	}
	if (contains(href, n > 8 ? 8 : n, ":") ||
	    (n > 1 && href[0] == '/' && href[1] == '/'))
		return 0xffff;	/* anchors, http:, mailto:, protocol relative */
	/* percent and entity decode, stop at fragment or query */
	for (i = 0; i < n && o < sizeof(dec) - 1; ++i) {
		if (href[i] == '#' || href[i] == '?')
			break;
		if (href[i] == '%' && i + 2 < n && hexval(href[i + 1]) >= 0 && hexval(href[i + 2]) >= 0) {
			dec[o++] = (char)(hexval(href[i + 1]) * 16 + hexval(href[i + 2]));
			i += 2;
		} else {
			dec[o++] = href[i];
		}
	}
	o = decode_entities(dec, o, dec);
	dec[o] = 0;
	if (!o)
		return 0xffff;

	/* base directory */
	{
		size_t bl = strlen(p->base_url), at = 0;
		if (bl > sizeof(path) - 4)
			return 0xffff;
		if (!p->new_ns) {
			path[at++] = p->base_ns;
			path[at++] = '/';
		}
		memcpy(path + at, p->base_url, bl + 1);
	}
	{
		char *slash = strrchr(path, '/');
		if (dec[0] == '/')
			path[0] = 0;
		else if (slash)
			slash[1] = 0;
		else
			path[0] = 0;
	}
	k = (uint32_t)strlen(path);
	if (k + o + 1 >= sizeof(path))
		return 0xffff;
	memcpy(path + k, dec[0] == '/' ? dec + 1 : dec, o + 1);

	/* normalise . and .. */
	{
		char *q = path;
		while (*q && nseg < 64) {
			char *start = q;
			while (*q && *q != '/')
				q++;
			if (*q)
				*q++ = 0;
			if (strcmp(start, ".") == 0 || !*start)
				continue;
			if (strcmp(start, "..") == 0) {
				if (nseg)
					nseg--;
				continue;
			}
			seg[nseg++] = start;
		}
	}
	if (!nseg)
		return 0xffff;
	s = 0;
	if (p->new_ns) {
		ns = 'C';
	} else {
		if (strlen(seg[0]) != 1 || nseg < 2)
			return 0xffff;
		ns = seg[0][0];
		s = 1;
	}
	need = 3 + flen;
	for (i = (uint32_t)s; i < (uint32_t)nseg; ++i)
		need += strlen(seg[i]) + 1;
	if (grow((void **)&d->links, &d->links_cap, d->links_len + (uint32_t)need + 1, 1) ||
	    grow((void **)&d->link_off, &d->link_off_cap, d->nlinks + 1, sizeof(uint32_t))) {
		p->err = ZW_ENOMEM;
		return 0xffff;
	}
	d->link_off[d->nlinks] = d->links_len;
	d->links[d->links_len++] = ns;
	for (i = (uint32_t)s; i < (uint32_t)nseg; ++i) {
		size_t l = strlen(seg[i]);
		if (i > (uint32_t)s)
			d->links[d->links_len++] = '/';
		memcpy(d->links + d->links_len, seg[i], l);
		d->links_len += (uint32_t)l;
	}
	d->links[d->links_len++] = 0;
	memcpy(d->links + d->links_len, frag, flen + 1);
	d->links_len += flen + 1;
	return (uint16_t)d->nlinks++;
}

/* ------------------------------------------------------------------ */
/* elements */

static void set_block(parser_t *p, int kind)
{
	end_block(p);
	p->kind = (uint8_t)kind;
}

static int heading_kind(int t)
{
	return t == T_H1 ? BK_TITLE : t == T_H2 ? BK_H2 : t == T_H3 ? BK_H3 : BK_H4;
}

static void cell_separator(parser_t *p, const char *sep)
{
	if (p->d->nblocks && p->block_open) {
		uint8_t b = p->bold;
		doc_run_t *r = p->d->nruns ? &p->d->runs[p->d->nruns - 1] : NULL;
		/* drop the space before the separator */
		if (p->d->text_len >= 2 && p->d->text[p->d->text_len - 1] == ' ' &&
		    (p->d->text[p->d->text_len - 2] == ',' || p->d->text[p->d->text_len - 2] == ':'))
			return;
		if (r && r->len && p->d->text[r->start + r->len - 1] == ' ' &&
		    r->start + r->len == p->d->text_len) {
			r->len--;
			p->d->text_len--;
		}
		/* "Total,<br>Semiconnex": the text already ends in punctuation,
		 * so a space is enough */
		if (sep[0] == ',' && p->d->text_len &&
		    (p->d->text[p->d->text_len - 1] == ',' || p->d->text[p->d->text_len - 1] == ';' ||
		     p->d->text[p->d->text_len - 1] == ':' || p->d->text[p->d->text_len - 1] == '-')) {
			if (p->d->text[p->d->text_len - 1] != '-')
				emit(p, " ", 1);
			return;
		}
		/* never two separators in a row */
		if (p->d->text_len >= 2 && p->d->text[p->d->text_len - 1] == ' ' &&
		    (p->d->text[p->d->text_len - 2] == ',' || p->d->text[p->d->text_len - 2] == ':' ||
		     p->d->text[p->d->text_len - 2] == ';' ||
		     (unsigned char)p->d->text[p->d->text_len - 2] == 0xb7))
			return;
		/* an empty cell after a separator: the trim above took its
		 * space, so the text ends in the dot itself */
		if (p->d->text_len && (unsigned char)p->d->text[p->d->text_len - 1] == 0xb7) {
			emit(p, " ", 1);
			return;
		}
		p->bold = 0;
		emit(p, sep, (uint32_t)strlen(sep));
		p->bold = b;
		p->space = 1;
	}
}

/* one piece of a formula from tex_render */
static void math_out(void *ctx, const char *s, uint32_t n, int style)
{
	parser_t *p = (parser_t *)ctx;
	uint8_t sup = p->sup, sub = p->sub, it = p->italic, bold = p->bold;
	if (style & TEX_SUP)
		p->sup++;
	if (style & TEX_SUB)
		p->sub++;
	if (!(style & TEX_UPRIGHT))
		p->italic++;
	if (style & TEX_BOLD)
		p->bold++;
	text(p, s, n);
	p->sup = sup;
	p->sub = sub;
	p->italic = it;
	p->bold = bold;
}

/* ------------------------------------------------------------------ */
/* display matrices: one line per row, columns lined up, drawn brackets.
 * Only simple ones (a matrix alone, or "A = matrix", short cells); the
 * rest keep the one line form [a, b; c, d]. */

#define MX_ROWS 8
#define MX_COLS 8
#define MX_GAP 10		/* px between columns */
#define MX_WIDTH 196		/* px a row may take (screen minus indent) */

typedef struct { const char *s; uint32_t n; } span_t;

static uint32_t match_brace(const char *s, uint32_t i, uint32_t end)
{
	int d = 0;
	for (; i < end; ++i) {
		if (s[i] == '\\' && i + 1 < end) {
			i++;
			continue;
		}
		if (s[i] == '{')
			d++;
		else if (s[i] == '}' && --d == 0)
			return i;
	}
	return end;
}

static void mx_measure(void *ctx, const char *s, uint32_t n, int style)
{
	int *w = (int *)ctx, f;
	if (style & (TEX_SUP | TEX_SUB))
		f = FONT_SMALL;
	else if (style & TEX_UPRIGHT)
		f = style & TEX_BOLD ? FONT_BOLD : FONT_TEXT;
	else
		f = style & TEX_BOLD ? FONT_BOLDITALIC : FONT_ITALIC;
	*w += font_width(f, s, n);
}

static int tex_width(const char *s, uint32_t n)
{
	int w = 0;
	tex_render(s, n, mx_measure, &w);
	return w;
}

static void mx_pad(parser_t *p, int px)
{
	static const char hair[] = "\xe2\x80\x8a\xe2\x80\x8a\xe2\x80\x8a\xe2\x80\x8a"
				   "\xe2\x80\x8a\xe2\x80\x8a\xe2\x80\x8a\xe2\x80\x8a";
	while (px > 0) {
		int k = px > 8 ? 8 : px;
		text(p, hair, (uint32_t)k * 3);
		px -= k;
	}
}

static void mx_piece(parser_t *p, uint32_t cp)
{
	char u[4];
	u[0] = (char)(0xe0 | (cp >> 12));
	u[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
	u[2] = (char)(0x80 | (cp & 0x3f));
	u[3] = 0;
	text(p, u, 3);
}

/* depth-0 position of needle in [a, b), or b */
static uint32_t mx_find(const char *s, uint32_t a, uint32_t b, const char *needle)
{
	uint32_t k = (uint32_t)strlen(needle), i;
	int lev = 0;
	for (i = a; i + k <= b; ++i) {
		if (s[i] == '{')
			lev++;
		else if (s[i] == '}')
			lev--;
		else if (!lev && !memcmp(s + i, needle, k))
			return i;
		if (s[i] == '\\' && i + 1 < b && !lev && memcmp(needle, "\\", 1) != 0)
			i++;	/* \& \{ and the like */
	}
	return b;
}

static void mx_trim(const char *s, uint32_t *a, uint32_t *b)
{
	while (*a < *b && (s[*a] == ' ' || s[*a] == '\n'))
		(*a)++;
	while (*b > *a && (s[*b - 1] == ' ' || s[*b - 1] == '\n'))
		(*b)--;
}

static int math_matrix(parser_t *p, const char *t, uint32_t n)
{
	static const char *const envs[] = { "bmatrix", "pmatrix", "vmatrix", "Vmatrix", "matrix", NULL };
	span_t cell[MX_ROWS][MX_COLS];
	int cw[MX_ROWS][MX_COLS], colw[MX_COLS], ncol[MX_ROWS];
	uint32_t a = 0, b = n, beg, bs, be, end, i, k, sfx_a, sfx_b;
	int env = -1, rows = 0, cols = 0, r, c, prefw = 0, total;
	char endtag[24];
	uint8_t kind0 = p->kind, ind0 = p->indent;

	mx_trim(t, &a, &b);
	/* drop braces around the whole thing */
	while (b - a >= 2 && t[a] == '{' && match_brace(t, a, b) == b - 1) {
		a++;
		b--;
		mx_trim(t, &a, &b);
	}
	beg = mx_find(t, a, b, "\\begin{");
	if (beg == b || beg - a > 24)
		return 0;
	for (k = 0; envs[k]; ++k) {
		uint32_t l = (uint32_t)strlen(envs[k]);
		if (beg + 7 + l + 1 <= b && !memcmp(t + beg + 7, envs[k], l) && t[beg + 7 + l] == '}') {
			env = (int)k;
			bs = beg + 7 + l + 1;
			break;
		}
	}
	if (env < 0)
		return 0;
	/* (no snprintf: the device's mini libc one is not dependable) */
	memcpy(endtag, "\\end{", 5);
	memcpy(endtag + 5, envs[env], strlen(envs[env]));
	endtag[5 + strlen(envs[env])] = '}';
	endtag[6 + strlen(envs[env])] = 0;
	/* the body must not hold another environment */
	be = bs;
	{
		const char *e = NULL;
		uint32_t j;
		for (j = bs; j + strlen(endtag) <= b; ++j)
			if (!memcmp(t + j, endtag, strlen(endtag))) {
				e = t + j;
				break;
			}
		if (!e)
			return 0;
		be = (uint32_t)(e - t);
		for (j = bs; j + 7 <= be; ++j)
			if (!memcmp(t + j, "\\begin{", 7))
				return 0;
	}
	end = be + (uint32_t)strlen(endtag);
	/* after it only a little punctuation */
	sfx_a = end;
	sfx_b = b;
	mx_trim(t, &sfx_a, &sfx_b);
	if (sfx_b - sfx_a > 2)
		return 0;
	for (i = sfx_a; i < sfx_b; ++i)
		if (t[i] != '.' && t[i] != ',' && t[i] != ';')
			return 0;
	/* rows and cells */
	i = bs;
	while (i < be && rows < MX_ROWS + 1) {
		uint32_t re = mx_find(t, i, be, "\\\\"), ce, cs = i;
		if (rows == MX_ROWS)
			return 0;
		ncol[rows] = 0;
		while (cs <= re) {
			uint32_t x, y;
			ce = mx_find(t, cs, re, "&");
			if (ncol[rows] == MX_COLS)
				return 0;
			x = cs;
			y = ce;
			mx_trim(t, &x, &y);
			if (y - x > 40 || mx_find(t, x, y, "\\hline") < y)
				return 0;
			cell[rows][ncol[rows]].s = t + x;
			cell[rows][ncol[rows]].n = y - x;
			ncol[rows]++;
			if (ce >= re)
				break;
			cs = ce + 1;
		}
		/* a trailing \\ leaves an empty last row */
		if (!(ncol[rows] == 1 && cell[rows][0].n == 0))
			rows++;
		i = re < be ? re + 2 : be;
	}
	if (rows < 2)
		return 0;
	for (r = 0; r < rows; ++r)
		if (ncol[r] > cols)
			cols = ncol[r];
	memset(colw, 0, sizeof(colw));
	for (r = 0; r < rows; ++r)
		for (c = 0; c < cols; ++c) {
			cw[r][c] = c < ncol[r] ? tex_width(cell[r][c].s, cell[r][c].n) : 0;
			if (cw[r][c] > colw[c])
				colw[c] = cw[r][c];
		}
	{
		uint32_t pa = a, pb = beg;
		mx_trim(t, &pa, &pb);
		prefw = pb > pa ? tex_width(t + pa, pb - pa) + 4 : 0;
		total = prefw + 2 * (5 + 3) + (cols - 1) * MX_GAP;
		for (c = 0; c < cols; ++c)
			total += colw[c];
		if (total > MX_WIDTH)
			return 0;
		/* draw: row by row, prefix on the middle row */
		for (r = 0; r < rows; ++r) {
			uint32_t lp, mp, rp;
			int pos = r == 0 ? 0 : r == rows - 1 ? 2 : 1;
			if (r) {
				set_block(p, BK_MROW);
				p->indent = ind0;
			}
			p->space = 1;
			if (prefw) {
				if (r == (rows - 1) / 2) {
					int w0 = tex_width(t + pa, pb - pa);
					tex_render(t + pa, pb - pa, math_out, p);
					mx_pad(p, prefw - w0);
				} else
					mx_pad(p, prefw);
			}
			switch (env) {
			case 0: lp = FONT_PIECE + pos; rp = FONT_PIECE + 3 + pos; break;
			case 1: lp = FONT_PIECE + 6 + pos; rp = FONT_PIECE + 9 + pos; break;
			case 2: lp = rp = FONT_PIECE + 12; break;
			case 3: lp = rp = FONT_PIECE + 13; break;
			default: lp = rp = 0; break;
			}
			mp = 0;
			(void)mp;
			if (lp)
				mx_piece(p, lp);
			mx_pad(p, lp ? 3 : 8);
			for (c = 0; c < cols; ++c) {
				int w = cw[r][c], left = (colw[c] - w) / 2;
				mx_pad(p, left);
				if (c < ncol[r] && cell[r][c].n)
					tex_render(cell[r][c].s, cell[r][c].n, math_out, p);
				mx_pad(p, colw[c] - w - left + (c < cols - 1 ? MX_GAP : 0));
			}
			mx_pad(p, rp ? 3 : 0);
			if (rp)
				mx_piece(p, rp);
			if (r == rows - 1 && sfx_b > sfx_a)
				text(p, t + sfx_a, sfx_b - sfx_a);
		}
	}
	set_block(p, kind0);
	p->indent = ind0;
	p->space = 1;
	return 1;
}

static uint32_t alt_hash(const char *s, uint32_t n)
{
	uint32_t h = 2166136261u, i;
	for (i = 0; i < n; ++i)
		h = (h ^ (uint8_t)s[i]) * 16777619u;
	return h;
}

/* <meta http-equiv="refresh" content="0;URL='./Page#Section'">: remember
 * where it leads (the app opens that instead) */
static void meta_refresh(parser_t *p, const char *c, uint32_t n)
{
	uint32_t i, k, o = 0;
	const char *u = NULL;
	char *f = p->d->refresh_frag;
	for (i = 0; i + 4 <= n; ++i)
		if (strncasecmp(c + i, "url=", 4) == 0) {
			u = c + i + 4;
			break;
		}
	if (!u)
		return;
	n -= (uint32_t)(u - c);
	while (n && (*u == '\'' || *u == '"' || *u == ' ')) {
		u++;
		n--;
	}
	while (n && (u[n - 1] == '\'' || u[n - 1] == '"' || u[n - 1] == ' '))
		n--;
	if (!n)
		return;
	p->d->refresh_link = add_link(p, u, n);
	/* the section: after '#', decoded, '_' as space */
	for (i = 0; i < n && u[i] != '#'; ++i)
		;
	for (k = i + 1; k < n && o < sizeof(p->d->refresh_frag) - 1; ++k) {
		char ch = u[k];
		if (ch == '%' && k + 2 < n && hexval(u[k + 1]) >= 0 && hexval(u[k + 2]) >= 0) {
			ch = (char)(hexval(u[k + 1]) * 16 + hexval(u[k + 2]));
			k += 2;
		}
		f[o++] = ch == '_' ? ' ' : ch;
	}
	f[o] = 0;
}

static void open_tag(parser_t *p, int t, const attrs_t *a, int self_closing)
{
	elem_t *e;
	int parent_skip = p->depth ? p->st[p->depth - 1].skip : 0;
	int skip = parent_skip || tag_hidden(t) || skip_by_attrs(a);

	if (t == T_META && a->content && p->d->refresh_link == 0xffff)
		meta_refresh(p, a->content, a->content_len);
	/* {{Y}} and {{N}} ticks: a picture (gone in nopic ZIMs) then a hidden
	 * "Y" or "N" for screen readers.  Show a tick or cross instead */
	if (t == T_SPAN && !parent_skip && !p->skip_level) {
		if (a->typ && contains(a->typ, a->typ_len, "mw:File")) {
			p->file_mark = p->d->text_len + 1;
		} else if (p->file_mark == p->d->text_len + 1 && a->style &&
			   (contains(a->style, a->style_len, "display:none") ||
			    contains(a->style, a->style_len, "display: none")) &&
			   p->tag_end + 1 < p->html_end && p->tag_end[1] == '<' &&
			   (p->tag_end[0] == 'Y' || p->tag_end[0] == 'N')) {
			emit(p, p->tag_end[0] == 'Y' ? "\xe2\x9c\x93" : "\xe2\x9c\x97", 3);
			p->file_mark = 0;
		}
	}

	/* MathML and math images: show the TeX source text */
	if (!parent_skip && !p->skip_level && (t == T_MATH ||
	    (t == T_IMG && a->cls && contains(a->cls, a->cls_len, "mwe-math"))) &&
	    a->alt && !(p->last_math_end == p->d->text_len &&
			p->last_math_hash == alt_hash(a->alt, a->alt_len))) {
		char buf[2048];
		uint32_t n = a->alt_len < sizeof(buf) ? a->alt_len : sizeof(buf) - 1;
		uint32_t s0 = 0;
		n = decode_entities(a->alt, n, buf);
		if (n > 14 && memcmp(buf, "{\\displaystyle", 14) == 0) {
			s0 = 14;
			while (s0 < n && buf[s0] == ' ')
				s0++;
			if (n > s0 && buf[n - 1] == '}')
				n--;
		}
		{
			/* display maths inside a paragraph gets a line of its own,
			 * indented like a <dd> formula */
			int block = !p->cell_mode && p->kind == BK_PARA &&
				((a->disp && a->disp_len == 5 && !memcmp(a->disp, "block", 5)) ||
				 (t == T_IMG && contains(a->cls, a->cls_len, "image-display")));
			uint8_t kind = p->kind, ind = p->indent;
			if (block) {
				set_block(p, BK_DD);
				p->indent = ind + 1;
				p->space = 1;
			}
			if (!((block || (p->kind == BK_DD && !p->cell_mode)) &&
			      math_matrix(p, buf + s0, n - s0)))
				tex_render(buf + s0, n - s0, math_out, p);
			if (block) {
				set_block(p, kind);
				p->indent = ind;
				p->space = 1;
			}
		}
		p->last_math_end = p->d->text_len;
		p->last_math_hash = alt_hash(a->alt, a->alt_len);
		skip = 1;
	}

	if (!skip) {
		switch (t) {
		case T_BR:
			if (p->cell_mode) {
				if (p->block_open && p->d->text_len > p->cell_start)
					cell_separator(p, ", ");
			} else
				end_block(p);
			break;
		case T_HR:
			if (!p->cell_mode && !p->skip_level) {
				set_block(p, BK_RULE);
				open_block(p);
				end_block(p);
				p->kind = BK_PARA;
			}
			break;
		case T_WBR:
			break;
		}
	}
	if (tag_void(t) || self_closing)
		return;
	if (p->depth >= STACK_MAX) {
		p->depth--;	/* drop the innermost; very deep nesting is junk */
	}
	e = &p->st[p->depth++];
	e->tag = (uint8_t)t;
	e->skip = (uint8_t)skip;
	e->kind = p->kind;
	e->indent = p->indent;
	e->number = p->number;
	e->link = p->link;
	e->bold = p->bold;
	e->italic = p->italic;
	e->small = p->small;
	e->sup = p->sup;
	e->sub = p->sub;
	e->cell_mode = (uint8_t)p->cell_mode;
	e->ol_count = 0;
	e->cells = (uint16_t)p->cells;
	e->prev_th = (uint8_t)p->prev_th;
	e->mark = 0;
	e->alpha = 0;
	if (skip)
		return;
	/* footnote anchors: the note (<li id="cite_note-9">) and the marker
	 * (<sup id="cite_ref-9">), for the jumps between them */
	if (html_opts && a->id && a->id_len > 5 && !memcmp(a->id, "cite_", 5))
		doc_add_anchor(p->d, a->id, a->id_len);
	if (t == T_SUP && html_opts && a->cls && contains(a->cls, a->cls_len, "reference")) {
		e->mark = 1;
		p->mark_text = p->d->text_len;
		p->mark_runs = p->d->nruns;
		p->mark_space = p->space;
	}
	if (t == T_OL && a->grp && a->grp_len && contains(a->grp, a->grp_len, "alpha"))
		e->alpha = 1;

	switch (t) {
	case T_B: case T_STRONG:
		p->bold++;
		break;
	case T_I: case T_EM: case T_CITE: case T_VAR: case T_DFN:
		p->italic++;
		break;
	case T_SMALL:
		p->small++;
		break;
	case T_SUP:
		p->sup++;
		break;
	case T_SUB:
		p->sub++;
		break;
	case T_A:
		if (a->href && !has_class(a, "external") && !has_class(a, "new"))
			p->link = add_link(p, a->href, a->href_len);
		break;
	case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: {
		int level = t - T_H1 + 1;
		if (p->skip_level && level <= p->skip_level)
			p->skip_level = 0;
		if (p->cell_mode) {
			p->bold++;
			break;
		}
		set_block(p, heading_kind(t));
		p->indent = 0;
		p->heading_level = level;
		p->heading_block = p->d->nblocks;
		break;
	}
	case T_P: case T_DIV: case T_SECTION: case T_CENTER: case T_SUMMARY:
	case T_DETAILS: case T_ASIDE: case T_HEADER: case T_FOOTER: case T_MAIN:
	case T_ARTICLE:
		if (p->cell_mode)
			emit_space(p);
		else
			set_block(p, p->kind == BK_ITEM ? BK_ITEM : (p->kind == BK_DD ? BK_DD : BK_PARA));
		break;
	case T_PRE:
		set_block(p, BK_PARA);
		p->pre = 1;
		break;
	case T_UL: case T_OL: case T_DL:
		if (p->cell_mode)
			break;
		end_block(p);
		if (t != T_DL && p->indent < 250)
			p->indent++;
		p->kind = BK_PARA;
		break;
	case T_LI:
		if (p->cell_mode) {
			if (p->block_open && p->d->text_len > p->cell_start)
				cell_separator(p, ", ");
			break;
		}
		set_block(p, BK_ITEM);
		{
			/* number if the nearest list is an <ol> */
			int k;
			p->number = 0;
			for (k = p->depth - 2; k >= 0; --k) {
				if (p->st[k].tag == T_OL) {
					p->number = ++p->st[k].ol_count;
					if (p->st[k].alpha && p->number < 0x4000)
						p->number |= 0x4000;	/* drawn as a. b. c. */
					break;
				}
				if (p->st[k].tag == T_UL)
					break;
			}
		}
		break;
	case T_DT:
		if (!p->cell_mode)
			set_block(p, BK_DT);
		break;
	case T_DD:
		if (!p->cell_mode) {
			set_block(p, BK_DD);
			if (p->indent < 250)
				p->indent++;
		}
		break;
	case T_BLOCKQUOTE:
		set_block(p, BK_QUOTE);
		if (p->indent < 250)
			p->indent++;
		break;
	case T_TABLE:
		/* nested tables get rows of their own */
		end_block(p);
		p->cell_mode = 0;
		break;
	case T_CAPTION:
		if (!p->cell_mode)
			set_block(p, BK_CAPTION);
		break;
	case T_TR:
		set_block(p, BK_ROW);
		p->cell_mode = 1;
		p->cells = 0;
		p->prev_th = 0;
		break;
	case T_TD: case T_TH:
		if (p->cells && p->block_open)
			cell_separator(p, p->prev_th && t == T_TD ? ": " : "  \xc2\xb7  ");
		p->cells++;
		p->prev_th = t == T_TH;
		p->cell_start = p->d->text_len;
		if (t == T_TH)
			p->bold++;
		break;
	}
}

static int section_is_skipped(parser_t *p)
{
	doc_t *d = p->d;
	doc_block_t *b;
	char t[64];
	uint32_t n = 0, i, k;
	if (p->heading_block >= d->nblocks)
		return 0;
	b = &d->blocks[p->heading_block];
	for (i = 0; i < b->nruns && n < sizeof(t) - 1; ++i) {
		doc_run_t *r = &d->runs[b->first_run + i];
		for (k = 0; k < r->len && n < sizeof(t) - 1; ++k) {
			char c = d->text[r->start + k];
			t[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
		}
	}
	t[n] = 0;
	if ((html_opts & HTML_CITES) && (!strcmp(t, "references") || !strcmp(t, "citations")))
		return 0;	/* citations switched on */
	for (i = 0; skip_sections[i]; ++i)
		if (strcmp(t, skip_sections[i]) == 0)
			return 1;
	return 0;
}

/* a footnote marker just closed: [a] (letters) is a note, [1] a
 * citation.  Remove it again when its kind is switched off */
static void end_marker(parser_t *p)
{
	doc_t *d = p->d;
	uint32_t i;
	int letters = 0, keep;
	for (i = p->mark_text; i < d->text_len; ++i) {
		char c = d->text[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
			letters = 1;
	}
	keep = letters ? (html_opts & HTML_NOTES) : (html_opts & HTML_CITES);
	if (keep || d->text_len == p->mark_text)
		return;
	while (d->nruns > p->mark_runs) {
		doc_run_t *r = &d->runs[d->nruns - 1];
		if (r->start < p->mark_text)
			break;
		d->nruns--;
		if (d->nblocks && d->blocks[d->nblocks - 1].nruns)
			d->blocks[d->nblocks - 1].nruns--;
	}
	if (d->nruns) {
		doc_run_t *r = &d->runs[d->nruns - 1];
		if (r->start + r->len > p->mark_text)
			r->len = p->mark_text - r->start;
	}
	d->text_len = p->mark_text;
	if (d->text)
		d->text[d->text_len] = 0;
	p->space = p->mark_space;
}

static void close_tag(parser_t *p, int t)
{
	int k;
	elem_t *e;

	for (k = p->depth - 1; k >= 0; --k)
		if (p->st[k].tag == t)
			break;
	if (k < 0)
		return;		/* stray close tag */
	/* implicitly close anything left open inside it */
	while (p->depth > k + 1)
		close_tag(p, p->st[p->depth - 1].tag);

	e = &p->st[--p->depth];
	if (e->mark && !e->skip)
		end_marker(p);
	if (!e->skip) {
		switch (t) {
		case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
			if (p->cell_mode)
				break;
			end_block(p);
			if (p->heading_level && t != T_H1 && section_is_skipped(p)) {
				if (p->heading_block < p->d->nblocks) {
					p->d->nruns = p->d->blocks[p->heading_block].first_run;
					p->d->nblocks = p->heading_block;
				}
				p->skip_level = t - T_H1 + 1;
			}
			p->heading_level = 0;
			break;
		case T_P: case T_DIV: case T_SECTION: case T_CENTER: case T_SUMMARY:
		case T_DETAILS: case T_ASIDE: case T_HEADER: case T_FOOTER: case T_MAIN:
		case T_ARTICLE: case T_LI: case T_DT: case T_DD: case T_BLOCKQUOTE: case T_CAPTION:
		case T_UL: case T_OL: case T_DL:
			if (!e->cell_mode)
				end_block(p);
			break;
		case T_PRE:
			end_block(p);
			p->pre = 0;
			break;
		case T_TR:
			if (!e->cell_mode)
				end_block(p);
			break;
		}
		if (t == T_TABLE) {
			end_block(p);
			p->cells = e->cells;
			p->prev_th = e->prev_th;
		}
	}
	/* restore context */
	p->kind = e->kind;
	p->indent = e->indent;
	p->number = (t == T_LI) ? 0 : e->number;
	p->link = e->link;
	p->bold = e->bold;
	p->italic = e->italic;
	p->small = e->small;
	p->sup = e->sup;
	p->sub = e->sub;
	p->cell_mode = e->cell_mode;
	if (t == T_LI && p->kind == BK_ITEM)
		p->kind = BK_PARA;
}

static int lookup_tag(const char *s, uint32_t n)
{
	/* tags by first letter, built once: a few compares instead of a
	 * strcmp against every known tag (this was the hottest function) */
	static uint8_t first[27], next[ZW_COUNT(tags)], len[ZW_COUNT(tags)], ready;
	char name[12];
	uint32_t i;
	int k;
	if (!ready) {
		memset(first, 0xff, sizeof(first));
		for (k = (int)ZW_COUNT(tags) - 1; k >= 0; --k) {
			int c = tags[k].name[0] - 'a';
			len[k] = (uint8_t)strlen(tags[k].name);
			next[k] = first[c];
			first[c] = (uint8_t)k;
		}
		ready = 1;
	}
	if (n == 0 || n >= sizeof(name))
		return T_OTHER;
	for (i = 0; i < n; ++i)
		name[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
	if (name[0] < 'a' || name[0] > 'z')
		return T_OTHER;
	for (k = first[name[0] - 'a']; k != 0xff; k = next[k])
		if (len[k] == n && memcmp(tags[k].name, name, n) == 0)
			return tags[k].tag;
	return T_OTHER;
}

/* a heading with nothing under it before the next heading of the same or
 * a higher level (a Notes section that held only citations, a gallery):
 * leave it out */
static void drop_empty_sections(doc_t *d)
{
	uint32_t i, o = 0;
	for (i = 0; i < d->nblocks; ++i) {
		int k = d->blocks[i].kind;
		if (k == BK_H2 || k == BK_H3 || k == BK_H4) {
			uint32_t j = i + 1;
			/* skip sub-headings that are themselves empty */
			while (j < d->nblocks && d->blocks[j].kind >= BK_H2 && d->blocks[j].kind <= BK_H4 &&
			       d->blocks[j].kind > k)
				j++;
			if (j >= d->nblocks || (d->blocks[j].kind >= BK_H2 && d->blocks[j].kind <= k))
				continue;	/* empty: drop it */
		}
		d->blocks[o++] = d->blocks[i];
	}
	d->nblocks = o;
}

int html_to_doc(doc_t *d, const char *html, uint32_t len,
		char base_ns, const char *base_url, int new_ns)
{
	parser_t *p = zw_calloc(sizeof(parser_t));
	const char *s = html, *end = html + len;
	char *tbuf = NULL;
	uint32_t tcap = 0;
	int r;

	if (!p)
		return ZW_ENOMEM;
	p->d = d;
	p->link = 0xffff;
	p->kind = BK_PARA;
	p->space = 1;
	p->base_ns = base_ns;
	p->base_url = base_url;
	p->new_ns = new_ns;
	p->last_math_end = 0xffffffffu;

	const char *kick = s;
	while (s < end && !p->err) {
		if (s - kick > 16384) {
			ZW_PROGRESS(2, (uint32_t)(s - html), len);
			kick = s;
		}
		if (*s == '<') {
			const char *q = s + 1;
			if (end - s >= 4 && memcmp(s, "<!--", 4) == 0) {
				const char *c = s + 4;
				while (c + 2 < end && !(c[0] == '-' && c[1] == '-' && c[2] == '>'))
					c++;
				/* mwoffliner wraps the licence footer in these */
				if (c - s >= 17 && memcmp(s + 4, "htdig_noindex", 13) == 0) {
					end_block(p);
					p->small_footer = 1;
				} else if (c - s >= 18 && memcmp(s + 4, "/htdig_noindex", 14) == 0) {
					end_block(p);
					p->small_footer = 0;
				}
				s = c + 3 < end ? c + 3 : end;
				continue;
			}
			if (q < end && (*q == '!' || *q == '?')) {
				while (s < end && *s != '>')
					s++;
				s++;
				continue;
			}
			{
				int closing = q < end && *q == '/';
				const char *name, *gt;
				uint32_t nlen;
				int t, selfc = 0;
				if (closing)
					q++;
				name = q;
				while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
						   (*q >= '0' && *q <= '9')))
					q++;
				nlen = (uint32_t)(q - name);
				if (!nlen) {
					text(p, "<", 1);	/* a literal '<' */
					s++;
					continue;
				}
				gt = q;
				{
					char quote = 0;
					while (gt < end && (quote || *gt != '>')) {
						if (quote) {
							if (*gt == quote)
								quote = 0;
						} else if (*gt == '"' || *gt == '\'') {
							quote = *gt;
						}
						gt++;
					}
				}
				if (gt > q && gt[-1] == '/')
					selfc = 1;
				t = lookup_tag(name, nlen);
				if (closing) {
					if (t != T_OTHER)
						close_tag(p, t);
				} else {
					attrs_t a;
					if (t != T_OTHER) {	/* unknown tags' attributes are never used */
						parse_attrs(q, gt, &a);
						p->tag_end = gt < end ? gt + 1 : end;
						p->html_end = end;
						open_tag(p, t, &a, selfc);
						/* raw text elements, and MathML (its TeX came from
						 * alttext): jump straight to their end tag */
						if ((t == T_SCRIPT || t == T_STYLE || t == T_MATH) && !selfc) {
							const char *c = gt;
							const char *tn = t == T_SCRIPT ? "</script" :
									 t == T_STYLE ? "</style" : "</math";
							size_t tl = strlen(tn);
							while (c + tl <= end) {
								const char *lt = memchr(c, '<', (size_t)(end - c));
								if (!lt || lt + tl > end) {
									c = end;
									break;
								}
								if (strncasecmp(lt, tn, tl) == 0) {
									c = lt;
									break;
								}
								c = lt + 1;
							}
							gt = c;
							while (gt < end && *gt != '>')
								gt++;
							close_tag(p, t);
						}
					}
				}
				s = gt < end ? gt + 1 : end;
			}
			continue;
		}
		{
			const char *t0 = s;
			uint32_t n;
			while (s < end && *s != '<')
				s++;
			n = (uint32_t)(s - t0);
			if ((p->depth && p->st[p->depth - 1].skip) || p->skip_level)
				continue;
			if (memchr(t0, '&', n)) {
				if (n > tcap) {
					zw_free(tbuf);
					tcap = n + 64;
					tbuf = zw_alloc(tcap);
					if (!tbuf) {
						p->err = ZW_ENOMEM;
						break;
					}
				}
				n = decode_entities(t0, n, tbuf);
				text(p, tbuf, n);
			} else {
				text(p, t0, n);
			}
		}
	}
	end_block(p);
	zw_free(tbuf);
	drop_empty_sections(d);
	r = p->err;
	zw_free(p);
	return r;
}
