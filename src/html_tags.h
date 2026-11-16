/*
 * zimwiki - the HTML tags the article converter knows (html.c), shared
 * with zimprep's slimming (tools/slim.c), which drops every other tag:
 * the converter ignores them
 */
#ifndef HTML_TAGS_H
#define HTML_TAGS_H
#include <stdint.h>

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

static const struct html_tag_name {
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

#endif
