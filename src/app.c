/*
 * zimwiki - the WikiReader application
 *
 * Screens: search (keyboard + live results), article, history, wiki
 * picker and messages.  Hardware buttons: SEARCH, HISTORY, RANDOM.
 */
#include <stdio.h>
#include "app.h"
#include "plat.h"
#include "library.h"
#include "blockdev.h"
#include "doc.h"
#include "layout.h"
#include "font.h"
#include "gfx.h"
#include "utf8.h"
#include "calc.h"

#define ROW_H       19
#define BAR_H       20
#define OPT2_DY     12	/* Text size switch: this far below the other two */
#define KB_ROW_H    28	/* three rows like the original firmware's keyboard */
#define KB_H        (3 * KB_ROW_H)
#define KB_Y        (SCREEN_H - KB_H)
#define TOOL_H      24
#define MAX_ROWS    10
#define HIST_MAX    60
#define BACK_MAX    32
#define SECT_MAX    160
#define QUERY_MAX   120
#define DRAG_START  7
#define HOLD_MS     500	/* HISTORY held this long opens the list,
				   RANDOM held this long opens the calculator */
#define CORNER      36	/* bottom left square that means back */
/* grifo does not debounce the buttons: a light press can arrive as
 * down/up/down/up within a few tens of ms */
#define BOUNCE_MS   80
#define CALC_MAX    60	/* calculator expression length */
#define POWER_OFF_MS (5u * 60u * 1000u)

enum screen { S_MESSAGE, S_SEARCH, S_ARTICLE, S_HISTORY, S_LIBRARY, S_SECTIONS, S_CALC };

typedef struct hist_item {
	uint32_t entry;
	int32_t scroll;
	char title[80];
} hist_item_t;

/* one step of "where you came from" */
enum { BI_ARTICLE, BI_SEARCH, BI_HISTORY };
typedef struct back_item {
	uint8_t kind;
	uint32_t entry;		/* BI_ARTICLE */
	int32_t scroll;
} back_item_t;

static struct app {
	uint8_t fb[SCREEN_H * SCREEN_STRIDE];
	uint8_t shown[SCREEN_H * SCREEN_STRIDE];	/* last frame sent to the LCD */
	uint8_t spin_buf[SCREEN_H * SCREEN_STRIDE];	/* shown + spinner */
	int need_draw;
	int screen;

	fat_vol_t vol;
	int mounted;
	lib_info_t libs[LIB_MAX];
	int nlibs;
	library_t lib;
	int have_lib;

	/* search */
	char query[QUERY_MAX + 1];
	int qlen;
	int kb_visible, kb_layer;
	int opts;		/* HTML_NOTES | HTML_CITES | HTML_LARGE (the switches) */
	int opt_y;		/* baseline of the switches line, 0 = not shown */
	int search_dirty;
	uint32_t res_pos;
	lib_hit_t rows[MAX_ROWS];
	char row_target[MAX_ROWS][80];
	int nrows;
	int pressed_row;
	int pressed_key;	/* index into key table or -1 */
	uint32_t key_down_ms, key_repeat_ms;

	/* article */
	doc_t doc;
	layout_t lay;
	int have_article;
	uint32_t entry;
	char title[256];
	int32_t scroll;
	int32_t vel;		/* momentum, 1/16 px per 20 ms step */
	uint32_t anim_ms;
	int toolbar;
	int show_bar;		/* scroll position indicator */
	back_item_t back[BACK_MAX];
	int nback;
	int hl_x, hl_w, hl_h;	/* tapped link highlight */
	int32_t hl_y;

	/* history */
	hist_item_t hist[HIST_MAX];
	/* section list of the open article */
	struct sect { int32_t y; uint8_t level; char title[72]; } sect[SECT_MAX];
	int nsect, sect_top;
	int nhist, hist_top, hist_dirty;

	/* library list */
	int lib_top;

	/* touch */
	int touching, dragging;
	int tx0, ty0, tx, ty, ty_prev;
	uint32_t t_prev, t_last;
	int32_t scroll0;
	int list_acc;

	/* message screen */
	char msg_title[64];
	char msg[6][64];
	int nmsg;
	int msg_return;		/* screen to go to on tap, -1 = none */

	uint32_t last_input;
	int hist_down;		/* HISTORY pressed, not yet resolved */
	uint32_t hist_down_ms;
	int opening;		/* an article is being opened: show progress */
	uint32_t open_t0, open_drawn;
	int font_loading;	/* WIKIFONT.BIN being read: spinner top right */
	uint32_t spin_t;
	int spin_i;
	int hist_up_pending;	/* released, waiting out contact bounce */
	uint32_t hist_up_ms;
	int rnd_down;		/* RANDOM pressed, not yet resolved */
	uint32_t rnd_down_ms;
	int rnd_up_pending;	/* released, waiting out contact bounce */
	uint32_t rnd_up_ms;

	/* calculator (RANDOM held) */
	char calc_expr[CALC_MAX + 1];	/* ASCII: digits . + - * / % ( ) A */
	int calc_len;
	int calc_done;		/* = pressed: the result is shown */
	int calc_err;		/* last = failed: CALC_ error code */
	double calc_ans;	/* Ans, kept in ZWSTATE.DAT */
	int calc_mode;		/* 0 Simple, 1 Sci (kept) */
	int calc_rad;		/* angles in radians (kept; default degrees) */
	int calc_2nd;		/* 2nd pressed: the next key is its other one */
	int calc_ret;		/* screen to go back to */
	struct {
		uint8_t ignore;	/* this press was a bounce: drop its release */
		uint8_t seen_up;
		uint32_t up_ms;
	} btn[4];
	int state_dirty;
	uint32_t seed;
	int quit;
	plat_event_t pushback;
	int have_pushback;
} A;

/* ------------------------------------------------------------------ */
/* drawing helpers */

static void fill(int x, int y, int w, int h, int c)
{
	gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, x, y, w, h, c);
}

static void hline(int x, int y, int w)
{
	gfx_hline(A.fb, SCREEN_STRIDE, SCREEN_H, x, y, w, BLACK);
}

static int text_at(int x, int base, int font, const char *s, int colour)
{
	return font_draw(A.fb, SCREEN_STRIDE, SCREEN_H, x, base, font, s, strlen(s), colour);
}

/* draw s clipped to maxw pixels, adding an ellipsis when cut */
static int text_clip(int x, int base, int maxw, int font, const char *s, int colour)
{
	const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s), *cut = p;
	int w = 0, ell = font_width(font, "\xe2\x80\xa6", 3);
	if (font_width(font, s, strlen(s)) <= maxw)
		return text_at(x, base, font, s, colour);
	while (p < end) {
		const uint8_t *q = p;
		glyph_t g;
		font_glyph(font, utf8_next(&q, end), &g);
		if (w + g.adv + ell > maxw)
			break;
		w += g.adv;
		p = q;
		cut = p;
	}
	x = font_draw(A.fb, SCREEN_STRIDE, SCREEN_H, x, base, font, s,
		      (size_t)(cut - (const uint8_t *)s), colour);
	return font_draw(A.fb, SCREEN_STRIDE, SCREEN_H, x, base, font, "\xe2\x80\xa6", 3, colour);
}

/* like text_clip, but a long title loses its middle: "The Works of …
 * /Chapter 12".  The end is what tells similar titles apart (subpages,
 * volumes, chapters), so it is kept, preferably from the last '/'. */
static int text_clip_mid(int x, int base, int maxw, int font, const char *s, int colour)
{
	size_t n = strlen(s);
	const char *tail, *slash = strrchr(s, '/');
	int ell = font_width(font, "\xe2\x80\xa6", 3), tw, hw, w;
	const uint8_t *p, *end, *cut;
	if (font_width(font, s, n) <= maxw)
		return text_at(x, base, font, s, colour);
	/* tail: the last two '/' parts if they fit in 60% ("/Volume 2/Chapter 1"),
	 * else the last part, else the last ~45% */
	const char *slash2 = NULL;
	if (slash && slash > s) {
		const char *q = slash - 1;
		while (q > s && *q != '/')
			q--;
		if (*q == '/' && q > s)
			slash2 = q;
	}
	if (slash2 && font_width(font, slash2, n - (size_t)(slash2 - s)) <= maxw * 6 / 10)
		tail = slash2;
	else if (slash && slash > s && font_width(font, slash, n - (size_t)(slash - s)) <= maxw * 6 / 10)
		tail = slash;
	else {
		tail = s + n;
		while (tail > s) {
			const char *q = tail - 1;
			while (q > s && ((uint8_t)*q & 0xc0) == 0x80)
				q--;
			if (font_width(font, q, n - (size_t)(q - s)) > maxw * 45 / 100)
				break;
			tail = q;
		}
	}
	tw = font_width(font, tail, n - (size_t)(tail - s));
	hw = maxw - tw - ell;
	/* head: as much of the start as fits before the ellipsis */
	p = (const uint8_t *)s;
	end = (const uint8_t *)tail;
	cut = p;
	w = 0;
	while (p < end) {
		const uint8_t *q = p;
		glyph_t g;
		font_glyph(font, utf8_next(&q, end), &g);
		if (w + g.adv > hw)
			break;
		w += g.adv;
		p = q;
		cut = p;
	}
	x = font_draw(A.fb, SCREEN_STRIDE, SCREEN_H, x, base, font, s,
		      (size_t)(cut - (const uint8_t *)s), colour);
	x = font_draw(A.fb, SCREEN_STRIDE, SCREEN_H, x, base, font, "\xe2\x80\xa6", 3, colour);
	return font_draw(A.fb, SCREEN_STRIDE, SCREEN_H, x, base, font, tail,
			 n - (size_t)(tail - s), colour);
}

static void text_centre(int cx, int base, int font, const char *s, int colour)
{
	text_at(cx - font_width(font, s, strlen(s)) / 2, base, font, s, colour);
}

static void triangle(int x, int y, int size, int up, int colour)
{
	int i;
	for (i = 0; i < size; ++i) {
		int w = up ? 2 * i + 1 : 2 * (size - i) - 1;
		gfx_hline(A.fb, SCREEN_STRIDE, SCREEN_H, x - w / 2, y + i, w, colour);
	}
}

static void triangle_left(int x, int y, int size, int colour)
{
	int i;
	for (i = 0; i < size; ++i)
		gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, x + i, y + size - 1 - i,
			 1, 2 * i + 1, colour);	/* tip on the left */
}

static void present(void)
{
	memcpy(A.shown, A.fb, sizeof(A.shown));
	plat_present(A.fb);
}

/* | / - \ in a small box at the top right, drawn over the last frame
 * shown (the font is loaded in the middle of drawing the next one, so
 * A.fb holds a half drawn screen) */
static void spinner_step(void)
{
	/* | / - \ as drawn lines, all the same length: step (dx, dy) */
	static const int8_t step[4][2] = { { 0, 1 }, { 1, -1 }, { 1, 0 }, { 1, 1 } };
	int x = SCREEN_W - 15, y = 1, w = 14, h = 14, cx = x + 6, cy = y + 6, i;
	uint32_t now = plat_ms();
	if (A.spin_t && now - A.spin_t < 120)
		return;
	A.spin_t = now;
	memcpy(A.spin_buf, A.shown, sizeof(A.spin_buf));
	gfx_fill(A.spin_buf, SCREEN_STRIDE, SCREEN_H, x, y, w, h, WHITE);
	gfx_rect(A.spin_buf, SCREEN_STRIDE, SCREEN_H, x, y, w, h, BLACK);
	for (i = -4; i <= 4; ++i)
		gfx_fill(A.spin_buf, SCREEN_STRIDE, SCREEN_H, cx + i * step[A.spin_i & 3][0],
			 cy + i * step[A.spin_i & 3][1], 2, 2, BLACK);
	A.spin_i++;
	plat_present(A.spin_buf);
}

/* small box in the middle of the screen, drawn over whatever is there */
static void banner(const char *s)
{
	int w = font_width(FONT_BOLD, s, strlen(s)) + 24, x = (SCREEN_W - w) / 2, y = 88;
	if (w > SCREEN_W - 8) {
		w = SCREEN_W - 8;
		x = 4;
	}
	fill(x, y, w, 30, WHITE);
	gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, x, y, w, 30, BLACK);
	gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, x + 1, y + 1, w - 2, 28, BLACK);
	text_clip(x + 12, y + 20, w - 20, FONT_BOLD, s, BLACK);
	present();
}

/* progress while opening an article: decompressing ~45%, reading the
 * HTML ~45%, layout ~10%.  Shown only on slow (big) pages. */
static void open_progress(int stage, uint32_t done, uint32_t total)
{
	/* reading and parsing are most of opening a page: only the first
	 * screens are laid out before it shows (the rest in idle time) */
	static const uint8_t from[4] = { 0, 0, 25, 90 }, span[4] = { 0, 25, 65, 10 };
	uint32_t now, pct;
	int x = 40, y = 134, w = SCREEN_W - 80;
	char t[8];
	plat_keepalive();
	if (stage == 4) {
		if (A.font_loading)
			spinner_step();
		return;
	}
	if (!A.opening || stage < 1 || stage > 3 || !total)
		return;
	now = plat_ms();
	if (now - A.open_t0 < 500 || now - A.open_drawn < 150)
		return;
	A.open_drawn = now;
	if (done > total)
		done = total;
	pct = from[stage] + (uint32_t)((uint64_t)span[stage] * done / total);
	if (A.opening == 2)	/* finishing the layout on its own */
		pct = (uint32_t)((uint64_t)100 * done / total);
	fill(x - 2, y - 2, w + 4, 38, WHITE);
	gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, x, y, w, 8, BLACK);
	fill(x + 1, y + 1, (int)((w - 2) * pct / 100), 6, BLACK);
	t[0] = 0;
	if (pct >= 10)
		t[0] = (char)('0' + pct / 10), t[1] = (char)('0' + pct % 10), t[2] = '%', t[3] = 0;
	else
		t[0] = (char)('0' + pct), t[1] = '%', t[2] = 0;
	text_centre(SCREEN_W / 2, y + 20, FONT_SMALL, t, BLACK);
	text_centre(SCREEN_W / 2, y + 34, FONT_SMALL,
		    A.opening == 2 || stage == 3 ? "laying out the page" :
		    stage == 2 ? "reading the text" : "unpacking from the card", BLACK);
	present();
}

/* "812 bytes", "45 KB", "1.3 MB" (no snprintf on the device) */
static void size_text(char *o, uint32_t n)
{
	char d[12];
	int k = 0, i = 0;
	uint32_t v = n < 1024 ? n : n < 1024u * 1024u ? (n + 512) / 1024 : n / (1024u * 1024u);
	do {
		d[k++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (k)
		o[i++] = d[--k];
	if (n >= 1024u * 1024u) {
		o[i++] = '.';
		o[i++] = (char)('0' + (n % (1024u * 1024u)) * 10 / (1024u * 1024u));
	}
	memcpy(o + i, n < 1024 ? " bytes" : n < 1024u * 1024u ? " KB" : " MB",
	       n < 1024 ? 7 : 4);
}

/* two line banner: "Opening…" and the article's title under it */
static void banner2(const char *s, const char *sub)
{
	int w = font_width(FONT_BOLD, s, strlen(s)), w2 = font_width(FONT_TEXT, sub, strlen(sub));
	int x, y = 80, h = 48;
	if (w2 > w)
		w = w2;
	w += 24;
	if (w > SCREEN_W - 8)
		w = SCREEN_W - 8;
	x = (SCREEN_W - w) / 2;
	fill(x, y, w, h, WHITE);
	gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, x, y, w, h, BLACK);
	gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, x + 1, y + 1, w - 2, h - 2, BLACK);
	text_clip(x + 12, y + 20, w - 20, FONT_BOLD, s, BLACK);
	text_clip(x + 12, y + 39, w - 20, FONT_TEXT, sub, BLACK);
	present();
}

static void set_message(const char *title, const char *const *lines, int ret)
{
	int i;
	memset(A.msg_title, 0, sizeof(A.msg_title));
	strncpy(A.msg_title, title, sizeof(A.msg_title) - 1);
	A.nmsg = 0;
	for (i = 0; lines[i] && i < 6; ++i) {
		memset(A.msg[i], 0, sizeof(A.msg[i]));
		strncpy(A.msg[i], lines[i], sizeof(A.msg[i]) - 1);
		A.nmsg++;
	}
	A.msg_return = ret;
	A.screen = S_MESSAGE;
	A.need_draw = 1;
}

static void error_message(const char *what, int code)
{
	static char line[64];
	const char *lines[4];
	const char *why = code == ZW_EIO ? "The card could not be read." :
			  code == ZW_ENOMEM ? "Not enough memory." :
			  code == ZW_EFORMAT ? "The data looks damaged." :
			  code == ZW_ENOTFOUND ? "It is not in this wiki." :
			  code == ZW_EUNSUPPORTED ? "This kind of file is not supported." :
			  "Something went wrong.";
	int i = 0;
	strncpy(line, what, sizeof(line) - 1);
	lines[i++] = line;
	lines[i++] = why;
	lines[i++] = "";
	lines[i++] = NULL;
	set_message("Sorry", lines, A.have_article ? S_ARTICLE : S_SEARCH);
}

/* ------------------------------------------------------------------ */
/* state files */

#define STATE_MAGIC 0x54535a57u	/* "WZST" */
#define HIST_MAGIC  0x48535a57u	/* "WZSH" */

static void hist_name(char *out)
{
	lib_path(&A.lib.info, ".HIS", out);
}

static void save_history(void)
{
	uint8_t *buf, *p;
	char name[32];
	int i;
	if (!A.have_lib || !A.hist_dirty)
		return;
	buf = zw_alloc(8 + (size_t)A.nhist * (9 + 80));
	if (!buf)
		return;
	zw_wr32(buf, HIST_MAGIC);
	zw_wr32(buf + 4, (uint32_t)A.nhist);
	p = buf + 8;
	for (i = 0; i < A.nhist; ++i) {
		uint8_t n = (uint8_t)strlen(A.hist[i].title);
		zw_wr32(p, A.hist[i].entry);
		zw_wr32(p + 4, (uint32_t)A.hist[i].scroll);
		p[8] = n;
		memcpy(p + 9, A.hist[i].title, n);
		p += 9 + n;
	}
	hist_name(name);
	/* clear the flag even when saving failed: retrying at once would
	 * keep the unit busy (no sleep, no auto power off) for nothing */
	if (plat_save(name, buf, (uint32_t)(p - buf)) != 0)
		zw_log("history not saved");
	A.hist_dirty = 0;
	zw_free(buf);
	bd_invalidate();
}

static void load_history(void)
{
	uint8_t *buf, *p, *end;
	uint32_t len, n, i;
	char name[32];
	A.nhist = 0;
	hist_name(name);
	if (plat_load(name, &buf, &len) < 0)
		return;
	if (len >= 8 && zw_rd32(buf) == HIST_MAGIC) {
		n = zw_rd32(buf + 4);
		p = buf + 8;
		end = buf + len;
		for (i = 0; i < n && i < HIST_MAX && p + 9 <= end; ++i) {
			uint8_t tl = p[8];
			if (p + 9 + tl > end || tl >= sizeof(A.hist[0].title))
				break;
			A.hist[i].entry = zw_rd32(p);
			A.hist[i].scroll = (int32_t)zw_rd32(p + 4);
			memcpy(A.hist[i].title, p + 9, tl);
			A.hist[i].title[tl] = 0;
			p += 9 + tl;
			A.nhist++;
		}
	}
	zw_free(buf);
}

static void save_state(void)
{
	uint8_t buf[256];
	size_t ql = (size_t)A.qlen;
	if (!A.state_dirty || !A.have_lib)
		return;
	memset(buf, 0, sizeof(buf));
	zw_wr32(buf, STATE_MAGIC);
	memcpy(buf + 4, A.lib.info.key, 9);
	{
		/* in the calculator: the screen under it (it reopens there,
		 * so RANDOM still goes back to the article after power off) */
		int scr = A.screen == S_CALC ? A.calc_ret : A.screen;
		buf[13] = (uint8_t)(scr == S_ARTICLE ? S_ARTICLE : S_SEARCH);
	}
	buf[14] = (uint8_t)(A.opts | 0x80);	/* switches; 0x80: set */
	zw_wr32(buf + 16, A.have_article ? A.entry : 0xffffffffu);
	zw_wr32(buf + 20, (uint32_t)A.scroll);
	buf[24] = (uint8_t)ql;
	memcpy(buf + 25, A.query, ql);
	{
		/* calculator Ans at 200: "CALC" and the double (copied as
		 * bytes: no unaligned access) */
		memcpy(buf + 200, "CALC", 4);
		memcpy(buf + 204, &A.calc_ans, sizeof(double));
		buf[212] = (uint8_t)A.calc_mode;
		buf[213] = (uint8_t)A.calc_rad;
	}
	if (plat_save("ZWSTATE.DAT", buf, 214) != 0)
		zw_log("state not saved");
	A.state_dirty = 0;	/* see save_history */
	bd_invalidate();
}

static void save_all(void)
{
	save_state();
	save_history();
	if (A.have_lib)
		lib_save_extents(&A.lib);
}

/* ------------------------------------------------------------------ */
/* library */

static void prep_progress(void *arg, uint32_t done, uint32_t total)
{
	int w = total ? (int)((uint64_t)done * 200 / total) : 0;
	(void)arg;
	fill(20, 130, 200, 8, WHITE);
	gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, 20, 130, 200, 8, BLACK);
	fill(20, 130, w, 8, BLACK);
	present();
}

static int open_library(int i)
{
	int r;
	uint32_t c;
	if (A.have_lib) {
		save_all();
		lib_close(&A.lib);
		A.have_lib = 0;
	}
	if (A.have_article) {
		/* it belongs to the old wiki (its links would resolve wrongly) */
		lay_free(&A.lay);
		doc_free(&A.doc);
		A.have_article = 0;
	}
	memset(A.fb, 0, sizeof(A.fb));
	text_centre(SCREEN_W / 2, 90, FONT_BOLD, A.libs[i].title, BLACK);
	text_centre(SCREEN_W / 2, 112, FONT_TEXT, "Opening\xe2\x80\xa6", BLACK);
	present();
	if ((r = lib_open(&A.lib, &A.vol, &A.libs[i])) < 0) {
		static char t[64];
		const char *lines[5];
		lib_close(&A.lib);
		memcpy(t, A.libs[i].key, 9);
		lines[0] = t;
		lines[1] = r == ZW_ENOTFOUND ? "Some of its files are missing." :
			   r == ZW_EFORMAT ? "Its index does not match the ZIM," : "It could not be opened.";
		lines[2] = r == ZW_EFORMAT ? "run zimprep again." : "";
		lines[3] = NULL;
		set_message("Cannot open wiki", lines, A.nlibs > 1 ? S_LIBRARY : -1);
		return r;
	}
	A.have_lib = 1;
	/* first use of a card: map every chunk now, with a progress bar,
	 * so later reads never stall on it */
	for (c = 0; c < A.lib.nchunks; ++c) {
		if (A.lib.chunk_ready[c])
			continue;
		{
			/* "Preparing" / wiki title / "file 3 of 26 (first time only)" */
			char line[48];
			uint32_t k = 0, v;
			char num[12];
			int nn;
			memset(A.fb, 0, sizeof(A.fb));
			text_centre(SCREEN_W / 2, 78, FONT_BOLD, "Preparing", BLACK);
			{
				int tw = font_width(FONT_TEXT, A.libs[i].title, strlen(A.libs[i].title));
				int x = tw > SCREEN_W - 16 ? 8 : (SCREEN_W - tw) / 2;
				text_clip(x, 100, SCREEN_W - 16, FONT_TEXT, A.libs[i].title, BLACK);
			}
			if (A.lib.nchunks > 1) {
				memcpy(line, "file ", 5);
				k = 5;
				for (v = c + 1, nn = 0; v || !nn; v /= 10)
					num[nn++] = (char)('0' + v % 10);
				while (nn)
					line[k++] = num[--nn];
				memcpy(line + k, " of ", 4);
				k += 4;
				for (v = A.lib.nchunks, nn = 0; v || !nn; v /= 10)
					num[nn++] = (char)('0' + v % 10);
				while (nn)
					line[k++] = num[--nn];
				memcpy(line + k, ", first time only", 18);
			} else
				memcpy(line, "first time only", 16);
			text_centre(SCREEN_W / 2, 118, FONT_SMALL, line, BLACK);
			prep_progress(NULL, c, A.lib.nchunks);
		}
		if ((r = fat_build_extents(&A.vol, &A.lib.chunk[c], NULL, NULL)) < 0) {
			error_message("Reading the wiki files failed.", r);
			return r;
		}
		A.lib.chunk_ready[c] = 1;
		A.lib.ext_dirty = 1;
	}
	lib_save_extents(&A.lib);
	load_history();
	A.nback = 0;
	A.query[0] = 0;
	A.qlen = 0;
	A.res_pos = 0;
	A.nrows = 0;
	A.search_dirty = 1;
	return ZW_OK;
}

/* ------------------------------------------------------------------ */
/* history and back stack */

static void hist_add(uint32_t entry, const char *title)
{
	int i, k = -1;
	for (i = 0; i < A.nhist; ++i)
		if (A.hist[i].entry == entry)
			k = i;
	if (k < 0)
		k = A.nhist < HIST_MAX ? A.nhist++ : HIST_MAX - 1;
	for (i = k; i > 0; --i)
		A.hist[i] = A.hist[i - 1];
	A.hist[0].entry = entry;
	A.hist[0].scroll = 0;
	memset(A.hist[0].title, 0, sizeof(A.hist[0].title));
	{
		size_t n = strlen(title);
		if (n >= sizeof(A.hist[0].title))
			n = sizeof(A.hist[0].title) - 1;
		while (n && ((unsigned char)title[n] & 0xc0) == 0x80)
			n--;	/* do not cut a character in half */
		memcpy(A.hist[0].title, title, n);
	}
	A.hist_dirty = 1;
}

static void hist_update_scroll(void)
{
	if (A.have_article && A.nhist && A.hist[0].entry == A.entry)
		A.hist[0].scroll = A.scroll;
}

static int32_t max_scroll(void)
{
	int32_t m;
	/* scrolling towards the part not laid out yet: lay out some more */
	if (A.have_article && !A.lay.done && A.lay.height < A.scroll + 4 * SCREEN_H)
		lay_more(&A.lay, A.scroll + 6 * SCREEN_H, 0);
	m = A.lay.height - SCREEN_H;
	return m > 0 ? m : 0;
}

/* ------------------------------------------------------------------ */
/* articles */

static void push_back(int kind, uint32_t entry, int32_t scroll)
{
	if (A.nback == BACK_MAX) {
		memmove(A.back, A.back + 1, sizeof(A.back[0]) * (BACK_MAX - 1));
		A.nback--;
	}
	A.back[A.nback].kind = (uint8_t)kind;
	A.back[A.nback].entry = entry;
	A.back[A.nback].scroll = scroll;
	A.nback++;
}

/* remember the screen being left, so Back can return to it */
static void push_current(void)
{
	switch (A.screen) {
	case S_ARTICLE:
		if (A.have_article) {
			hist_update_scroll();
			push_back(BI_ARTICLE, A.entry, A.scroll);
		}
		break;
	case S_SEARCH:
		push_back(BI_SEARCH, 0, (int32_t)A.res_pos);	/* where the results were scrolled to */
		break;
	case S_HISTORY:
		push_back(BI_HISTORY, 0, A.hist_top);
		break;
	}
}

static void build_sections(void);
static void fetch_rows(void);
static void jump_to_section(const char *name);
static int jump_to_anchor(const char *name);
static int redirect_depth;

/* open an article; push: remember the current screen for Back */
static int open_entry(uint32_t entry, int32_t scroll, int push)
{
	zim_dirent_t d;
	const uint8_t *html;
	uint32_t len, final;
	doc_t nd;
	layout_t nl;
	int r;

	if (!A.have_lib)
		return ZW_ENOTFOUND;
	r = zim_follow(&A.lib.zim, entry, &final, &d);
	if (!(A.screen == S_ARTICLE && A.have_article))
		memset(A.fb, 0, sizeof(A.fb));
	if (r < 0) {
		banner("Opening\xe2\x80\xa6");
		goto fail;
	}
	banner2("Opening\xe2\x80\xa6", d.title);
	A.opening = 1;
	A.open_t0 = plat_ms();
	A.open_drawn = 0;
	if (d.mime != A.lib.zim.html_mime) {
		r = ZW_EUNSUPPORTED;
		goto fail;
	}
	if ((r = zim_blob(&A.lib.zim, d.cluster, d.blob, &html, &len)) < 0)
		goto fail;
	{
		/* now the size is known: "Opening… (1.3 MB)" */
		char head[40];
		static const char op[] = "Opening\xe2\x80\xa6 (";
		memcpy(head, op, sizeof(op) - 1);
		size_text(head + sizeof(op) - 1, len);
		strcat(head, ")");
		banner2(head, d.title);
	}
	doc_init(&nd);
	if ((r = html_to_doc(&nd, (const char *)html, len, d.ns, d.url,
			     A.lib.zim.content_ns == 'C')) < 0) {
		doc_free(&nd);
		goto fail;
	}
	if (nd.refresh_link != 0xffff && redirect_depth < 4) {
		/* a page that only sends the reader on ("Left quasi-reflexivity"
		 * -> "Reflexive relation#Left quasi-reflexivity"): go there */
		char ns, sect[sizeof(nd.refresh_frag)];
		const char *u = doc_link(&nd, nd.refresh_link, &ns);
		uint32_t idx;
		if (u && zim_find(&A.lib.zim, ns, u, &idx) == 0 && idx != final) {
			memcpy(sect, nd.refresh_frag, sizeof(sect));
			doc_free(&nd);
			A.opening = 0;
			redirect_depth++;
			r = open_entry(idx, scroll, push);
			redirect_depth--;
			if (r == ZW_OK && sect[0] && scroll <= 0)
				jump_to_section(sect);
			return r;
		}
	}
	/* lay out just enough to show the page; the rest follows in idle
	 * time (a long book takes many seconds to lay out in full) */
	if ((r = lay_begin(&nl, &nd)) < 0 ||
	    (r = lay_more(&nl, (scroll > 0 ? scroll : 0) + 3 * SCREEN_H, 0)) < 0) {
		lay_free(&nl);
		doc_free(&nd);
		goto fail;
	}
	A.opening = 0;
	/* success: remember where we came from, retire the old article */
	if (push)
		push_current();
	if (A.have_article) {
		hist_update_scroll();
		lay_free(&A.lay);
		doc_free(&A.doc);
	}
	A.doc = nd;
	A.lay = nl;
	A.lay.doc = &A.doc;	/* the doc moved */
	A.have_article = 1;
	A.entry = final;
	memset(A.title, 0, sizeof(A.title));
	strncpy(A.title, d.title, sizeof(A.title) - 1);
	A.scroll = scroll < 0 ? 0 : scroll > max_scroll() ? max_scroll() : scroll;
	A.vel = 0;
	A.toolbar = 0;
	A.hl_w = 0;
	A.screen = S_ARTICLE;
	A.need_draw = 1;
	A.state_dirty = 1;
	hist_add(final, A.title);
	if (scroll > 0)
		A.hist[0].scroll = A.scroll;
	return ZW_OK;
fail:
	A.opening = 0;
	zw_log("open_entry %u failed (%d)", (unsigned)entry, r);
	error_message("This article could not be opened.", r);
	return r;
}

/* Back: return to the previous screen, like a browser */
static void go_back(void)
{
	A.toolbar = 0;
	A.vel = 0;
	A.state_dirty = 1;
	A.need_draw = 1;
	if (A.screen == S_MESSAGE) {
		if (A.msg_return >= 0)
			A.screen = A.msg_return == S_ARTICLE && !A.have_article ? S_SEARCH : A.msg_return;
		return;
	}
	if (A.screen == S_ARTICLE)
		hist_update_scroll();
	if (!A.nback) {
		/* nothing recorded: from an article go to search, from
		 * elsewhere return to the article being read */
		if (A.screen == S_ARTICLE)
			A.screen = S_SEARCH;
		else if (A.have_article)
			A.screen = S_ARTICLE;
		else
			A.screen = S_SEARCH;
		if (A.screen == S_SEARCH && A.qlen)
			A.search_dirty = 1;
		return;
	}
	A.nback--;
	switch (A.back[A.nback].kind) {
	case BI_ARTICLE:
		if (A.have_article && A.back[A.nback].entry == A.entry) {
			/* still loaded (we went to search or history from it) */
			A.scroll = A.back[A.nback].scroll;
			if (A.scroll > max_scroll())
				A.scroll = max_scroll();
			A.screen = S_ARTICLE;
		} else {
			open_entry(A.back[A.nback].entry, A.back[A.nback].scroll, 0);
		}
		break;
	case BI_SEARCH:
		A.screen = S_SEARCH;
		if (A.qlen && A.have_lib) {
			/* back to the results as they were scrolled, not to
			 * the top of the search again */
			A.res_pos = (uint32_t)A.back[A.nback].scroll;
			A.search_dirty = 0;
			fetch_rows();
			if (!A.nrows)
				A.search_dirty = 1;
		}
		break;
	case BI_HISTORY:
		A.screen = S_HISTORY;
		A.hist_top = A.back[A.nback].scroll < A.nhist ? A.back[A.nback].scroll : 0;
		break;
	}
}

static void show_history(void)
{
	if (A.screen == S_HISTORY)
		return;
	push_current();
	A.screen = S_HISTORY;
	A.hist_top = 0;
	A.toolbar = 0;
	A.need_draw = 1;
}

static void open_link(int link)
{
	char ns;
	const char *url = doc_link(&A.doc, (uint32_t)link, &ns);
	char sect[96];
	uint32_t idx;
	int r;
	if (!url)
		return;
	strncpy(sect, doc_link_section(&A.doc, (uint32_t)link), sizeof(sect) - 1);
	sect[sizeof(sect) - 1] = 0;
	if (!ns) {
		/* "#History" within this page: go there (Back returns), or
		 * stay put when there is no such heading */
		int32_t before = A.scroll;
		push_current();
		if (jump_to_anchor(sect) < 0)
			jump_to_section(sect);
		if (A.scroll == before)
			A.nback--;	/* nothing moved: forget the Back step */
		A.need_draw = 1;
		return;
	}
	if ((r = zim_find(&A.lib.zim, ns, url, &idx)) < 0) {
		const char *lines[] = { "That page is not included", "in this wiki.", NULL };
		set_message("Link not available", lines, S_ARTICLE);
		return;
	}
	if (open_entry(idx, 0, 1) == ZW_OK && sect[0])
		jump_to_section(sect);	/* not found: stays at the top */
}

static void random_article(void)
{
	uint32_t e;
	A.seed = A.seed * 1103515245u + 12345u + plat_ms();
	if (!A.have_lib)
		return;
	if (lib_random(&A.lib, A.seed, &e) == 0)
		open_entry(e, 0, 1);
}

/* ------------------------------------------------------------------ */
/* keyboard */

typedef struct key {
	int16_t x, y, w;
	const char *label;
	uint8_t code;		/* char, or one of the K_ codes */
} key_t_;

enum { K_SHIFT = 1, K_HIDE = 2, K_BS = 8, K_GO = 10 };

static key_t_ keys[48];
static int nkeys = -1, keys_layer = -1;

static void add_key(int x, int row, int w, const char *label, int code)
{
	key_t_ *k = &keys[nkeys++];
	k->x = (int16_t)x;
	k->y = (int16_t)(KB_Y + row * KB_ROW_H);
	k->w = (int16_t)w;
	k->label = label;
	k->code = (uint8_t)code;
}

/* the original firmware's layout: three rows of ten 24 px keys, every
 * key's touch area its whole cell, so a slightly-off tap still lands on
 * the nearest key.  No Go key (tap a result) and no hide key (drag the
 * results to hide the keyboard, tap the search bar to bring it back) */
static void build_keys(int layer)
{
	static const char *const abc[3] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCV" };
	static const char *const abc2 = "BNM";
	static const char *const num[3] = { "1234567890", "-.,'&()!?", "/:;\"" };
	static const char *const num2 = "#+=";
	static char labels[3][10][2], labels2[3][2];
	const char *const *rows = layer ? num : abc;
	const char *tail = layer ? num2 : abc2;
	int r, i;
	if (keys_layer == layer)
		return;
	nkeys = 0;
	for (r = 0; r < 3; ++r) {
		int n = (int)strlen(rows[r]);
		for (i = 0; i < n; ++i) {
			char c = rows[r][i];
			labels[r][i][0] = c;
			labels[r][i][1] = 0;
			/* letters type lower case (search ignores case anyway) */
			add_key(i * 24, r, 24, labels[r][i],
				(unsigned char)(c >= 'A' && c <= 'Z' ? c + 32 : c));
		}
	}
	add_key(216, 1, 24, "\xe2\x86\x90", K_BS);	/* left arrow, end of row 2 */
	add_key(96, 2, 48, "", ' ');			/* space */
	for (i = 0; i < 3; ++i) {
		labels2[i][0] = tail[i];
		labels2[i][1] = 0;
		add_key(144 + i * 24, 2, 24, labels2[i],
			(unsigned char)(tail[i] >= 'A' && tail[i] <= 'Z' ? tail[i] + 32 : tail[i]));
	}
	add_key(216, 2, 24, layer ? "abc" : "123", K_SHIFT);
	keys_layer = layer;
}

static int key_at(int x, int y)
{
	int i;
	build_keys(A.kb_layer);
	for (i = 0; i < nkeys; ++i)
		if (x >= keys[i].x && x < keys[i].x + keys[i].w &&
		    y >= keys[i].y && y < keys[i].y + KB_ROW_H)
			return i;
	return -1;
}

/* one character at double size (each pixel drawn 2x2), centred on cx
 * with its baseline at base: the key bubble's large letter */
static void big_char(int cx, int base, const char *s)
{
	const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);
	glyph_t g;
	int x0, y0, r, c, nb;
	font_glyph(FONT_TITLE, utf8_next(&p, end), &g);
	nb = (g.w + 7) >> 3;
	x0 = cx - g.w;		/* 2 * w / 2 */
	y0 = base - 2 * g.top;
	for (r = 0; r < g.h; ++r)
		for (c = 0; c < g.w; ++c)
			if (g.bits[r * nb + (c >> 3)] & (0x80 >> (c & 7)))
				fill(x0 + 2 * c, y0 + 2 * r, 2, 2, BLACK);
}

static void draw_keyboard(void)
{
	int i;
	build_keys(A.kb_layer);
	/* light keys on a dark board, like the original */
	fill(0, KB_Y, SCREEN_W, KB_H, BLACK);
	for (i = 0; i < nkeys; ++i) {
		const key_t_ *k = &keys[i];
		int pressed = i == A.pressed_key;
		int x = k->x + 2, y = k->y + 2, w = k->w - 4, h = KB_ROW_H - 4;
		int font = strlen(k->label) > 1 && (unsigned char)k->label[0] < 0x80 ? FONT_SMALL : FONT_HEADING;
		fill(x, y, w, h, WHITE);
		/* rounded corners */
		fill(x, y, 1, 1, BLACK);
		fill(x + w - 1, y, 1, 1, BLACK);
		fill(x, y + h - 1, 1, 1, BLACK);
		fill(x + w - 1, y + h - 1, 1, 1, BLACK);
		if (pressed)
			fill(x + 2, y + 2, w - 4, h - 4, BLACK);
		if (k->label[0])
			text_centre(k->x + k->w / 2, k->y + (font == FONT_SMALL ? 18 : 20), font, k->label,
				    pressed ? WHITE : BLACK);
	}
	/* the pressed letter, large, in a bubble above the key (a finger
	 * covers the key itself) */
	if (A.pressed_key >= 0 && A.pressed_key < nkeys) {
		const key_t_ *k = &keys[A.pressed_key];
		const char *l = k->label;
		int one = l[0] && (!l[1] || ((unsigned char)l[0] >= 0xc0 && strlen(l) <= 4));
		if (one && strcmp(l, "-") != 0 && strcmp(l, ".") != 0) {
			int bw = 46, bh = 58, bx = k->x + k->w / 2 - bw / 2, by = k->y - bh + 4;
			if (bx < 1)
				bx = 1;
			if (bx + bw > SCREEN_W - 1)
				bx = SCREEN_W - 1 - bw;
			if (by < 0)
				by = 0;
			fill(bx, by, bw, bh, WHITE);
			gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, bx, by, bw, bh, BLACK);
			gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, bx + 1, by + 1, bw - 2, bh - 2, BLACK);
			big_char(bx + bw / 2, by + 42, l);
		}
	}
}

/* ------------------------------------------------------------------ */
/* calculator: RANDOM held opens it, RANDOM (or SEARCH, HISTORY) leaves.
 * A row of mode tabs at the top (Simple, Sci), the expression and
 * result below them, then the keys.  The expression is kept in the
 * calculator's own ASCII form (see calc.h) and shown with symbols */

#define CALC_TAB_H   18

enum { CK_AC = 1, CK_DEL, CK_NEG, CK_EQ, CK_2ND, CK_DEG, CK_SQ, CK_CUBE, CK_RECIP,
       CK_EXPE, CK_TENX };

typedef struct calc_key {
	uint8_t col, row, span;
	const char *label;
	uint8_t code;		/* expression char, or a CK_ code */
	const char *label2;	/* with 2nd: another key (or NULL) */
	uint8_t code2;
} calc_key_t;

typedef struct calc_layout {
	const calc_key_t *keys;
	int nkeys, cols, rows, col_w, row_h, key_y;
	int expr_base, res_base, res_font;	/* display */
} calc_layout_t;

static const calc_key_t calc_simple[] = {
	{ 0, 0, 1, "AC", CK_AC, 0, 0 }, { 1, 0, 1, "DEL", CK_DEL, 0, 0 },
	{ 2, 0, 1, "(", '(', 0, 0 }, { 3, 0, 1, ")", ')', 0, 0 }, { 4, 0, 1, "\xc3\xb7", '/', 0, 0 },
	{ 0, 1, 1, "7", '7', 0, 0 }, { 1, 1, 1, "8", '8', 0, 0 }, { 2, 1, 1, "9", '9', 0, 0 },
	{ 3, 1, 1, "%", '%', 0, 0 }, { 4, 1, 1, "\xc3\x97", '*', 0, 0 },
	{ 0, 2, 1, "4", '4', 0, 0 }, { 1, 2, 1, "5", '5', 0, 0 }, { 2, 2, 1, "6", '6', 0, 0 },
	{ 3, 2, 1, "\xc2\xb1", CK_NEG, 0, 0 }, { 4, 2, 1, "\xe2\x88\x92", '-', 0, 0 },
	{ 0, 3, 1, "1", '1', 0, 0 }, { 1, 3, 1, "2", '2', 0, 0 }, { 2, 3, 1, "3", '3', 0, 0 },
	{ 3, 3, 1, "Ans", 'A', 0, 0 }, { 4, 3, 1, "+", '+', 0, 0 },
	{ 0, 4, 2, "0", '0', 0, 0 }, { 2, 4, 1, ".", '.', 0, 0 }, { 3, 4, 2, "=", CK_EQ, 0, 0 },
};

#define SUP_M1 "\xe2\x81\xbb\xc2\xb9"	/* superscript -1 */
static const calc_key_t calc_sci[] = {
	{ 0, 0, 1, "2nd", CK_2ND, 0, 0 },
	{ 1, 0, 1, "sin", 's', "sin" SUP_M1, 'S' },
	{ 2, 0, 1, "cos", 'o', "cos" SUP_M1, 'O' },
	{ 3, 0, 1, "tan", 't', "tan" SUP_M1, 'T' },
	{ 4, 0, 1, "(", '(', 0, 0 }, { 5, 0, 1, ")", ')', 0, 0 },
	{ 0, 1, 1, "DEG", CK_DEG, 0, 0 },	/* label shows DEG or RAD */
	{ 1, 1, 1, "ln", 'l', "e\xcb\xa3", CK_EXPE },
	{ 2, 1, 1, "log", 'g', "10\xcb\xa3", CK_TENX },
	{ 3, 1, 1, "\xe2\x88\x9a", 'r', "x!", '!' },
	{ 4, 1, 1, "x\xc2\xb2", CK_SQ, "x\xc2\xb3", CK_CUBE },
	{ 5, 1, 1, "x\xca\xb8", '^', "x" SUP_M1, CK_RECIP },
	{ 0, 2, 1, "7", '7', 0, 0 }, { 1, 2, 1, "8", '8', 0, 0 }, { 2, 2, 1, "9", '9', 0, 0 },
	{ 3, 2, 1, "DEL", CK_DEL, 0, 0 }, { 4, 2, 1, "AC", CK_AC, 0, 0 },
	{ 5, 2, 1, "\xcf\x80", 'p', 0, 0 },
	{ 0, 3, 1, "4", '4', 0, 0 }, { 1, 3, 1, "5", '5', 0, 0 }, { 2, 3, 1, "6", '6', 0, 0 },
	{ 3, 3, 1, "\xc3\x97", '*', 0, 0 }, { 4, 3, 1, "\xc3\xb7", '/', 0, 0 },
	{ 5, 3, 1, "e", 'e', 0, 0 },
	{ 0, 4, 1, "1", '1', 0, 0 }, { 1, 4, 1, "2", '2', 0, 0 }, { 2, 4, 1, "3", '3', 0, 0 },
	{ 3, 4, 1, "+", '+', 0, 0 }, { 4, 4, 1, "\xe2\x88\x92", '-', 0, 0 },
	{ 5, 4, 1, "EXP", 'E', 0, 0 },
	{ 0, 5, 1, "0", '0', 0, 0 }, { 1, 5, 1, ".", '.', 0, 0 },
	{ 2, 5, 1, "\xc2\xb1", CK_NEG, 0, 0 }, { 3, 5, 1, "Ans", 'A', 0, 0 },
	{ 4, 5, 1, "%", '%', 0, 0 }, { 5, 5, 1, "=", CK_EQ, 0, 0 },
};

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const calc_layout_t calc_layouts[2] = {
	/* Simple: 5 x 5, 48 x 28 keys, large result */
	{ calc_simple, NELEM(calc_simple), 5, 5, 48, 28, 68, 38, 62, FONT_TITLEL },
	/* Sci: 6 x 6, 40 x 24 keys */
	{ calc_sci, NELEM(calc_sci), 6, 6, 40, 24, 62, 34, 56, FONT_TITLE },
};
static const char *const calc_tabs[2] = { "Simple", "Sci" };
static const int calc_tab_x[3] = { 0, 64, 106 };	/* tab edges */

static const calc_layout_t *calc_lay(void)
{
	return &calc_layouts[A.calc_mode ? 1 : 0];
}

static int calc_flags(void)
{
	return A.calc_rad ? 0 : CALC_DEG;
}

static int calc_key_at(int x, int y)
{
	const calc_layout_t *L = calc_lay();
	int i, col, row;
	if (y < L->key_y || x < 0 || x >= SCREEN_W)
		return -1;
	col = x / L->col_w;
	row = (y - L->key_y) / L->row_h;
	if (row >= L->rows)
		row = L->rows - 1;
	for (i = 0; i < L->nkeys; ++i)
		if (L->keys[i].row == row && col >= L->keys[i].col &&
		    col < L->keys[i].col + L->keys[i].span)
			return i;
	return -1;
}

static void calc_enter(void)
{
	if (A.screen == S_CALC)
		return;
	A.calc_ret = A.screen;
	A.screen = S_CALC;
	A.calc_2nd = 0;
	A.toolbar = 0;
	A.vel = 0;
	A.need_draw = 1;
}

static void calc_leave(void)
{
	if (A.screen != S_CALC)
		return;
	A.screen = A.calc_ret;
	if (A.screen == S_ARTICLE && !A.have_article)
		A.screen = S_SEARCH;
	A.need_draw = 1;
}

static int calc_is_op(int c)
{
	return c == '+' || c == '-' || c == '*' || c == '/' || c == '^';
}

static int calc_is_fn(int c)
{
	return c && strchr("sotSOTlgr", c) != NULL;
}

/* does an operand end with c (so an operator or postfix can follow)? */
static int calc_ends_operand(int c)
{
	return (c >= '0' && c <= '9') || c == '.' || c == ')' || c == 'A' || c == 'p' ||
	       c == 'e' || c == '%' || c == '!';
}

/* the last character, or 0 */
static int calc_last(void)
{
	return A.calc_len ? (unsigned char)A.calc_expr[A.calc_len - 1] : 0;
}

static int calc_open_brackets(void)
{
	int i, n = 0;
	for (i = 0; i < A.calc_len; ++i)
		n += A.calc_expr[i] == '(' ? 1 : A.calc_expr[i] == ')' ? -1 : 0;
	return n;
}

/* is the '-' at i a sign (not a subtraction)? */
static int calc_is_sign(int i)
{
	int p = i ? (unsigned char)A.calc_expr[i - 1] : 0;
	return !p || calc_is_op(p) || p == '(' || p == 'E';
}

static void calc_insert(int at, int c)
{
	if (A.calc_len >= CALC_MAX)
		return;
	memmove(A.calc_expr + at + 1, A.calc_expr + at, (size_t)(A.calc_len - at));
	A.calc_expr[at] = (char)c;
	A.calc_len++;
	A.calc_expr[A.calc_len] = 0;
}

static void calc_append(const char *s)
{
	if (A.calc_len + (int)strlen(s) > CALC_MAX)
		return;
	while (*s)
		calc_insert(A.calc_len, (unsigned char)*s++);
}

static void calc_remove(int at)
{
	memmove(A.calc_expr + at, A.calc_expr + at + 1, (size_t)(A.calc_len - at - 1));
	A.calc_len--;
	A.calc_expr[A.calc_len] = 0;
}

static void calc_set(const char *s)
{
	A.calc_len = (int)strlen(s);
	memcpy(A.calc_expr, s, (size_t)A.calc_len + 1);
}

/* start of the number being typed at the end: i such that expr[i..] is
 * digits and points; *in_exp set if it is the part after an E */
static int calc_number_start(int *in_exp)
{
	int i = A.calc_len;
	while (i > 0 && ((A.calc_expr[i - 1] >= '0' && A.calc_expr[i - 1] <= '9') ||
			 A.calc_expr[i - 1] == '.'))
		i--;
	*in_exp = 0;
	if (i > 0 && A.calc_expr[i - 1] == 'E')
		*in_exp = 1;
	else if (i > 1 && (A.calc_expr[i - 1] == '-' || A.calc_expr[i - 1] == '+') &&
		 A.calc_expr[i - 2] == 'E')
		*in_exp = 1;
	return i;
}

/* ± : change the sign of the last number (or bracket, function, Ans) */
static void calc_negate(void)
{
	int i = A.calc_len, c = calc_last();
	if (c == ')') {
		/* back to the matching (, and its function if any */
		int depth = 0;
		while (i > 0) {
			c = (unsigned char)A.calc_expr[--i];
			depth += c == ')' ? 1 : c == '(' ? -1 : 0;
			if (!depth)
				break;
		}
		if (i > 0 && calc_is_fn((unsigned char)A.calc_expr[i - 1]))
			i--;
	} else {
		while (i > 0) {
			c = (unsigned char)A.calc_expr[i - 1];
			if ((c >= '0' && c <= '9') || c == '.' || c == 'A' || c == 'p' || c == 'e' ||
			    c == '%' || c == '!' || c == 'E')
				i--;
			else if ((c == '-' || c == '+') && i > 1 && A.calc_expr[i - 2] == 'E')
				i -= 2;	/* the sign of an exponent: part of the number */
			else
				break;
		}
	}
	/* i: start of the operand (== calc_len if there is none yet) */
	if (i > 0 && A.calc_expr[i - 1] == '-') {
		if (calc_is_sign(i - 1))
			calc_remove(i - 1);		/* -5 -> 5 */
		else
			A.calc_expr[i - 1] = '+';	/* 3-5 -> 3+5 */
	} else if (i > 0 && A.calc_expr[i - 1] == '+' && !calc_is_sign(i - 1)) {
		A.calc_expr[i - 1] = '-';		/* 3+5 -> 3-5 */
	} else {
		calc_insert(i, '-');			/* 5 -> -5 */
	}
}

static void calc_press(int code)
{
	int c, in_exp;
	if (code == CK_2ND) {
		A.calc_2nd = !A.calc_2nd;
		return;
	}
	A.calc_2nd = 0;
	if (code == CK_DEG) {
		A.calc_rad = !A.calc_rad;
		A.state_dirty = 1;
		if (A.calc_done) {
			/* show the answer of the same sum in the new unit */
			double v;
			if (calc_eval(A.calc_expr, A.calc_ans, calc_flags(), &v) == CALC_OK)
				A.calc_ans = v;
		}
		return;
	}
	if (code == CK_AC) {
		calc_set("");
		A.calc_done = 0;
		A.calc_err = 0;
		return;
	}
	if (A.calc_done) {
		/* after =: a number starts afresh, an operator carries on
		 * from the answer */
		A.calc_done = 0;
		if (calc_is_op(code) || code == '%' || code == '!' || code == CK_SQ ||
		    code == CK_CUBE || code == CK_RECIP)
			calc_set("A");
		else if (code == CK_NEG) {
			calc_set("-A");
			A.calc_done = 1;
			code = CK_EQ;
		} else if (code != CK_DEL && code != CK_EQ)
			calc_set("");
	}
	c = calc_last();
	A.calc_err = 0;
	switch (code) {
	case CK_DEL:
		if (A.calc_len) {
			int was = calc_last();
			calc_remove(A.calc_len - 1);
			if (was == '(' && calc_is_fn(calc_last()))
				calc_remove(A.calc_len - 1);	/* "sin(" goes at once */
		}
		return;
	case CK_NEG:
		calc_negate();
		return;
	case CK_EQ: {
		double v;
		int r;
		if (!A.calc_len)
			return;
		r = calc_eval(A.calc_expr, A.calc_ans, calc_flags(), &v);
		if (r == CALC_OK) {
			A.calc_ans = v;
			A.calc_done = 1;
			A.state_dirty = 1;
		} else {
			A.calc_err = r;
		}
		return;
	}
	case '+': case '*': case '/': case '^':
		if (!A.calc_len)
			calc_insert(0, 'A');	/* starts from Ans */
		else if (c == '(' || c == 'E')
			return;
		else if (calc_is_op(c)) {
			/* replace the operator (and a sign after one) */
			while (A.calc_len && calc_is_op(calc_last()))
				calc_remove(A.calc_len - 1);
			if (!A.calc_len)
				return;
		}
		calc_insert(A.calc_len, code);
		return;
	case '-':
		if (c == '-')
			return;
		if (c == '+')
			calc_remove(A.calc_len - 1);	/* 3+ then - is 3- */
		calc_insert(A.calc_len, '-');	/* also a sign after * / ^ ( E */
		return;
	case '%': case '!':
		if (calc_ends_operand(c))
			calc_insert(A.calc_len, code);
		return;
	case CK_SQ: case CK_CUBE: case CK_RECIP:
		if (!A.calc_len)
			calc_insert(0, 'A');
		else if (!calc_ends_operand(c))
			return;
		calc_append(code == CK_SQ ? "^2" : code == CK_CUBE ? "^3" : "^-1");
		return;
	case ')':
		if (calc_open_brackets() > 0 && calc_ends_operand(c))
			calc_insert(A.calc_len, ')');
		return;
	case '.': {
		/* one point per number, none in an exponent; "." alone
		 * becomes "0." */
		int i = calc_number_start(&in_exp);
		if (in_exp)
			return;
		if (strchr(A.calc_expr + i, '.'))
			return;
		if (i == A.calc_len)
			calc_insert(A.calc_len, '0');
		calc_insert(A.calc_len, '.');
		return;
	}
	case 'E': {
		/* times ten to the: after a number, or 1E on its own */
		int i = calc_number_start(&in_exp);
		if (in_exp)
			return;
		if (i == A.calc_len)
			calc_insert(A.calc_len, '1');
		calc_insert(A.calc_len, 'E');
		return;
	}
	case CK_EXPE:
		calc_append("e^(");
		return;
	case CK_TENX:
		if ((c >= '0' && c <= '9') || c == '.')
			calc_insert(A.calc_len, '*');	/* 2 then 10^x: 2*10^( */
		calc_append("10^(");
		return;
	default:
		if (calc_is_fn(code)) {
			char f[3] = { (char)code, '(', 0 };
			calc_append(f);
			return;
		}
		/* digits, (, Ans, pi, e */
		if (code >= '0' && code <= '9' && c == '0') {
			/* no leading zeros: "007" is "7" (but 0.07 and 10 are fine) */
			int i = A.calc_len - 1;
			if (i == 0 || !((A.calc_expr[i - 1] >= '0' && A.calc_expr[i - 1] <= '9') ||
					A.calc_expr[i - 1] == '.'))
				calc_remove(i);
		}
		calc_insert(A.calc_len, code);
		return;
	}
}

/* the expression as shown: x for *, ÷ for /, − for -, Ans, π, sin ... */
static void calc_display(const char *e, char *out, size_t cap)
{
	size_t n = 0;
	for (; *e && n + 9 < cap; ++e) {
		const char *t = NULL;
		switch (*e) {
		case '*': t = "\xc3\x97"; break;
		case '/': t = "\xc3\xb7"; break;
		case '-': t = "\xe2\x88\x92"; break;
		case 'A': t = "Ans"; break;
		case 'p': t = "\xcf\x80"; break;
		case 's': t = "sin"; break;
		case 'o': t = "cos"; break;
		case 't': t = "tan"; break;
		case 'S': t = "sin" SUP_M1; break;
		case 'O': t = "cos" SUP_M1; break;
		case 'T': t = "tan" SUP_M1; break;
		case 'l': t = "ln"; break;
		case 'g': t = "log"; break;
		case 'r': t = "\xe2\x88\x9a"; break;
		}
		if (t) {
			size_t k = strlen(t);
			memcpy(out + n, t, k);
			n += k;
		} else {
			out[n++] = *e;
		}
	}
	out[n] = 0;
}

/* right aligned at right edge xr; if too wide, the start is cut off and
 * shown as … (the end, where you type, stays visible) */
static void calc_text_right(int xr, int base, int maxw, int font, const char *s, int colour)
{
	static const char ell[] = "\xe2\x80\xa6";
	const char *p = s;
	int w = font_width(font, s, strlen(s));
	if (w > maxw) {
		int we = font_width(font, ell, 3);
		while (*p && font_width(font, p, strlen(p)) > maxw - we) {
			p++;
			while (((uint8_t)*p & 0xc0) == 0x80)
				p++;
		}
		w = we + font_width(font, p, strlen(p));
		text_at(xr - w, base, font, ell, colour);
		text_at(xr - w + we, base, font, p, colour);
		return;
	}
	text_at(xr - w, base, font, s, colour);
}

/* the biggest of these fonts the label fits in */
static int calc_label_font(const char *s, int maxw, int big)
{
	static const int fonts[] = { FONT_TITLE, FONT_HEADING, FONT_SMALLL, FONT_SMALL };
	int i;
	for (i = big ? 0 : 1; i < 3; ++i)
		if (font_width(fonts[i], s, strlen(s)) <= maxw)
			return fonts[i];
	return FONT_SMALL;
}

static void draw_calc(void)
{
	const calc_layout_t *L = calc_lay();
	char shown[CALC_MAX * 6 + 8], res[48];
	int i, w, font, left = 4;
	double v;

	memset(A.fb, 0, sizeof(A.fb));
	/* mode tabs, and how to leave */
	for (i = 0; i < 2; ++i) {
		int x = calc_tab_x[i], tw = calc_tab_x[i + 1] - x, on = i == (A.calc_mode ? 1 : 0);
		if (on)
			fill(x, 0, tw, CALC_TAB_H, BLACK);
		else
			fill(x + tw - 1, 3, 1, CALC_TAB_H - 6, BLACK);
		text_centre(x + tw / 2, 14, FONT_HEADING, calc_tabs[i], on ? WHITE : BLACK);
	}
	text_at(SCREEN_W - 4 - font_width(FONT_SMALL, "RANDOM: back", 12), 12, FONT_SMALL,
		"RANDOM: back", BLACK);
	hline(0, CALC_TAB_H, SCREEN_W);

	/* angle unit and 2nd, at the left of the display */
	if (A.calc_mode) {
		text_at(3, CALC_TAB_H + 12, FONT_SMALL, A.calc_rad ? "RAD" : "DEG", BLACK);
		if (A.calc_2nd) {
			fill(2, CALC_TAB_H + 15, 19, 11, BLACK);
			text_at(4, CALC_TAB_H + 24, FONT_SMALL, "2nd", WHITE);
		}
		left = 24;
	}

	/* expression, then the result (or a preview of it) */
	calc_display(A.calc_expr, shown, sizeof(shown) - 3);
	if (A.calc_done)
		strcat(shown, " =");
	calc_text_right(SCREEN_W - 5, L->expr_base, SCREEN_W - 5 - left, FONT_TEXT, shown, BLACK);
	res[0] = 0;
	font = L->res_font;
	if (A.calc_err) {
		strcpy(res, calc_error_text(A.calc_err));
		font = FONT_HEADING;
	} else if (A.calc_done) {
		calc_format(A.calc_ans, res);
	} else if (!A.calc_len) {
		strcpy(res, "0");
	} else if (calc_eval(A.calc_expr, A.calc_ans, calc_flags(), &v) == CALC_OK) {
		/* live preview while typing, smaller than a result */
		calc_format(v, res);
		font = FONT_HEADING;
	}
	if (res[0]) {
		w = font_width(font, res, strlen(res));
		if (w > SCREEN_W - 5 - left && font == FONT_TITLEL)
			font = FONT_TITLE;
		calc_text_right(SCREEN_W - 5, L->res_base, SCREEN_W - 5 - left, font, res, BLACK);
	}

	/* keys: light on a dark board like the search keyboard; = dark */
	fill(0, L->key_y - 2, SCREEN_W, SCREEN_H - L->key_y + 2, BLACK);
	for (i = 0; i < L->nkeys; ++i) {
		const calc_key_t *k = &L->keys[i];
		int x = k->col * L->col_w + 2, y = L->key_y + k->row * L->row_h + 2;
		int kw = k->span * L->col_w - 4, kh = L->row_h - 4;
		int on = (k->code == CK_EQ) || (k->code == CK_2ND && A.calc_2nd);
		int dark = on != (i == A.pressed_key);
		const char *label = k->label;
		int code = k->code, big, f;
		if (A.calc_2nd && k->label2) {
			label = k->label2;
			code = k->code2;
		}
		if (code == CK_DEG)
			label = A.calc_rad ? "RAD" : "DEG";
		/* operators a size up: the minus is thin */
		big = calc_is_op(code) && code != '^';
		big |= code == CK_NEG || code == CK_EQ;
		f = calc_label_font(label, kw - 4, big);
		fill(x, y, kw, kh, WHITE);
		fill(x, y, 1, 1, BLACK);
		fill(x + kw - 1, y, 1, 1, BLACK);
		fill(x, y + kh - 1, 1, 1, BLACK);
		fill(x + kw - 1, y + kh - 1, 1, 1, BLACK);
		if (dark)
			fill(x + 2, y + 2, kw - 4, kh - 4, BLACK);
		else if (A.calc_2nd && k->label2)
			gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, x + 2, y + 2, kw - 4, kh - 4, BLACK);
		text_centre(x + kw / 2, y + kh / 2 + (f == FONT_SMALL ? 4 : f == FONT_SMALLL ? 5 : 6),
			    f, label, dark ? WHITE : BLACK);
	}
}

/* a tap on the mode tabs */
static int calc_tab_tap(int x, int y)
{
	int m;
	if (y >= CALC_TAB_H || x >= calc_tab_x[2])
		return 0;
	m = x >= calc_tab_x[1];
	if (m != A.calc_mode) {
		A.calc_mode = m;
		A.calc_2nd = 0;
		A.state_dirty = 1;
	}
	A.need_draw = 1;
	return 1;
}

/* ------------------------------------------------------------------ */
/* search */

static int visible_rows(void)
{
	int h = (A.kb_visible ? KB_Y : SCREEN_H) - BAR_H - 1;
	int n = h / ROW_H;
	return n > MAX_ROWS ? MAX_ROWS : n;
}

static void fetch_rows(void)
{
	int i, n;
	if (!A.have_lib) {
		A.nrows = 0;
		return;
	}
	n = lib_results(&A.lib, A.res_pos, A.rows, MAX_ROWS);
	A.nrows = n < 0 ? 0 : n;
	for (i = 0; i < A.nrows; ++i) {
		A.row_target[i][0] = 0;
		if (A.rows[i].redirect && i < visible_rows()) {
			zim_dirent_t d;
			if (zim_dirent(&A.lib.zim, A.rows[i].entry, &d) == 0) {
				strncpy(A.row_target[i], d.title, sizeof(A.row_target[i]) - 1);
				A.row_target[i][sizeof(A.row_target[i]) - 1] = 0;
			}
		}
	}
}

static void run_search(void)
{
	uint32_t pos = 0;
	A.search_dirty = 0;
	if (!A.have_lib)
		return;
	if (A.qlen == 0) {
		A.nrows = 0;
		A.res_pos = 0;
	} else {
		if (lib_seek(&A.lib, A.query, &pos) < 0)
			pos = 0;
		A.res_pos = pos;
		fetch_rows();
	}
	A.need_draw = 1;
}

static void scroll_results(int delta)
{
	int64_t np = (int64_t)A.res_pos + delta;
	if (np < 0)
		np = 0;
	if (A.have_lib && A.lib.total && np > (int64_t)A.lib.total - 1)
		np = (int64_t)A.lib.total - 1;
	if ((uint32_t)np == A.res_pos)
		return;
	A.res_pos = (uint32_t)np;
	fetch_rows();
	A.need_draw = 1;
}

static void query_backspace(void)
{
	if (!A.qlen)
		return;
	A.qlen--;
	while (A.qlen && ((unsigned char)A.query[A.qlen] & 0xc0) == 0x80)
		A.qlen--;
	A.query[A.qlen] = 0;
	A.search_dirty = 1;
	A.state_dirty = 1;
}

static void query_add(int c)
{
	if (A.qlen >= QUERY_MAX)
		return;
	if (c == ' ' && (A.qlen == 0 || A.query[A.qlen - 1] == ' '))
		return;
	A.query[A.qlen++] = (char)c;
	A.query[A.qlen] = 0;
	A.search_dirty = 1;
	A.state_dirty = 1;
}

static void key_press(int code)
{
	switch (code) {
	case K_SHIFT:
		A.kb_layer = !A.kb_layer;
		break;
	case K_HIDE:
		A.kb_visible = 0;
		fetch_rows();
		break;
	case K_BS:
		query_backspace();
		break;
	case K_GO:
		if (A.search_dirty)
			run_search();
		if (A.nrows)
			open_entry(A.rows[0].entry, 0, 1);
		return;
	default:
		query_add(code);
	}
	A.need_draw = 1;
}

static void draw_search(void)
{
	int i, rows = visible_rows(), y;
	memset(A.fb, 0, sizeof(A.fb));
	/* query bar */
	gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, 0, 0, SCREEN_W, BAR_H, BLACK);
	if (A.qlen) {
		int w = font_width(FONT_TEXT, A.query, (size_t)A.qlen) + 3, x = 5;
		if (w > SCREEN_W - 14)
			x = SCREEN_W - 9 - w;	/* keep the end of a long query visible */
		x = font_draw(A.fb, SCREEN_STRIDE, BAR_H, x, 15, FONT_TEXT, A.query,
			      (size_t)A.qlen, BLACK);
		/* the font's space is only 3 px: make a typed space visible */
		if (A.query[A.qlen - 1] == ' ')
			x += 4;
		/* glyphs end 1 px before their advance: this leaves a 1 px gap */
		fill(x, 3, 2, 14, BLACK);
		fill(0, 1, 4, BAR_H - 2, WHITE);
		gfx_rect(A.fb, SCREEN_STRIDE, SCREEN_H, 0, 0, SCREEN_W, BAR_H, BLACK);
	} else {
		fill(5, 3, 2, 14, BLACK);
		text_clip(10, 15, SCREEN_W - 16, FONT_ITALIC,
			  A.have_lib ? A.lib.info.title : "Search", BLACK);
	}
	y = BAR_H + 1;
	if (!A.qlen) {
		char line[48];
		uint32_t n = A.have_lib ? A.lib.info.articles : 0;
		int k = 0, j;
		char t[16];
		do {
			t[k++] = (char)('0' + n % 10);
			n /= 10;
			if (n && (k == 3 || k == 7 || k == 11))
				t[k++] = ',';
		} while (n);
		for (j = 0; j < k; ++j)
			line[j] = t[k - 1 - j];
		{
			const char *w = A.have_lib && A.lib.info.articles == 1 ? " article" : " articles";
			memcpy(line + k, w, strlen(w) + 1);
		}
		text_centre(SCREEN_W / 2, y + 20, FONT_TEXT, line, BLACK);
		if (!A.lib.info.prepared)
			text_centre(SCREEN_W / 2, y + 35, FONT_SMALL,
				    "No index: exact titles only (run zimprep)", BLACK);
		else if (A.nlibs > 1)
			text_centre(SCREEN_W / 2, y + 35, FONT_SMALL,
				    "Tap the name above to change wiki", BLACK);
		text_centre(SCREEN_W / 2, y + (A.kb_visible ? 49 : 60), FONT_SMALL,
			    "Type to search. Buttons:", BLACK);
		text_centre(SCREEN_W / 2, y + (A.kb_visible ? 60 : 73), FONT_SMALL,
			    "SEARCH  clear    HISTORY  back    RANDOM  any", BLACK);
		{
			/* the two switches, each half of the line is a button */
			const char *a = A.opts & HTML_NOTES ? "Footnotes: on" : "Footnotes: off";
			const char *b = A.opts & HTML_CITES ? "Citations: on" : "Citations: off";
			int wa = font_width(FONT_SMALL, a, strlen(a)), wb = font_width(FONT_SMALL, b, strlen(b));
			const char *c = A.opts & HTML_LARGE ? "Large text: on" : "Large text: off";
			int wc = font_width(FONT_SMALL, c, strlen(c));
			A.opt_y = y + (A.kb_visible ? 74 : 96);
			text_at(SCREEN_W / 4 - wa / 2, A.opt_y, FONT_SMALL, a, BLACK);
			text_at(3 * SCREEN_W / 4 - wb / 2, A.opt_y, FONT_SMALL, b, BLACK);
			gfx_dotted(A.fb, SCREEN_STRIDE, SCREEN_H, SCREEN_W / 4 - wa / 2, A.opt_y + 2, wa);
			gfx_dotted(A.fb, SCREEN_STRIDE, SCREEN_H, 3 * SCREEN_W / 4 - wb / 2, A.opt_y + 2, wb);
			/* the third switch on a line of its own below */
			text_at(SCREEN_W / 2 - wc / 2, A.opt_y + OPT2_DY, FONT_SMALL, c, BLACK);
			gfx_dotted(A.fb, SCREEN_STRIDE, SCREEN_H, SCREEN_W / 2 - wc / 2,
				   A.opt_y + OPT2_DY + 2, wc);
		}
		{
			/* "zimwiki 1.0.0 · Oct 29 2026" at the foot of the free area */
			static const char v[] = "zimwiki " ZW_VERSION "  \xc2\xb7  " __DATE__;
			text_centre(SCREEN_W / 2, (A.kb_visible ? KB_Y : SCREEN_H) - 4, FONT_SMALL, v, BLACK);
		}
	} else {
		A.opt_y = 0;
		for (i = 0; i < rows && i < A.nrows; ++i) {
			int base = y + i * ROW_H + 14, x;
			int inv = i == A.pressed_row;
			if (inv)
				fill(0, y + i * ROW_H, SCREEN_W, ROW_H, BLACK);
			if (A.rows[i].redirect && A.row_target[i][0]) {
				int tw = font_width(FONT_TEXT, A.rows[i].title, strlen(A.rows[i].title));
				if (tw > 150)
					tw = 150;
				x = text_clip(4, base, tw, FONT_TEXT, A.rows[i].title, inv ? WHITE : BLACK);
				x = text_at(x + 3, base, FONT_TEXT, "\xe2\x86\x92", inv ? WHITE : BLACK);
				text_clip(x + 3, base, SCREEN_W - x - 6, FONT_ITALIC, A.row_target[i],
					  inv ? WHITE : BLACK);
			} else {
				text_clip_mid(4, base, SCREEN_W - 8, FONT_TEXT, A.rows[i].title,
					  inv ? WHITE : BLACK);
			}
			if (!inv)
				gfx_dotted(A.fb, SCREEN_STRIDE, SCREEN_H, 0, y + (i + 1) * ROW_H - 1, SCREEN_W);
		}
		if (!A.nrows)
			text_centre(SCREEN_W / 2, y + 22, FONT_ITALIC, "No matching titles", BLACK);
	}
	if (A.kb_visible)
		draw_keyboard();
}

/* ------------------------------------------------------------------ */
/* article view */

#define TOOLS 5
static const char *const tool_labels[TOOLS] = { "Back", "List", "Top", "Prev", "Next" };

static void draw_toolbar(void)
{
	int i, y = SCREEN_H - TOOL_H, w = SCREEN_W / TOOLS;
	fill(0, y, SCREEN_W, TOOL_H, WHITE);
	hline(0, y, SCREEN_W);
	hline(0, y + 1, SCREEN_W);
	for (i = 0; i < TOOLS; ++i) {
		const char *l = tool_labels[i];
		int tw = font_width(FONT_BOLD, l, strlen(l));
		int arrow = i != 1;	/* List has no arrow */
		int x0 = i * w + (w - tw - (arrow ? 9 : 0)) / 2;
		if (i)
			gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, i * w, y + 4, 1, TOOL_H - 8, BLACK);
		if (i == 0)
			triangle_left(x0, y + 8, 4, BLACK);
		else if (i == 2 || i == 3)
			triangle(x0 + 3, y + 10, 4, 1, BLACK);
		else if (i == 4)
			triangle(x0 + 3, y + 10, 4, 0, BLACK);
		text_at(x0 + (arrow ? 9 : 0), y + 17, FONT_BOLD, l, BLACK);
	}
}

/* headings of the open article, from the document (for the names) and
 * the layout (for where they are); a heading may wrap onto several lines */
static void build_sections(void)
{
	const doc_t *d = &A.doc;
	uint32_t b = 0, i;
	A.nsect = 0;
	A.sect_top = 0;
	A.sect[0].y = 0;
	A.sect[0].level = 0;
	strcpy(A.sect[0].title, "Top of the article");
	A.nsect = 1;
	for (i = 0; i < A.lay.nlines && A.nsect < SECT_MAX; ++i) {
		const lay_line_t *ln = &A.lay.lines[i];
		const lay_line_t *pv = i ? &A.lay.lines[i - 1] : NULL;
		uint32_t r, n = 0;
		struct sect *sc;
		if (ln->kind != BK_H2 && ln->kind != BK_H3 && ln->kind != BK_H4)
			continue;
		if (pv && pv->kind == ln->kind && ln->y <= pv->y + pv->h + 1)
			continue;	/* same heading, next line */
		/* the matching heading block */
		while (b < d->nblocks && d->blocks[b].kind != ln->kind)
			b++;
		if (b >= d->nblocks)
			break;
		sc = &A.sect[A.nsect++];
		sc->y = ln->y - 4 < 0 ? 0 : ln->y - 4;
		sc->level = (uint8_t)(ln->kind == BK_H2 ? 1 : ln->kind == BK_H3 ? 2 : 3);
		for (r = 0; r < d->blocks[b].nruns; ++r) {
			const doc_run_t *ru = &d->runs[d->blocks[b].first_run + r];
			uint32_t k = ru->len;
			if (n + k > sizeof(sc->title) - 1)
				k = sizeof(sc->title) - 1 - n;
			memcpy(sc->title + n, d->text + ru->start, k);
			n += k;
		}
		sc->title[n] = 0;
		b++;
	}
}

/* same section name? case, '_' and spaces ignored; the list keeps only
 * the start of very long headings */
static int same_section(const char *a, const char *b)
{
	int n = 0;
	for (;; a++, b++, n++) {
		char x = *a == '_' ? ' ' : *a, y = *b == '_' ? ' ' : *b;
		if (x >= 'A' && x <= 'Z')
			x = (char)(x + 32);
		if (y >= 'A' && y <= 'Z')
			y = (char)(y + 32);
		if (!x || !y)
			return (!x && !y) || n >= 70;
		if (x != y)
			return 0;
	}
}

/* scroll to the heading called name, laying out more of the page as
 * needed */
static void jump_to_section(const char *name)
{
	for (;;) {
		int i;
		build_sections();
		for (i = 1; i < A.nsect; ++i)
			if (same_section(A.sect[i].title, name)) {
				A.scroll = A.sect[i].y;
				if (A.scroll > max_scroll())
					A.scroll = max_scroll();
				A.hist[0].scroll = A.scroll;
				A.need_draw = 1;
				return;
			}
		if (A.lay.done || A.nsect >= SECT_MAX)
			return;
		lay_more(&A.lay, A.lay.height + 20 * SCREEN_H, 0);
	}
}

/* scroll to a footnote anchor ("cite note-9", "cite ref-9"): the line
 * holding its text, laying out more of the page as needed.  -1 if the
 * page has no such anchor */
static int jump_to_anchor(const char *name)
{
	uint32_t off, i;
	if (!A.have_article || doc_find_anchor(&A.doc, name, &off) < 0)
		return -1;
	for (;;) {
		int32_t y = -1;
		int past = 0;
		for (i = 0; i < A.lay.nlines; ++i) {
			const lay_line_t *ln = &A.lay.lines[i];
			if (!ln->nfrag)
				continue;
			if (A.lay.frags[ln->first].off > off) {
				past = 1;
				break;
			}
			y = ln->y;
		}
		if (past || A.lay.done) {
			if (y < 0)
				return -1;
			A.scroll = y > 4 ? y - 4 : 0;
			if (A.scroll > max_scroll())
				A.scroll = max_scroll();
			A.hist[0].scroll = A.scroll;
			A.need_draw = 1;
			return 0;
		}
		lay_more(&A.lay, A.lay.height + 20 * SCREEN_H, 0);
	}
}

/* finish laying out the page (for the section list and jumps) */
static void finish_layout(void)
{
	if (!A.have_article || A.lay.done)
		return;
	banner("Please wait\xe2\x80\xa6");
	A.opening = 2;
	A.open_t0 = plat_ms();
	A.open_drawn = 0;
	lay_more(&A.lay, -1, 0);
	A.opening = 0;
	A.need_draw = 1;
}

/* the Text size switch changed: lay the open page out again in the
 * new size, keeping the line at the top of the screen.  Saved scroll
 * positions (Back, HISTORY) are pixels, so they are scaled by how much
 * this page grew or shrank, or by a typical ratio */
static int32_t scale256(int32_t v, int32_t k)
{
	return (v >> 8) * k + ((v & 255) * k >> 8);
}

static void relayout_article(int large)
{
	layout_t nl;
	uint32_t i, off = 0;
	int32_t old_y = 0, new_y = 0;
	int have_off = 0;

	lay_set_large(large);
	if (!A.have_article || A.lay.large == (uint8_t)large)
		return;
	for (i = 0; i < A.lay.nlines; ++i) {
		const lay_line_t *ln = &A.lay.lines[i];
		if (ln->y + ln->h <= A.scroll || !ln->nfrag)
			continue;
		off = A.lay.frags[ln->first].off;
		old_y = ln->y;
		have_off = 1;
		break;
	}
	if (lay_begin(&nl, &A.doc) < 0) {
		lay_free(&nl);
		return;
	}
	banner("Please wait\xe2\x80\xa6");
	A.opening = 2;
	A.open_t0 = plat_ms();
	A.open_drawn = 0;
	for (;;) {
		int found = 0;
		if (!have_off)
			break;
		for (i = 0; i < nl.nlines; ++i) {
			const lay_line_t *ln = &nl.lines[i];
			if (!ln->nfrag)
				continue;
			if (nl.frags[ln->first].off > off) {
				found = 1;
				break;
			}
			new_y = ln->y;
		}
		if (found || nl.done)
			break;
		if (lay_more(&nl, nl.height + 20 * SCREEN_H, 0) < 0)
			break;
	}
	if (!nl.done)
		lay_more(&nl, new_y + 3 * SCREEN_H, 0);
	A.opening = 0;
	lay_free(&A.lay);
	A.lay = nl;
	A.lay.doc = &A.doc;
	{
		/* scale factor in 1/256ths (32-bit maths only: no 64-bit
		 * division on the device) */
		int32_t k = large ? 384 : 171;
		int j;
		if (old_y > 2 * SCREEN_H && new_y > 0)
			k = old_y < 0x400000 ? new_y * 256 / old_y : new_y / (old_y >> 8);
		if (k < 64 || k > 1024)
			k = large ? 384 : 171;
		for (j = 0; j < A.nback; ++j)
			if (A.back[j].kind == BI_ARTICLE)
				A.back[j].scroll = scale256(A.back[j].scroll, k);
		for (j = 0; j < A.nhist; ++j)
			A.hist[j].scroll = scale256(A.hist[j].scroll, k);
		A.hist_dirty = 1;
	}
	A.scroll = have_off ? new_y : 0;
	if (A.scroll > max_scroll())
		A.scroll = max_scroll();
	hist_update_scroll();
	A.vel = 0;
	A.hl_w = 0;
	A.need_draw = 1;
}

static void show_sections(void)
{
	int i;
	if (!A.have_article)
		return;
	finish_layout();
	build_sections();
	/* start the list at the section being read */
	for (i = 1; i < A.nsect; ++i)
		if (A.sect[i].y > A.scroll + 8)
			break;
	A.sect_top = i - 1 > 2 ? i - 3 : 0;
	push_current();
	A.screen = S_SECTIONS;
	A.toolbar = 0;
	A.need_draw = 1;
}

static void draw_article(void)
{
	lay_blit(&A.lay, A.scroll, A.fb, 0, SCREEN_H);
	if (A.hl_w)
		gfx_fill(A.fb, SCREEN_STRIDE, SCREEN_H, A.hl_x - 1, (int)(A.hl_y - A.scroll),
			 A.hl_w + 2, A.hl_h, INVERT);
	if (A.show_bar && A.lay.height > SCREEN_H) {
		int h = (int)((int64_t)SCREEN_H * SCREEN_H / A.lay.height);
		int y;
		if (h < 12)
			h = 12;
		y = (int)((int64_t)(SCREEN_H - h) * A.scroll / (max_scroll() ? max_scroll() : 1));
		fill(SCREEN_W - 3, 0, 3, SCREEN_H, WHITE);
		fill(SCREEN_W - 2, y, 2, h, BLACK);
	}
	if (A.toolbar)
		draw_toolbar();
}

static void tool_tap(int x)
{
	int i = x / (SCREEN_W / TOOLS);
	int32_t t;
	A.toolbar = 0;
	switch (i) {
	case 0:
		go_back();
		break;
	case 1:
		show_sections();
		break;
	case 2:
		A.scroll = 0;
		break;
	case 3:
		A.scroll = lay_next_heading(&A.lay, A.scroll, -1);
		break;
	case 4:
		t = lay_next_heading(&A.lay, A.scroll, 1);
		if (t < 0 && !A.lay.done) {
			finish_layout();
			t = lay_next_heading(&A.lay, A.scroll, 1);
		}
		if (t >= 0)
			A.scroll = t > max_scroll() ? max_scroll() : t;
		break;
	}
	A.state_dirty = 1;
	A.need_draw = 1;
}

static void article_tap(int x, int y)
{
	int rx, rw, rh, link;
	int32_t ry;
	if (A.toolbar && y >= SCREEN_H - TOOL_H) {
		tool_tap(x);
		return;
	}
	/* bottom left corner goes back, as on the original WikiReader */
	if (x < CORNER && y >= SCREEN_H - CORNER) {
		go_back();
		return;
	}
	link = lay_hit(&A.lay, x, A.scroll + y, &rx, &ry, &rw, &rh);
	if (link >= 0) {
		A.hl_x = rx;
		A.hl_y = ry;
		A.hl_w = rw;
		A.hl_h = rh;
		A.toolbar = 0;
		draw_article();
		present();
		A.hl_w = 0;
		open_link(link);
		return;
	}
	A.toolbar = !A.toolbar;
	A.need_draw = 1;
}

/* ------------------------------------------------------------------ */
/* history and library screens */

static void draw_list_header(const char *title, const char *right)
{
	fill(0, 0, SCREEN_W, BAR_H, BLACK);
	text_clip(5, 15, SCREEN_W - 70, FONT_BOLD, title, WHITE);
	if (right) {
		int w = font_width(FONT_BOLD, right, strlen(right));
		text_at(SCREEN_W - w - 6, 15, FONT_BOLD, right, WHITE);
	}
}

static int list_rows(void)
{
	return (SCREEN_H - BAR_H) / ROW_H;
}

static void draw_history(void)
{
	int i, y = BAR_H;
	memset(A.fb, 0, sizeof(A.fb));
	draw_list_header("History", A.nhist ? "Clear" : NULL);
	if (!A.nhist) {
		text_centre(SCREEN_W / 2, 70, FONT_ITALIC, "Articles you read appear here", BLACK);
		return;
	}
	for (i = 0; i < list_rows() && A.hist_top + i < A.nhist; ++i) {
		int inv = i == A.pressed_row;
		if (inv)
			fill(0, y + i * ROW_H, SCREEN_W, ROW_H, BLACK);
		text_clip_mid(4, y + i * ROW_H + 14, SCREEN_W - 8, FONT_TEXT,
			      A.hist[A.hist_top + i].title, inv ? WHITE : BLACK);
		if (!inv)
			gfx_dotted(A.fb, SCREEN_STRIDE, SCREEN_H, 0, y + (i + 1) * ROW_H - 1, SCREEN_W);
	}
}

static void draw_sections(void)
{
	int i, y = BAR_H;
	memset(A.fb, 0, sizeof(A.fb));
	draw_list_header(A.title, NULL);
	for (i = 0; i < list_rows() && A.sect_top + i < A.nsect; ++i) {
		const struct sect *sc = &A.sect[A.sect_top + i];
		int inv = i == A.pressed_row, ind = 4 + (sc->level > 1 ? (sc->level - 1) * 12 : 0);
		if (inv)
			fill(0, y + i * ROW_H, SCREEN_W, ROW_H, BLACK);
		text_clip(ind, y + i * ROW_H + 14, SCREEN_W - ind - 4,
			  sc->level == 1 ? FONT_BOLD : sc->level ? FONT_TEXT : FONT_ITALIC,
			  sc->title, inv ? WHITE : BLACK);
		if (!inv)
			gfx_dotted(A.fb, SCREEN_STRIDE, SCREEN_H, 0, y + (i + 1) * ROW_H - 1, SCREEN_W);
	}
}

static void draw_library(void)
{
	int i, y = BAR_H;
	memset(A.fb, 0, sizeof(A.fb));
	{
		/* "11 wikis" by hand: no snprintf on the device */
		char cnt[16];
		int n = 0, v = A.nlibs;
		if (v >= 10)
			cnt[n++] = (char)('0' + v / 10 % 10);
		cnt[n++] = (char)('0' + v % 10);
		memcpy(cnt + n, v == 1 ? " wiki" : " wikis", v == 1 ? 6 : 7);
		draw_list_header("Choose a wiki", cnt);
	}
	for (i = 0; i < list_rows() && A.lib_top + i < A.nlibs; ++i) {
		int inv = i == A.pressed_row;
		if (inv)
			fill(0, y + i * ROW_H, SCREEN_W, ROW_H, BLACK);
		text_clip(4, y + i * ROW_H + 14, SCREEN_W - 8, FONT_TEXT,
			  A.libs[A.lib_top + i].title, inv ? WHITE : BLACK);
		if (!inv)
			gfx_dotted(A.fb, SCREEN_STRIDE, SCREEN_H, 0, y + (i + 1) * ROW_H - 1, SCREEN_W);
	}
}

static void draw_message(void)
{
	int i;
	memset(A.fb, 0, sizeof(A.fb));
	text_centre(SCREEN_W / 2, 40, FONT_HEADING, A.msg_title, BLACK);
	hline(30, 48, SCREEN_W - 60);
	for (i = 0; i < A.nmsg; ++i)
		text_centre(SCREEN_W / 2, 74 + i * 18, FONT_TEXT, A.msg[i], BLACK);
	if (A.msg_return >= 0)
		text_centre(SCREEN_W / 2, SCREEN_H - 10, FONT_SMALL, "tap to continue", BLACK);
}

static void draw(void)
{
	switch (A.screen) {
	case S_SEARCH: draw_search(); break;
	case S_ARTICLE: draw_article(); break;
	case S_HISTORY: draw_history(); break;
	case S_SECTIONS: draw_sections(); break;
	case S_LIBRARY: draw_library(); break;
	case S_CALC: draw_calc(); break;
	default: draw_message(); break;
	}
	present();
	A.need_draw = 0;
}

/* ------------------------------------------------------------------ */
/* input */

static void goto_search(void)
{
	if (A.screen == S_SEARCH) {
		if (A.qlen) {
			A.qlen = 0;
			A.query[0] = 0;
			A.search_dirty = 1;
		}
		A.kb_visible = 1;
	} else {
		push_current();
		A.screen = S_SEARCH;
		/* keyboard as it was left (hidden to see more results stays
		 * hidden); always up for an empty search */
		if (!A.qlen)
			A.kb_visible = 1;
		/* the same results, scrolled as they were left (pressing
		 * SEARCH again clears) */
		if (A.qlen && A.nrows && A.have_lib)
			fetch_rows();
		else if (A.qlen)
			A.search_dirty = 1;
	}
	A.state_dirty = 1;
	A.need_draw = 1;
}

static void button(int code)
{
	if (!A.have_lib && code != BTN_POWER)
		return;
	A.toolbar = 0;
	A.vel = 0;
	if (A.screen == S_CALC && (code == BTN_SEARCH || code == BTN_HISTORY)) {
		/* leave the calculator: HISTORY back to where you were,
		 * SEARCH to the search screen */
		calc_leave();
		if (code == BTN_SEARCH)
			goto_search();
		return;
	}
	switch (code) {
	case BTN_SEARCH:
		goto_search();
		break;
	case BTN_HISTORY:
		/* short press = back, held = history list (see tick and
		 * button_up) */
		A.hist_down = 1;
		A.hist_down_ms = plat_ms();
		break;
	case BTN_RANDOM:
		/* short press = random article (or leave the calculator),
		 * held = calculator (see tick and the button up event) */
		A.rnd_down = 1;
		A.rnd_down_ms = plat_ms();
		break;
	case BTN_POWER:
		save_all();
		plat_power_off();
		break;
	}
}

static void touch_down(int x, int y)
{
	A.touching = 1;
	A.dragging = 0;
	A.tx0 = A.tx = x;
	A.ty0 = A.ty = A.ty_prev = y;
	A.t_prev = A.t_last = plat_ms();
	A.scroll0 = A.scroll;
	A.list_acc = 0;
	A.vel = 0;
	A.pressed_row = -1;
	A.pressed_key = -1;
	switch (A.screen) {
	case S_CALC:
		A.pressed_key = calc_key_at(x, y);
		A.need_draw = 1;
		break;
	case S_SEARCH:
		if (A.kb_visible && y >= KB_Y) {
			A.pressed_key = key_at(x, y);
			A.key_down_ms = A.key_repeat_ms = plat_ms();
			if (A.pressed_key >= 0 && keys[A.pressed_key].code == K_BS) {
				query_backspace();	/* act at once, repeat while held */
				A.key_repeat_ms += 350;
			}
		} else if (y > BAR_H && A.qlen) {
			int r = (y - BAR_H - 1) / ROW_H;
			if (r < A.nrows && r < visible_rows())
				A.pressed_row = r;
		}
		A.need_draw = 1;
		break;
	case S_HISTORY:
		if (y > BAR_H) {
			int r = (y - BAR_H) / ROW_H;
			if (A.hist_top + r < A.nhist)
				A.pressed_row = r;
			A.need_draw = 1;
		}
		break;
	case S_SECTIONS:
		if (y > BAR_H) {
			int r = (y - BAR_H) / ROW_H;
			if (A.sect_top + r < A.nsect)
				A.pressed_row = r;
			A.need_draw = 1;
		}
		break;
	case S_LIBRARY:
		if (y > BAR_H) {
			int r = (y - BAR_H) / ROW_H;
			if (A.lib_top + r < A.nlibs)
				A.pressed_row = r;
			A.need_draw = 1;
		}
		break;
	}
}

static void list_drag(int dy, int *top, int total, int rows)
{
	A.list_acc += dy;
	while (A.list_acc >= ROW_H) {
		A.list_acc -= ROW_H;
		if (*top > 0)
			(*top)--;
	}
	while (A.list_acc <= -ROW_H) {
		A.list_acc += ROW_H;
		if (*top + rows < total)
			(*top)++;
	}
	A.need_draw = 1;
}

static void touch_move(int x, int y)
{
	int dy = y - A.ty;
	uint32_t now = plat_ms();
	if (!A.touching)
		return;
	if (!A.dragging && (y - A.ty0 > DRAG_START || A.ty0 - y > DRAG_START ||
			    x - A.tx0 > DRAG_START * 2 || A.tx0 - x > DRAG_START * 2)) {
		if ((A.screen == S_SEARCH || A.screen == S_CALC) && A.pressed_key >= 0) {
			/* sliding on the keyboard: just track the key */
		} else {
			A.dragging = 1;
			A.pressed_row = -1;
			A.need_draw = 1;
		}
	}
	A.t_prev = A.t_last;
	A.ty_prev = A.ty;
	A.t_last = now;
	A.tx = x;
	A.ty = y;
	if (A.screen == S_CALC) {
		int k = calc_key_at(x, y);
		if (k != A.pressed_key) {
			A.pressed_key = k;
			A.need_draw = 1;
		}
		return;
	}
	if (A.screen == S_SEARCH && A.pressed_key >= 0) {
		int k = y >= KB_Y ? key_at(x, y) : -1;
		if (k != A.pressed_key) {
			A.pressed_key = k;
			A.key_down_ms = A.key_repeat_ms = now + (k >= 0 && keys[k].code == K_BS ? 350 : 0);
			A.need_draw = 1;
		}
		return;
	}
	if (!A.dragging)
		return;
	switch (A.screen) {
	case S_ARTICLE: {
		int32_t s = A.scroll0 - (y - A.ty0);
		if (s < 0)
			s = 0;
		if (s > max_scroll())
			s = max_scroll();
		if (s != A.scroll) {
			A.scroll = s;
			A.show_bar = 1;
			A.toolbar = 0;
			A.need_draw = 1;
		}
		break;
	}
	case S_SEARCH:
		if (A.qlen) {
			/* dragging the results puts the keyboard away (there is
			 * no hide key); tap the search bar to bring it back */
			if (A.kb_visible) {
				A.kb_visible = 0;
				fetch_rows();
				A.need_draw = 1;
			}
			A.list_acc += dy;
			while (A.list_acc >= ROW_H) {
				A.list_acc -= ROW_H;
				scroll_results(-1);
			}
			while (A.list_acc <= -ROW_H) {
				A.list_acc += ROW_H;
				scroll_results(1);
			}
		}
		break;
	case S_HISTORY:
		list_drag(dy, &A.hist_top, A.nhist, list_rows());
		break;
	case S_LIBRARY:
		list_drag(dy, &A.lib_top, A.nlibs, list_rows());
		break;
	case S_SECTIONS:
		list_drag(dy, &A.sect_top, A.nsect, list_rows());
		break;
	}
}

static void touch_up(int x, int y)
{
	int was_drag = A.dragging, row = A.pressed_row, key = A.pressed_key;
	if (!A.touching)
		return;
	A.touching = 0;
	A.dragging = 0;
	A.pressed_row = -1;
	A.pressed_key = -1;
	A.need_draw = 1;

	switch (A.screen) {
	case S_CALC:
		if (key >= 0) {
			const calc_key_t *k = &calc_lay()->keys[key];
			calc_press(A.calc_2nd && k->label2 ? k->code2 : k->code);
		} else if (!was_drag) {
			calc_tab_tap(x, y);
		}
		return;
	case S_SEARCH:
		if (key >= 0) {
			if (keys[key].code != K_BS)
				key_press(keys[key].code);
			return;
		}
		if (was_drag)
			return;
		if (!A.qlen && A.opt_y && y >= A.opt_y - 12 && y <= A.opt_y + 5) {
			/* a switch: applies to the next article opened */
			A.opts ^= x < SCREEN_W / 2 ? HTML_NOTES : HTML_CITES;
			html_set_options(A.opts);
			A.state_dirty = 1;
			A.need_draw = 1;
			return;
		}
		if (!A.qlen && A.opt_y && y > A.opt_y + 5 && y <= A.opt_y + OPT2_DY + 7) {
			/* Text size: the open page too, at once */
			A.opts ^= HTML_LARGE;
			html_set_options(A.opts);
			relayout_article((A.opts & HTML_LARGE) != 0);
			A.state_dirty = 1;
			A.need_draw = 1;
			return;
		}
		if (y < BAR_H) {
			if (!A.qlen && A.nlibs > 1) {
				/* the bar shows the wiki's name: tap it to switch */
				int k;
				A.lib_top = 0;
				for (k = 0; k < A.nlibs; ++k)
					if (!strcmp(A.libs[k].key, A.lib.info.key))
						A.lib_top = k > 2 ? k - 2 : 0;
				/* never scroll past the end: a part-empty list looks
				 * like there are fewer wikis */
				if (A.lib_top > A.nlibs - list_rows())
					A.lib_top = A.nlibs > list_rows() ? A.nlibs - list_rows() : 0;
				A.screen = S_LIBRARY;
				A.need_draw = 1;
			} else
				A.kb_visible = 1;
		} else if (row >= 0 && row < A.nrows) {
			if (A.search_dirty)
				run_search();
			if (row < A.nrows)
				open_entry(A.rows[row].entry, 0, 1);
		}
		return;
	case S_ARTICLE:
		if (was_drag) {
			/* fling: velocity from the last two samples */
			int32_t dt = (int32_t)(A.t_last - A.t_prev);
			A.show_bar = 1;
			if (dt > 0 && dt < 120 && plat_ms() - A.t_last < 80) {
				A.vel = (int32_t)(A.ty_prev - A.ty) * 16 * 20 / dt;
				if (A.vel > 16 * 60)
					A.vel = 16 * 60;
				if (A.vel < -16 * 60)
					A.vel = -16 * 60;
				A.anim_ms = plat_ms();
			}
			A.state_dirty = 1;
			if (!A.vel)
				A.show_bar = 0;
		} else {
			article_tap(x, y);
		}
		return;
	case S_HISTORY:
		if (was_drag)
			return;
		if (y < BAR_H && x > SCREEN_W - 60 && A.nhist) {
			A.nhist = 0;
			A.hist_dirty = 1;
		} else if (row >= 0 && A.hist_top + row < A.nhist) {
			hist_item_t h = A.hist[A.hist_top + row];
			open_entry(h.entry, h.scroll, 1);
		}
		return;
	case S_SECTIONS:
		if (!was_drag && row >= 0 && A.sect_top + row < A.nsect) {
			/* jump; Back returns to where you were (on the stack) */
			A.scroll = A.sect[A.sect_top + row].y;
			if (A.scroll > max_scroll())
				A.scroll = max_scroll();
			A.screen = S_ARTICLE;
			A.state_dirty = 1;
		}
		return;
	case S_LIBRARY:
		if (!was_drag && row >= 0 && A.lib_top + row < A.nlibs) {
			if (open_library(A.lib_top + row) == 0) {
				A.screen = S_SEARCH;
				A.kb_visible = 1;
				A.state_dirty = 1;
			}
		}
		return;
	case S_MESSAGE:
		if (A.msg_return >= 0) {
			A.screen = A.msg_return;
			if (A.screen == S_ARTICLE && !A.have_article)
				A.screen = S_SEARCH;
		}
		return;
	}
}

void app_event(const void *ev)
{
	const plat_event_t *e = ev;
	A.last_input = plat_ms();
	switch (e->type) {
	case PE_TOUCH_DOWN:
		touch_down(e->x, e->y);
		break;
	case PE_TOUCH_MOVE:
		touch_move(e->x, e->y);
		break;
	case PE_TOUCH_UP:
		if (e->x != A.tx || e->y != A.ty)
			touch_move(e->x, e->y);
		touch_up(e->x, e->y);
		break;
	case PE_BUTTON_DOWN: {
		uint32_t now = plat_ms();
		int c = e->code & 3;
		if (e->code == BTN_HISTORY && A.hist_up_pending) {
			A.hist_up_pending = 0;	/* bounce: still the same press */
			break;
		}
		if (e->code == BTN_RANDOM && A.rnd_up_pending) {
			A.rnd_up_pending = 0;	/* bounce: still the same press */
			break;
		}
		if (A.btn[c].seen_up && now - A.btn[c].up_ms < BOUNCE_MS) {
			A.btn[c].ignore = 1;
			break;
		}
		button(e->code);
		break;
	}
	case PE_BUTTON_UP: {
		int c = e->code & 3;
		A.btn[c].seen_up = 1;
		A.btn[c].up_ms = plat_ms();
		if (A.btn[c].ignore) {
			A.btn[c].ignore = 0;
			break;
		}
		if (e->code == BTN_HISTORY && A.hist_down && !A.hist_up_pending) {
			A.hist_up_pending = 1;	/* act once it has stayed up */
			A.hist_up_ms = A.btn[c].up_ms;
		}
		if (e->code == BTN_RANDOM && A.rnd_down && !A.rnd_up_pending) {
			A.rnd_up_pending = 1;
			A.rnd_up_ms = A.btn[c].up_ms;
		}
		break;
	}
	case PE_KEY:
		if (A.screen == S_CALC) {
			/* PC keyboard (simulator, scripted tests) */
			int k = e->code;
			if (k == '\n' || k == '\r' || k == '=')
				calc_press(CK_EQ);
			else if (k == 8)
				calc_press(CK_DEL);
			else if (k == 'c' || k == 27)
				calc_press(CK_AC);
			else if (k == 'n')
				calc_press(CK_NEG);
			else if (k == 'a')
				calc_press('A');
			else if (k == 'x')
				calc_press('*');
			else if (k == 'D')
				calc_press(CK_DEG);
			else if (k == 'X')
				calc_press(CK_EXPE);
			else if (k == 'Y')
				calc_press(CK_TENX);
			else if (k == 'q')
				calc_press(CK_SQ);
			else if (k == 'Q')
				calc_press(CK_CUBE);
			else if (k == 'i')
				calc_press(CK_RECIP);
			else if (k == 'M')
				A.calc_mode = !A.calc_mode;
			else if (k && strchr("0123456789.+-*/%()^!pesotSOTlgrE", k))
				calc_press(k);
			A.need_draw = 1;
			break;
		}
		/* host keyboard shortcut for typing */
		if (A.screen != S_SEARCH && A.have_lib && e->code >= 32)
			goto_search();
		if (A.screen == S_SEARCH) {
			if (e->code == 8)
				query_backspace();
			else if (e->code == '\n' || e->code == '\r')
				key_press(K_GO);
			else if (e->code >= 32 && e->code < 127)
				query_add(e->code >= 'A' && e->code <= 'Z' ? e->code + 32 : e->code);
			A.need_draw = 1;
		} else if (A.screen == S_ARTICLE && e->code == 8) {
			go_back();
		}
		break;
	case PE_QUIT:
		save_all();
		A.quit = 1;
		break;
	case PE_IDLE_TIMEOUT:
		save_all();
		break;
	}
}

/* ------------------------------------------------------------------ */
/* time based work: momentum, key repeat, saving, prefetch */

/* a button was released less than BOUNCE_MS ago.  The app must stay awake
 * through that window: asleep, grifo stops the timer behind plat_ms, so
 * the next press (seconds later) would look like contact bounce of the
 * last release and be ignored */
static int button_settling(void)
{
	uint32_t now = plat_ms();
	int c;
	for (c = 0; c < 4; ++c)
		if (A.btn[c].seen_up && now - A.btn[c].up_ms < BOUNCE_MS)
			return 1;
	return 0;
}

static int animating(void)
{
	return A.vel != 0 || A.hist_down || A.rnd_down || button_settling() || (A.touching && A.pressed_key >= 0 &&
			      keys[A.pressed_key].code == K_BS && A.screen == S_SEARCH);
}

static void tick(void)
{
	uint32_t now = plat_ms();
	if (A.hist_up_pending && now - A.hist_up_ms >= BOUNCE_MS) {
		A.hist_up_pending = 0;
		A.hist_down = 0;
		go_back();
	} else if (A.hist_down && !A.hist_up_pending && now - A.hist_down_ms >= HOLD_MS) {
		A.hist_down = 0;	/* held: the list opens while still pressed */
		show_history();
	}
	if (A.rnd_up_pending && now - A.rnd_up_ms >= BOUNCE_MS) {
		A.rnd_up_pending = 0;
		A.rnd_down = 0;
		if (A.screen == S_CALC)
			calc_leave();
		else
			random_article();
	} else if (A.rnd_down && !A.rnd_up_pending && now - A.rnd_down_ms >= HOLD_MS) {
		A.rnd_down = 0;		/* held: in or out of the calculator */
		if (A.screen == S_CALC)
			calc_leave();
		else
			calc_enter();
	}
	if (A.vel && A.screen == S_ARTICLE) {
		while ((int32_t)(now - A.anim_ms) >= 20 && A.vel) {
			int32_t s = A.scroll + A.vel / 16;
			A.anim_ms += 20;
			if (s <= 0 || s >= max_scroll()) {
				s = s <= 0 ? 0 : max_scroll();
				A.vel = 0;
			}
			A.scroll = s;
			A.vel = A.vel * 15 / 16;
			if (A.vel > -12 && A.vel < 12)
				A.vel = 0;
			A.need_draw = 1;
		}
		if (!A.vel)
			A.show_bar = 0, A.need_draw = 1;
	} else {
		A.vel = 0;
	}
	if (A.touching && A.screen == S_SEARCH && A.pressed_key >= 0 &&
	    keys[A.pressed_key].code == K_BS && (int32_t)(now - A.key_repeat_ms) >= 0) {
		query_backspace();
		A.key_repeat_ms = now + 90;
		A.need_draw = 1;
	}
}

/* returns 1 if it did something */
static int idle_work(void)
{
	if (A.search_dirty && A.screen == S_SEARCH) {
		run_search();
		return 1;
	}
	if (A.need_draw) {
		draw();
		return 1;
	}
	if (A.screen == S_ARTICLE && A.have_article && !A.touching) {
		uint8_t before = 0;
		(void)before;
		lay_prefetch(&A.lay, A.scroll, SCREEN_H);
	}
	if (A.have_article && !A.lay.done && !A.touching && !A.vel) {
		/* the rest of a long page, a slice at a time so a touch is
		 * never kept waiting for long */
		lay_more(&A.lay, -1, 16);
		if (A.show_bar && A.screen == S_ARTICLE)
			A.need_draw = 1;
		return 1;
	}
	if ((A.state_dirty || A.hist_dirty || (A.have_lib && A.lib.ext_dirty)) &&
	    !A.touching && !A.vel && plat_ms() - A.last_input > 2000) {
		hist_update_scroll();
		save_all();
		return 1;
	}
	return 0;
}

int app_awake(void)
{
	/* the same tests as app_main before it calls plat_wait */
	if (animating() || A.need_draw || A.search_dirty)
		return 1;
	if (A.have_article && !A.lay.done && !A.touching && !A.vel)
		return 1;
	if ((A.state_dirty || A.hist_dirty || (A.have_lib && A.lib.ext_dirty)) &&
	    A.have_lib && !A.touching)
		return 1;
	return 0;
}

void app_settle(void)
{
	int guard = 0;
	while (guard++ < 100000) {
		tick();
		if (A.vel && A.screen == S_ARTICLE)
			continue;	/* tests run momentum to completion */
		if (!idle_work())
			break;
	}
}

/* ------------------------------------------------------------------ */
/* start up */

static void restore_state(void)
{
	uint8_t *buf;
	uint32_t len;
	int i, want = -1;
	char name[9] = { 0 };
	if (plat_load("ZWSTATE.DAT", &buf, &len) == 0) {
		if (len >= 212 && zw_rd32(buf) == STATE_MAGIC && !memcmp(buf + 200, "CALC", 4)) {
			double v;
			memcpy(&v, buf + 204, sizeof(double));
			if (v == v && v - v == 0)	/* not NaN or infinity */
				A.calc_ans = v;
			A.calc_mode = buf[212] == 1;
			A.calc_rad = buf[213] == 1;
		}
		if (len >= 25 && zw_rd32(buf) == STATE_MAGIC && (buf[14] & 0x80)) {
			A.opts = buf[14] & (HTML_NOTES | HTML_CITES | HTML_LARGE);
			html_set_options(A.opts);
			lay_set_large((A.opts & HTML_LARGE) != 0);
		}
		if (len >= 25 && zw_rd32(buf) == STATE_MAGIC) {
			memcpy(name, buf + 4, 8);
			for (i = 0; i < A.nlibs; ++i)
				if (strcmp(A.libs[i].key, name) == 0)
					want = i;
		}
		if (want < 0 && A.nlibs > 1) {
			zw_free(buf);
			A.screen = S_LIBRARY;
			A.need_draw = 1;
			return;
		}
		if (want < 0)
			want = 0;
		if (open_library(want) == 0) {
			uint32_t entry = zw_rd32(buf + 16);
			int32_t scroll = (int32_t)zw_rd32(buf + 20);
			uint8_t ql = buf[24];
			if (ql <= QUERY_MAX && 25u + ql <= len) {
				memcpy(A.query, buf + 25, ql);
				A.query[ql] = 0;
				A.qlen = ql;
			}
			A.screen = S_SEARCH;
			A.kb_visible = 1;
			A.search_dirty = 1;
			if (buf[13] == S_ARTICLE && entry != 0xffffffffu &&
			    entry < A.lib.zim.entry_count)
				open_entry(entry, scroll, 0);
		}
		zw_free(buf);
	} else if (A.nlibs == 1) {
		if (open_library(0) == 0) {
			A.screen = S_SEARCH;
			A.kb_visible = 1;
		}
	} else {
		A.screen = S_LIBRARY;
	}
	A.need_draw = 1;
}

static int load_fallback_font(void)
{
	uint8_t *font;
	uint32_t flen;
	int r;
	A.font_loading = 1;
	A.spin_t = 0;
	r = plat_load("WIKIFONT.BIN", &font, &flen);
	A.font_loading = 0;
	if (r < 0)
		return -1;
	if (font_set_fallback(font, flen) < 0) {
		zw_free(font);
		return -1;
	}
	return 0;
}

void app_init(void)
{
	zw_progress = open_progress;
	int r;
	uint8_t *font;
	uint32_t flen;

	memset(&A, 0, sizeof(A));
	A.pressed_row = A.pressed_key = -1;
	A.seed = plat_ms() ^ 0x9e3779b9u;
	A.kb_visible = 1;

	memset(A.fb, 0, sizeof(A.fb));
	text_centre(SCREEN_W / 2, 100, FONT_TITLE, "WikiReader", BLACK);
	text_centre(SCREEN_W / 2, 122, FONT_SMALL, "zimwiki", BLACK);
	present();

	if ((r = lib_mount(&A.vol)) < 0) {
		const char *lines[] = { "Insert a FAT formatted", "microSD card and restart.", NULL };
		set_message("No card", lines, -1);
		return;
	}
	A.mounted = 1;
	/* optional big fallback font (CJK and more): loaded when first needed */
	font_set_fallback_loader(load_fallback_font);
	(void)font;
	(void)flen;
	A.nlibs = lib_scan(&A.vol, A.libs, LIB_MAX);
	if (A.nlibs <= 0) {
		const char *lines[] = { "Put a Kiwix .zim on the card,", "or run zimprep on a PC and",
					"copy NAME.IDX and NAME.Z00...", "to the top folder or to a",
					"folder of their own.", NULL };
		A.nlibs = 0;
		set_message("No wiki found", lines, -1);
		return;
	}
	restore_state();
}

int app_main(void)
{
	plat_event_t e;
	app_init();
	while (!A.quit) {
		int got;
		tick();
		got = plat_poll(&e);
		if (!got) {
			if (idle_work())
				continue;
			if (animating())
				continue;	/* keep polling while something moves */
			/* like the original firmware: stay awake until pending
			 * state has been saved (sleep would postpone it) */
			if ((A.state_dirty || A.hist_dirty || (A.have_lib && A.lib.ext_dirty)) &&
			    A.have_lib && !A.touching)
				continue;
			got = plat_wait(&e);
			if (!got)
				continue;
		}
		/* coalesce queued finger movement: only the latest matters */
		if (e.type == PE_TOUCH_MOVE) {
			plat_event_t n;
			while (plat_poll(&n)) {
				if (n.type != PE_TOUCH_MOVE) {
					app_event(&e);
					e = n;
					break;
				}
				e = n;
			}
		}
		app_event(&e);
		if (A.need_draw && !A.search_dirty)
			draw();
	}
	if (A.have_lib)
		lib_close(&A.lib);
	return 0;
}

void app_describe(char *out, int cap)
{
	static const char *const names[] = { "message", "search", "article", "history", "library" };
	char tmp[400];
	int n = 0;
	const char *s = names[A.screen];
	tmp[0] = 0;
	switch (A.screen) {
	case S_ARTICLE:
		n = snprintf(tmp, sizeof(tmp), "article \"%s\" scroll=%d/%d back=%d", A.title,
			     (int)A.scroll, (int)max_scroll(), A.nback);
		break;
	case S_SEARCH:
		n = snprintf(tmp, sizeof(tmp), "search \"%s\" rows=%d first=\"%s\"", A.query, A.nrows,
			     A.nrows ? A.rows[0].title : "");
		break;
	case S_HISTORY:
		n = snprintf(tmp, sizeof(tmp), "history n=%d first=\"%s\"", A.nhist,
			     A.nhist ? A.hist[0].title : "");
		break;
	case S_SECTIONS:
		n = snprintf(tmp, sizeof(tmp), "sections n=%d top=%d", A.nsect, A.sect_top);
		break;
	case S_CALC: {
		char r[48];
		double v;
		if (A.calc_err)
			strcpy(r, calc_error_text(A.calc_err));
		else if (A.calc_done)
			calc_format(A.calc_ans, r);
		else if (calc_eval(A.calc_expr, A.calc_ans, calc_flags(), &v) == CALC_OK)
			calc_format(v, r);
		else
			strcpy(r, "-");
		n = snprintf(tmp, sizeof(tmp), "calc \"%s\" result=%s%s%s%s%s", A.calc_expr, r,
			     A.calc_done ? " done" : "", A.calc_mode ? " sci" : "",
			     A.calc_rad ? " rad" : "", A.calc_2nd ? " 2nd" : "");
		break;
	}
	case S_MESSAGE:
		n = snprintf(tmp, sizeof(tmp), "message \"%s\" \"%s\"", A.msg_title, A.nmsg ? A.msg[0] : "");
		break;
	default:
		n = snprintf(tmp, sizeof(tmp), "%s", s);
	}
	(void)n;
	strncpy(out, tmp, (size_t)cap - 1);
	out[cap - 1] = 0;
}
