/*
 * zimprep - slimming an article's HTML for the WikiReader (see slim.h)
 *
 * Every rule here mirrors src/html.c: tags are found the same way, the
 * same attributes are read (by the same names, the last one winning),
 * class words are kept when html.c could match them, and an element is
 * emptied only when html.c would skip it whatever the options (Footnotes,
 * Citations, Pictures).  An emptied element keeps its start and end tag,
 * so opening and closing it still has the same effect on the blocks.
 */
#include <string.h>
#include <strings.h>
#include "slim.h"
#include "../src/html_tags.h"

#define ZW_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* class words html.c compares whole (skip list, picture boxes and
 * captions, link classes) */
static const char *const exact_classes[] = {
	"navbox", "vertical-navbox", "navbox-inner", "sidebar", "nomobile",
	"noprint", "mw-editsection", "reference", "references", "refbegin", "metadata",
	"ambox", "mbox-small", "thumb", "thumbinner", "gallery", "tmulti", "toc", "portal",
	"portalbox", "sistersitebox", "catlinks", "printfooter", "mw-jump-link",
	"authority-control", "navigation-not-searchable", "mw-empty-elt",
	"shortdescription", "mw-cite-backlink", "infobox-image", "mw-ref",
	"mw-kartographer-maplink", "locmap", "mwe-math-fallback-image-inline",
	"mwe-math-fallback-image-display", "noexcerpt", "box-Multiple_issues",
	"side-box", "mw-indicators", "magnify", "infobox-below", "navbar",
	"mw-heading-anchor", "pcs-collapse-table-collapsed-container",
	"pcs-edit-section-link-container",
	/* pictures and captions */
	"gallerybox", "tsingle", "thumbcaption", "infobox-caption", "gallerytext",
	/* links */
	"external", "new", NULL
};
/* ... and the skip list on its own (the first of these in the class
 * attribute decides), without the picture boxes */
static const char *const skip_classes[] = {
	"navbox", "vertical-navbox", "navbox-inner", "sidebar", "nomobile",
	"noprint", "mw-editsection", "reference", "references", "refbegin", "metadata",
	"ambox", "mbox-small", "thumb", "thumbinner", "gallery", "tmulti", "toc", "portal",
	"portalbox", "sistersitebox", "catlinks", "printfooter", "mw-jump-link",
	"authority-control", "navigation-not-searchable", "mw-empty-elt",
	"shortdescription", "mw-cite-backlink", "infobox-image", "mw-ref",
	"mw-kartographer-maplink", "locmap", "mwe-math-fallback-image-inline",
	"mwe-math-fallback-image-display", "noexcerpt", "box-Multiple_issues",
	"side-box", "mw-indicators", "magnify", "infobox-below", "navbar",
	"mw-heading-anchor", "pcs-collapse-table-collapsed-container",
	"pcs-edit-section-link-container", NULL
};
static const char *const pic_boxes[] = { "thumb", "thumbinner", "gallery", "tmulti",
					 "infobox-image", NULL };
/* parts of class words html.c looks for anywhere in the attribute */
static const char *const sub_classes[] = { "mwe-math", "reference", "mw-cite-backlink",
					   "image-display", NULL };

static int lookup(const char *s, size_t n)
{
	char name[12];
	size_t i, k;
	if (n == 0 || n >= sizeof(name))
		return T_OTHER;
	for (i = 0; i < n; ++i)
		name[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
	for (k = 0; k < ZW_COUNT(tags); ++k)
		if (strlen(tags[k].name) == n && !memcmp(tags[k].name, name, n))
			return tags[k].tag;
	return T_OTHER;
}

static int in_list(const char *w, size_t n, const char *const *list)
{
	int i;
	for (i = 0; list[i]; ++i)
		if (strlen(list[i]) == n && !memcmp(list[i], w, n))
			return 1;
	return 0;
}

static int contains(const char *s, size_t n, const char *needle)
{
	size_t k = strlen(needle), i;
	for (i = 0; i + k <= n; ++i)
		if (!memcmp(s + i, needle, k))
			return 1;
	return 0;
}

/* tags whose end comes only from their end tag (no implied ends), so an
 * emptied one can be skipped by counting */
static int countable(int t)
{
	switch (t) {
	case T_DIV: case T_SPAN: case T_TABLE: case T_UL: case T_OL: case T_DL:
	case T_SECTION: case T_ASIDE: case T_NAV: case T_CENTER: case T_DETAILS:
	case T_HEADER: case T_FOOTER: case T_MAIN: case T_ARTICLE: case T_BLOCKQUOTE:
	case T_SMALL: case T_SUP: case T_SUB:
	case T_NOSCRIPT: case T_SVG: case T_TEMPLATE: case T_BUTTON: case T_AUDIO:
	case T_VIDEO: case T_OBJECT: case T_IFRAME: case T_MAP:
		return 1;
	}
	return 0;
}

/* never shown, and no pictures inside (html.c tag_hidden, without the
 * picture containers) */
static int hidden_whole(int t)
{
	switch (t) {
	case T_NOSCRIPT: case T_SVG: case T_TEMPLATE: case T_BUTTON: case T_AUDIO:
	case T_VIDEO: case T_OBJECT: case T_IFRAME: case T_MAP:
		return 1;
	}
	return 0;
}

typedef struct {
	const char *name; size_t nlen;
	const char *val; size_t vlen;
	char quote;		/* '"', '\'' or 0 (unquoted) */
	int has_val;
} attr_t;

#define MAX_ATTRS 64

/* html.c parse_attrs, keeping each attribute */
static int split_attrs(const char *s, const char *end, attr_t *at)
{
	int n = 0;
	while (s < end) {
		attr_t a;
		memset(&a, 0, sizeof(a));
		while (s < end && (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r' || *s == '/'))
			s++;
		a.name = s;
		while (s < end && *s != '=' && *s != ' ' && *s != '>' && *s != '\n' && *s != '\t' && *s != '/')
			s++;
		a.nlen = (size_t)(s - a.name);
		if (!a.nlen) {
			s++;
			continue;
		}
		while (s < end && (*s == ' ' || *s == '\n'))
			s++;
		if (s < end && *s == '=') {
			s++;
			while (s < end && (*s == ' ' || *s == '\n'))
				s++;
			a.has_val = 1;
			if (s < end && (*s == '"' || *s == '\'')) {
				a.quote = *s++;
				a.val = s;
				while (s < end && *s != a.quote)
					s++;
				a.vlen = (size_t)(s - a.val);
				if (s < end)
					s++;
			} else {
				a.val = s;
				while (s < end && *s != ' ' && *s != '>' && *s != '\n')
					s++;
				a.vlen = (size_t)(s - a.val);
			}
		}
		if (n < MAX_ATTRS)
			at[n++] = a;
	}
	return n;
}

static int name_is(const attr_t *a, const char *nm)
{
	return a->nlen == strlen(nm) && !strncasecmp(a->name, nm, a->nlen);
}

/* the last value html.c would see for this name (NULL: none) */
static const attr_t *last_attr(const attr_t *at, int n, const char *nm)
{
	const attr_t *r = NULL;
	int i;
	for (i = 0; i < n; ++i)
		if (name_is(&at[i], nm))
			r = &at[i];
	return r && r->has_val ? r : NULL;
}

static char *put(char *o, const char *s, size_t n)
{
	memcpy(o, s, n);
	return o + n;
}

static char *put_s(char *o, const char *s)
{
	return put(o, s, strlen(s));
}

static char *put_attr(char *o, const char *name, const char *v, size_t n, char quote)
{
	*o++ = ' ';
	o = put_s(o, name);
	*o++ = '=';
	if (!quote)
		quote = memchr(v, '"', n) ? '\'' : '"';
	*o++ = quote;
	o = put(o, v, n);
	*o++ = quote;
	return o;
}

static int style_none(const attr_t *st)
{
	return st && (contains(st->val, st->vlen, "display:none") ||
		      contains(st->val, st->vlen, "display: none"));
}

/* the class words html.c could match, in order; returns the new length */
static size_t filter_classes(const char *c, size_t n, char *out)
{
	const char *end = c + n;
	size_t o = 0;
	while (c < end) {
		const char *w;
		size_t wl;
		int i, keep;
		while (c < end && *c == ' ')
			c++;
		w = c;
		while (c < end && *c != ' ')
			c++;
		wl = (size_t)(c - w);
		if (!wl)
			continue;
		keep = in_list(w, wl, exact_classes);
		for (i = 0; !keep && sub_classes[i]; ++i)
			keep = contains(w, wl, sub_classes[i]);
		if (keep) {
			if (o)
				out[o++] = ' ';
			memcpy(out + o, w, wl);
			o += wl;
		}
	}
	return o;
}

/* would html.c skip this element whatever the options?  1: by style,
 * 2: by a class word (*word, *wlen) */
static int always_skipped(const attr_t *at, int n, const char **word, size_t *wlen)
{
	const attr_t *cls = last_attr(at, n, "class");
	const attr_t *st = last_attr(at, n, "style");
	const attr_t *grp = last_attr(at, n, "data-mw-group");
	const char *c, *end;
	if (cls && contains(cls->val, cls->vlen, "mwe-math-mathml"))
		return 0;
	if (style_none(st))
		return 1;
	if (grp && grp->vlen)
		return 0;
	if (!cls)
		return 0;
	if (contains(cls->val, cls->vlen, "reference") || contains(cls->val, cls->vlen, "mw-cite-backlink"))
		return 0;	/* depends on Footnotes / Citations */
	c = cls->val;
	end = c + cls->vlen;
	while (c < end) {
		const char *w;
		size_t wl;
		while (c < end && *c == ' ')
			c++;
		w = c;
		while (c < end && *c != ' ')
			c++;
		wl = (size_t)(c - w);
		if (wl && in_list(w, wl, skip_classes)) {
			if (in_list(w, wl, pic_boxes))
				return 0;	/* shown for its pictures */
			*word = w;
			*wlen = wl;
			return 2;
		}
	}
	return 0;
}

/* html.c: from just after '<' at s, the tag's '>' (quotes respected) */
static const char *tag_gt(const char *q, const char *end)
{
	char quote = 0;
	while (q < end && (quote || *q != '>')) {
		if (quote) {
			if (*q == quote)
				quote = 0;
		} else if (*q == '"' || *q == '\'') {
			quote = *q;
		}
		q++;
	}
	return q;
}

/* the end of the element of tag t that opened just before s: after its
 * end tag (counting nested ones), as html.c finds tags */
static const char *element_end(const char *s, const char *end, int t)
{
	int depth = 1;
	while (s < end) {
		const char *lt = memchr(s, '<', (size_t)(end - s)), *q, *name, *gt;
		int closing, tt;
		if (!lt)
			return end;
		if (end - lt >= 4 && !memcmp(lt, "<!--", 4)) {
			const char *c = lt + 4;
			while (c + 2 < end && !(c[0] == '-' && c[1] == '-' && c[2] == '>'))
				c++;
			s = c + 3 < end ? c + 3 : end;
			continue;
		}
		q = lt + 1;
		closing = q < end && *q == '/';
		if (closing)
			q++;
		name = q;
		while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
				   (*q >= '0' && *q <= '9')))
			q++;
		if (q == name) {
			s = lt + 1;
			continue;
		}
		gt = tag_gt(q, end);
		tt = lookup(name, (size_t)(q - name));
		if (tt == t) {
			if (closing) {
				if (--depth == 0)
					return gt < end ? gt + 1 : end;
			} else if (!(gt > q && gt[-1] == '/')) {
				depth++;
			}
		}
		/* raw text: html.c jumps to its end tag */
		if (!closing && (tt == T_SCRIPT || tt == T_STYLE || tt == T_MATH) &&
		    !(gt > q && gt[-1] == '/')) {
			const char *tn = tt == T_SCRIPT ? "</script" : tt == T_STYLE ? "</style" : "</math";
			size_t tl = strlen(tn);
			const char *c = gt;
			for (;;) {
				const char *l2 = c < end ? memchr(c, '<', (size_t)(end - c)) : NULL;
				if (!l2 || l2 + tl > end) {
					c = end;
					break;
				}
				if (!strncasecmp(l2, tn, tl)) {
					c = l2;
					break;
				}
				c = l2 + 1;
			}
			gt = c;
			while (gt < end && *gt != '>')
				gt++;
		}
		s = gt < end ? gt + 1 : end;
	}
	return end;
}

size_t html_slim(const char *in, size_t len, char *out)
{
	const char *s = in, *end = in + len;
	char *o = out;
	int pre = 0;
	while (s < end) {
		if (*s != '<') {
			/* text: outside <pre> any run of white space is one space
			 * to html.c */
			const char *t0 = s;
			while (s < end && *s != '<')
				s++;
			if (pre) {
				o = put(o, t0, (size_t)(s - t0));
			} else {
				const char *c;
				for (c = t0; c < s; ) {
					if (*c == ' ' || *c == '\n' || *c == '\t' || *c == '\r') {
						int nl = 0;
						while (c < s && (*c == ' ' || *c == '\n' || *c == '\t' || *c == '\r'))
							nl |= *c++ == '\n';
						*o++ = nl ? '\n' : ' ';
					} else {
						*o++ = *c++;
					}
				}
			}
			continue;
		}
		if (end - s >= 4 && !memcmp(s, "<!--", 4)) {
			const char *c = s + 4;
			while (c + 2 < end && !(c[0] == '-' && c[1] == '-' && c[2] == '>'))
				c++;
			/* the licence footer markers matter to html.c */
			if (c - s >= 17 && !memcmp(s + 4, "htdig_noindex", 13))
				o = put_s(o, "<!--htdig_noindex-->");
			else if (c - s >= 18 && !memcmp(s + 4, "/htdig_noindex", 14))
				o = put_s(o, "<!--/htdig_noindex-->");
			s = c + 3 < end ? c + 3 : end;
			continue;
		}
		{
			const char *q = s + 1, *name, *gt;
			int closing, selfc, t;
			size_t nlen;
			if (q < end && (*q == '!' || *q == '?')) {
				while (s < end && *s != '>')
					s++;
				s++;
				continue;
			}
			closing = q < end && *q == '/';
			if (closing)
				q++;
			name = q;
			while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
					   (*q >= '0' && *q <= '9')))
				q++;
			nlen = (size_t)(q - name);
			if (!nlen) {
				*o++ = '<';	/* a literal '<' */
				s++;
				continue;
			}
			gt = tag_gt(q, end);
			selfc = gt > q && gt[-1] == '/';
			t = lookup(name, nlen);
			s = gt < end ? gt + 1 : end;
			if (t == T_OTHER)
				continue;	/* html.c ignores it */
			if (closing) {
				o = put_s(o, "</");
				{
					size_t k;
					for (k = 0; k < ZW_COUNT(tags); ++k)
						if (tags[k].tag == t) {
							o = put_s(o, tags[k].name);
							break;
						}
				}
				*o++ = '>';
				if (t == T_PRE && pre)
					pre--;
				continue;
			}
			{
				attr_t at[MAX_ATTRS];
				int na = split_attrs(q, gt, at), i, sk;
				const char *word = NULL, *tname = NULL;
				size_t wlen = 0, k;
				for (k = 0; k < ZW_COUNT(tags); ++k)
					if (tags[k].tag == t) {
						tname = tags[k].name;
						break;
					}
				sk = selfc || !countable(t) ? 0 :
				     hidden_whole(t) ? 3 : always_skipped(at, na, &word, &wlen);
				*o++ = '<';
				o = put_s(o, tname);
				if (sk) {
					/* emptied: just what makes html.c skip it, and the
					 * footnote anchor it may add */
					const attr_t *id = last_attr(at, na, "id");
					if (sk == 1)
						o = put_s(o, " style=\"display:none\"");
					else if (sk == 2)
						o = put_attr(o, "class", word, wlen, 0);
					if (id && id->vlen > 5 && !memcmp(id->val, "cite_", 5))
						o = put_attr(o, "id", id->val, id->vlen, id->quote);
					*o++ = '>';
					/* {{Y}} / {{N}}: html.c reads the letter just after a
					 * hidden span's tag */
					if (t == T_SPAN && end - s >= 2 && (s[0] == 'Y' || s[0] == 'N') && s[1] == '<')
						*o++ = s[0];
					s = element_end(s, end, t);
					o = put_s(o, "</");
					o = put_s(o, tname);
					*o++ = '>';
					continue;
				}
				for (i = 0; i < na; ++i) {
					const attr_t *a = &at[i];
					if (!a->has_val)
						continue;
					if (name_is(a, "class")) {
						char buf[4096];
						size_t n = a->vlen < sizeof(buf) ? filter_classes(a->val, a->vlen, buf) : 0;
						if (a->vlen >= sizeof(buf))
							o = put_attr(o, "class", a->val, a->vlen, a->quote);
						else if (n)
							o = put_attr(o, "class", buf, n, 0);
					} else if (name_is(a, "style")) {
						if (style_none(a))
							o = put_s(o, " style=\"display:none\"");
					} else if (name_is(a, "id")) {
						if (a->vlen > 5 && !memcmp(a->val, "cite_", 5))
							o = put_attr(o, "id", a->val, a->vlen, a->quote);
					} else if (name_is(a, "typeof")) {
						if (t == T_SPAN && contains(a->val, a->vlen, "mw:File"))
							o = put_s(o, " typeof=\"mw:File\"");
					} else if ((t == T_A && name_is(a, "href")) ||
						   ((t == T_MATH || t == T_IMG) &&
						    (name_is(a, "alttext") || name_is(a, "alt") || name_is(a, "display"))) ||
						   (t == T_META && name_is(a, "content")) ||
						   (t == T_IMG && (name_is(a, "src") || name_is(a, "data-src"))) ||
						   name_is(a, "data-mw-group")) {
						char nm[16];
						size_t j;
						for (j = 0; j < a->nlen && j < sizeof(nm) - 1; ++j)
							nm[j] = (char)(a->name[j] >= 'A' && a->name[j] <= 'Z' ?
								       a->name[j] + 32 : a->name[j]);
						nm[j] = 0;
						o = put_attr(o, nm, a->val, a->vlen, a->quote);
					}
				}
				if (selfc)
					o = put_s(o, " /");
				*o++ = '>';
				if ((t == T_SCRIPT || t == T_STYLE || t == T_MATH) && !selfc) {
					/* raw text (MathML: its TeX is in alttext): html.c
					 * jumps to the end tag */
					const char *tn = t == T_SCRIPT ? "</script" : t == T_STYLE ? "</style" : "</math";
					size_t tl = strlen(tn);
					const char *c = s;
					for (;;) {
						const char *l2 = c < end ? memchr(c, '<', (size_t)(end - c)) : NULL;
						if (!l2 || l2 + tl > end) {
							c = end;
							break;
						}
						if (!strncasecmp(l2, tn, tl)) {
							c = l2;
							break;
						}
						c = l2 + 1;
					}
					while (c < end && *c != '>')
						c++;
					s = c < end ? c + 1 : end;
					o = put_s(o, "</");
					o = put_s(o, tname);
					*o++ = '>';
				}
				if (t == T_PRE && !selfc)
					pre++;
			}
		}
	}
	return (size_t)(o - out);
}
