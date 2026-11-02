/*
 * zimwiki - TeX maths to linear text (see tex.h)
 *
 * Not a TeX implementation: a forgiving reader for the subset Wikipedia
 * uses, which never fails.  Anything it does not know is shown as its
 * name, so nothing is silently lost.
 */
#include <string.h>
#include "tex.h"

enum { K_ATOM, K_OP, K_BIG, K_OPEN, K_CLOSE };
enum { P_START, P_OPEN, P_OP, P_ATOM };

typedef struct {
	const char *s;
	uint32_t i;
	tex_out_fn out;
	void *ctx;
	char buf[128];
	uint32_t blen;
	int bstyle;
	int prev;		/* what was last written at baseline level */
	int pend_space;		/* after "sin", "∑": space before the next atom */
	int env[8], nenv;
	int depth;
} tex_t;

static const struct sym {
	const char *name, *u;
	uint8_t kind;
} syms[] = {
	/* Greek */
	{"alpha", "\xce\xb1", K_ATOM}, {"beta", "\xce\xb2", K_ATOM}, {"gamma", "\xce\xb3", K_ATOM},
	{"delta", "\xce\xb4", K_ATOM}, {"epsilon", "\xce\xb5", K_ATOM}, {"varepsilon", "\xce\xb5", K_ATOM},
	{"zeta", "\xce\xb6", K_ATOM}, {"eta", "\xce\xb7", K_ATOM}, {"theta", "\xce\xb8", K_ATOM},
	{"vartheta", "\xcf\x91", K_ATOM}, {"iota", "\xce\xb9", K_ATOM}, {"kappa", "\xce\xba", K_ATOM},
	{"lambda", "\xce\xbb", K_ATOM}, {"mu", "\xce\xbc", K_ATOM}, {"nu", "\xce\xbd", K_ATOM},
	{"xi", "\xce\xbe", K_ATOM}, {"omicron", "\xce\xbf", K_ATOM}, {"pi", "\xcf\x80", K_ATOM},
	{"varpi", "\xcf\x96", K_ATOM}, {"rho", "\xcf\x81", K_ATOM}, {"varrho", "\xcf\xb1", K_ATOM},
	{"sigma", "\xcf\x83", K_ATOM}, {"varsigma", "\xcf\x82", K_ATOM}, {"tau", "\xcf\x84", K_ATOM},
	{"upsilon", "\xcf\x85", K_ATOM}, {"phi", "\xcf\x86", K_ATOM}, {"varphi", "\xcf\x86", K_ATOM},
	{"chi", "\xcf\x87", K_ATOM}, {"psi", "\xcf\x88", K_ATOM}, {"omega", "\xcf\x89", K_ATOM},
	{"Gamma", "\xce\x93", K_ATOM}, {"Delta", "\xce\x94", K_ATOM}, {"Theta", "\xce\x98", K_ATOM},
	{"Lambda", "\xce\x9b", K_ATOM}, {"Xi", "\xce\x9e", K_ATOM}, {"Pi", "\xce\xa0", K_ATOM},
	{"Sigma", "\xce\xa3", K_ATOM}, {"Upsilon", "\xce\xa5", K_ATOM}, {"Phi", "\xce\xa6", K_ATOM},
	{"Psi", "\xce\xa8", K_ATOM}, {"Omega", "\xce\xa9", K_ATOM},
	/* big operators */
	{"sum", "\xe2\x88\x91", K_BIG}, {"prod", "\xe2\x88\x8f", K_BIG}, {"coprod", "\xe2\x88\x90", K_BIG},
	{"int", "\xe2\x88\xab", K_BIG}, {"iint", "\xe2\x88\xac", K_BIG}, {"iiint", "\xe2\x88\xad", K_BIG},
	{"oint", "\xe2\x88\xae", K_BIG}, {"bigcup", "\xe2\x8b\x83", K_BIG}, {"bigcap", "\xe2\x8b\x82", K_BIG},
	{"bigoplus", "\xe2\x8a\x95", K_BIG}, {"bigotimes", "\xe2\x8a\x97", K_BIG},
	{"bigvee", "\xe2\x8b\x81", K_BIG}, {"bigwedge", "\xe2\x8b\x80", K_BIG},
	{"bigsqcup", "\xe2\x8a\x94", K_BIG},
	/* relations and binary operators (spaced) */
	{"leq", "\xe2\x89\xa4", K_OP}, {"le", "\xe2\x89\xa4", K_OP}, {"leqslant", "\xe2\x89\xa4", K_OP},
	{"geq", "\xe2\x89\xa5", K_OP}, {"ge", "\xe2\x89\xa5", K_OP}, {"geqslant", "\xe2\x89\xa5", K_OP},
	{"neq", "\xe2\x89\xa0", K_OP}, {"ne", "\xe2\x89\xa0", K_OP}, {"approx", "\xe2\x89\x88", K_OP},
	{"equiv", "\xe2\x89\xa1", K_OP}, {"sim", "\xe2\x88\xbc", K_OP}, {"simeq", "\xe2\x89\x83", K_OP},
	{"cong", "\xe2\x89\x85", K_OP}, {"propto", "\xe2\x88\x9d", K_OP}, {"asymp", "\xe2\x89\x8d", K_OP},
	{"ll", "\xe2\x89\xaa", K_OP}, {"gg", "\xe2\x89\xab", K_OP}, {"prec", "\xe2\x89\xba", K_OP},
	{"succ", "\xe2\x89\xbb", K_OP}, {"preceq", "\xe2\xaa\xaf", K_OP}, {"succeq", "\xe2\xaa\xb0", K_OP},
	{"to", "\xe2\x86\x92", K_OP}, {"rightarrow", "\xe2\x86\x92", K_OP}, {"longrightarrow", "\xe2\x86\x92", K_OP},
	{"leftarrow", "\xe2\x86\x90", K_OP}, {"gets", "\xe2\x86\x90", K_OP}, {"longleftarrow", "\xe2\x86\x90", K_OP},
	{"leftrightarrow", "\xe2\x86\x94", K_OP}, {"Rightarrow", "\xe2\x87\x92", K_OP},
	{"Longrightarrow", "\xe2\x87\x92", K_OP}, {"implies", "\xe2\x87\x92", K_OP},
	{"Leftarrow", "\xe2\x87\x90", K_OP}, {"Longleftarrow", "\xe2\x87\x90", K_OP}, {"impliedby", "\xe2\x87\x90", K_OP},
	{"Leftrightarrow", "\xe2\x87\x94", K_OP}, {"Longleftrightarrow", "\xe2\x87\x94", K_OP},
	{"iff", "\xe2\x87\x94", K_OP}, {"mapsto", "\xe2\x86\xa6", K_OP}, {"longmapsto", "\xe2\x86\xa6", K_OP},
	{"hookrightarrow", "\xe2\x86\xaa", K_OP}, {"uparrow", "\xe2\x86\x91", K_ATOM}, {"downarrow", "\xe2\x86\x93", K_ATOM},
	{"in", "\xe2\x88\x88", K_OP}, {"notin", "\xe2\x88\x89", K_OP}, {"ni", "\xe2\x88\x8b", K_OP},
	{"subset", "\xe2\x8a\x82", K_OP}, {"subseteq", "\xe2\x8a\x86", K_OP}, {"subsetneq", "\xe2\x8a\x8a", K_OP},
	{"supset", "\xe2\x8a\x83", K_OP}, {"supseteq", "\xe2\x8a\x87", K_OP},
	{"cup", "\xe2\x88\xaa", K_OP}, {"cap", "\xe2\x88\xa9", K_OP}, {"setminus", "\xe2\x88\x96", K_OP},
	{"wedge", "\xe2\x88\xa7", K_OP}, {"land", "\xe2\x88\xa7", K_OP}, {"vee", "\xe2\x88\xa8", K_OP},
	{"lor", "\xe2\x88\xa8", K_OP}, {"oplus", "\xe2\x8a\x95", K_OP}, {"otimes", "\xe2\x8a\x97", K_OP},
	{"ominus", "\xe2\x8a\x96", K_OP}, {"odot", "\xe2\x8a\x99", K_OP}, {"times", "\xc3\x97", K_OP},
	{"div", "\xc3\xb7", K_OP}, {"pm", "\xc2\xb1", K_OP}, {"mp", "\xe2\x88\x93", K_OP},
	{"cdot", "\xc2\xb7", K_OP}, {"ast", "\xe2\x88\x97", K_OP}, {"star", "\xe2\x8b\x86", K_OP},
	{"circ", "\xe2\x88\x98", K_ATOM}, {"bullet", "\xe2\x80\xa2", K_OP}, {"mid", "|", K_OP},
	{"parallel", "\xe2\x88\xa5", K_OP}, {"perp", "\xe2\x8a\xa5", K_OP}, {"models", "\xe2\x8a\xa8", K_OP},
	{"vdash", "\xe2\x8a\xa2", K_OP}, {"triangleq", "\xe2\x89\x9c", K_OP}, {"coloneqq", ":=", K_OP},
	{"sqcup", "\xe2\x8a\x94", K_OP}, {"sqcap", "\xe2\x8a\x93", K_OP}, {"wr", "\xe2\x89\x80", K_OP},
	{"lhd", "\xe2\x8a\xb2", K_OP}, {"rhd", "\xe2\x8a\xb3", K_OP}, {"triangleleft", "\xe2\x8a\xb2", K_OP},
	{"trianglelefteq", "\xe2\x8a\xb4", K_OP}, {"unlhd", "\xe2\x8a\xb4", K_OP},
	/* others */
	{"infty", "\xe2\x88\x9e", K_ATOM}, {"partial", "\xe2\x88\x82", K_ATOM}, {"nabla", "\xe2\x88\x87", K_ATOM},
	{"forall", "\xe2\x88\x80", K_ATOM}, {"exists", "\xe2\x88\x83", K_ATOM}, {"nexists", "\xe2\x88\x84", K_ATOM},
	{"emptyset", "\xe2\x88\x85", K_ATOM}, {"varnothing", "\xe2\x88\x85", K_ATOM},
	{"neg", "\xc2\xac", K_OPEN}, {"lnot", "\xc2\xac", K_OPEN},
	{"aleph", "\xe2\x84\xb5", K_ATOM}, {"hbar", "\xe2\x84\x8f", K_ATOM}, {"ell", "\xe2\x84\x93", K_ATOM},
	{"wp", "\xe2\x84\x98", K_ATOM}, {"Re", "\xe2\x84\x9c", K_ATOM}, {"Im", "\xe2\x84\x91", K_ATOM},
	{"prime", "\xe2\x80\xb2", K_ATOM}, {"angle", "\xe2\x88\xa0", K_ATOM}, {"triangle", "\xe2\x96\xb3", K_ATOM},
	{"square", "\xe2\x96\xa1", K_ATOM}, {"Box", "\xe2\x96\xa1", K_ATOM}, {"blacksquare", "\xe2\x96\xa0", K_ATOM},
	{"top", "\xe2\x8a\xa4", K_ATOM}, {"bot", "\xe2\x8a\xa5", K_ATOM}, {"surd", "\xe2\x88\x9a", K_ATOM},
	{"ldots", "\xe2\x80\xa6", K_ATOM}, {"dots", "\xe2\x80\xa6", K_ATOM}, {"dotsc", "\xe2\x80\xa6", K_ATOM},
	{"cdots", "\xe2\x8b\xaf", K_ATOM}, {"dotsb", "\xe2\x8b\xaf", K_ATOM}, {"dotsm", "\xe2\x8b\xaf", K_ATOM},
	{"vdots", "\xe2\x8b\xae", K_ATOM}, {"ddots", "\xe2\x8b\xb1", K_ATOM}, {"deg", "\xc2\xb0", K_ATOM},
	{"langle", "\xe2\x9f\xa8", K_OPEN}, {"rangle", "\xe2\x9f\xa9", K_CLOSE},
	{"lfloor", "\xe2\x8c\x8a", K_OPEN}, {"rfloor", "\xe2\x8c\x8b", K_CLOSE},
	{"lceil", "\xe2\x8c\x88", K_OPEN}, {"rceil", "\xe2\x8c\x89", K_CLOSE},
	{"lvert", "|", K_OPEN}, {"rvert", "|", K_CLOSE}, {"vert", "|", K_ATOM},
	{"lVert", "\xe2\x80\x96", K_OPEN}, {"rVert", "\xe2\x80\x96", K_CLOSE}, {"Vert", "\xe2\x80\x96", K_ATOM},
	{"|", "\xe2\x80\x96", K_ATOM}, {"{", "{", K_OPEN}, {"}", "}", K_CLOSE}, {"%", "%", K_ATOM},
	{"$", "$", K_ATOM}, {"&", "&", K_ATOM}, {"#", "#", K_ATOM}, {"_", "_", K_ATOM},
	{"backslash", "\\", K_ATOM}, {"colon", ":", K_OP}, {"lbrace", "{", K_OPEN}, {"rbrace", "}", K_CLOSE},
	{"lbrack", "[", K_OPEN}, {"rbrack", "]", K_CLOSE}, {"S", "\xc2\xa7", K_ATOM},
	{"checkmark", "\xe2\x9c\x93", K_ATOM}, {"dagger", "\xe2\x80\xa0", K_ATOM},
	{"flat", "\xe2\x99\xad", K_ATOM}, {"sharp", "\xe2\x99\xaf", K_ATOM}, {"natural", "\xe2\x99\xae", K_ATOM},
	{NULL, NULL, 0}
};

/* upright operator names; the value is what is shown */
static const char *const funcs[] = {
	"lim", "sup", "inf", "max", "min", "sin", "cos", "tan", "cot", "sec", "csc",
	"sinh", "cosh", "tanh", "coth", "arcsin", "arccos", "arctan", "log", "ln", "lg",
	"exp", "det", "gcd", "dim", "ker", "arg", "Pr", "hom", "deg", "injlim", "projlim",
	NULL
};

/* ------------------------------------------------------------------ */
/* output */

static void flush(tex_t *t)
{
	if (t->blen)
		t->out(t->ctx, t->buf, t->blen, t->bstyle);
	t->blen = 0;
}

static void put(tex_t *t, const char *u, uint32_t n, int style)
{
	if (t->blen && (style != t->bstyle || t->blen + n > sizeof(t->buf)))
		flush(t);
	if (n > sizeof(t->buf)) {
		t->out(t->ctx, u, n, style);
		return;
	}
	t->bstyle = style;
	memcpy(t->buf + t->blen, u, n);
	t->blen += n;
}

static void puts_(tex_t *t, const char *u, int style)
{
	put(t, u, (uint32_t)strlen(u), style);
}

static int level(int style)
{
	return style & (TEX_SUP | TEX_SUB);
}

/* before something that behaves like a variable or number */
static void atom_start(tex_t *t, int style)
{
	if (level(style))
		return;
	if (t->pend_space)
		puts_(t, " ", style & ~(TEX_SUP | TEX_SUB));
	t->pend_space = 0;
}

static void atom(tex_t *t, const char *u, uint32_t n, int style)
{
	atom_start(t, style);
	put(t, u, n, style);
	if (!level(style))
		t->prev = P_ATOM;
}

/* relation or binary operator: spaced on the baseline unless unary */
static void op(tex_t *t, const char *u, int style)
{
	if (!level(style) && t->prev == P_ATOM) {
		t->pend_space = 0;
		puts_(t, " ", style);
		puts_(t, u, style);
		puts_(t, " ", style);
	} else {
		atom_start(t, style);
		puts_(t, u, style);
	}
	if (!level(style))
		t->prev = P_OP;
}

static void open_(tex_t *t, const char *u, int style)
{
	if ((u[0] == '(' || u[0] == '[') && !u[1])
		t->pend_space = 0;	/* sin(x), E[X] */
	atom_start(t, style);
	puts_(t, u, style);
	if (!level(style))
		t->prev = P_OPEN;
}

/* ------------------------------------------------------------------ */
/* source scanning */

static int is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static uint32_t utf8_len(unsigned char c)
{
	return c < 0xc0 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
}

static void skip_space(tex_t *t, uint32_t end)
{
	while (t->i < end && (t->s[t->i] == ' ' || t->s[t->i] == '\n' || t->s[t->i] == '\t'))
		t->i++;
}

/* index of the '}' matching the '{' at i (or end) */
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

/* command name after a backslash at t->i (which points past it) */
static uint32_t read_name(tex_t *t, uint32_t end, char *name, uint32_t cap)
{
	uint32_t n = 0;
	if (t->i < end && is_alpha(t->s[t->i])) {
		while (t->i < end && is_alpha(t->s[t->i])) {
			if (n + 1 < cap)
				name[n++] = t->s[t->i];
			t->i++;
		}
	} else if (t->i < end) {
		name[n++] = t->s[t->i++];
	}
	name[n] = 0;
	return n;
}

/* one argument: {group}, \command or one character */
static int arg_span(tex_t *t, uint32_t end, uint32_t *a, uint32_t *b)
{
	skip_space(t, end);
	if (t->i >= end)
		return 0;
	if (t->s[t->i] == '{') {
		uint32_t c = match_brace(t->s, t->i, end);
		*a = t->i + 1;
		*b = c;
		t->i = c < end ? c + 1 : end;
		return 1;
	}
	*a = t->i;
	if (t->s[t->i] == '\\') {
		char nm[32];
		t->i++;
		read_name(t, end, nm, sizeof(nm));
	} else {
		t->i += utf8_len((unsigned char)t->s[t->i]);
		if (t->i > end)
			t->i = end;
	}
	*b = t->i;
	return 1;
}

/* [optional] argument */
static int opt_span(tex_t *t, uint32_t end, uint32_t *a, uint32_t *b)
{
	uint32_t j;
	skip_space(t, end);
	if (t->i >= end || t->s[t->i] != '[')
		return 0;
	for (j = t->i + 1; j < end && t->s[j] != ']'; ++j)
		;
	*a = t->i + 1;
	*b = j;
	t->i = j < end ? j + 1 : end;
	return 1;
}

/* end of one group, command or character starting at i */
static uint32_t skip_token(const char *s, uint32_t i, uint32_t b)
{
	if (i >= b)
		return b;
	if (s[i] == '{') {
		uint32_t c = match_brace(s, i, b);
		return c < b ? c + 1 : b;
	}
	if (s[i] == '\\') {
		i++;
		if (i < b && is_alpha(s[i])) {
			while (i < b && is_alpha(s[i]))
				i++;
			return i;
		}
		return i < b ? i + 1 : b;
	}
	return i + utf8_len((unsigned char)s[i]);
}

/* an argument that reads fine without brackets: x, 10, dx, \pi, a_{n+1},
 * n^{2}, {\sqrt {2}} */
static int simple(const char *s, uint32_t a, uint32_t b)
{
	uint32_t i;
	while (a < b && s[a] == ' ')
		a++;
	while (b > a && s[b - 1] == ' ')
		b--;
	if (a >= b)
		return 1;
	/* {...} around everything: look inside */
	if (s[a] == '{' && match_brace(s, a, b) == b - 1)
		return simple(s, a + 1, b - 1);
	if (s[a] >= '0' && s[a] <= '9') {
		for (i = a; i < b && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.'); ++i)
			;
	} else if (s[a] == 'd' && b - a == 2 && is_alpha(s[a + 1])) {
		return 1;	/* dx */
	} else if (s[a] == '\\' && b - a > 5 && memcmp(s + a, "\\sqrt", 5) == 0 && !is_alpha(s[a + 5])) {
		i = skip_token(s, a + 5, b);
		while (i < b && s[i] == ' ')
			i++;
		return skip_token(s, i, b) == b && simple(s, i, b);
	} else {
		i = skip_token(s, a, b);
		if (s[a] == '\\') {
			/* only symbols, not \frac and the like */
			char nm[16];
			uint32_t n = i - a - 1 < sizeof(nm) - 1 ? i - a - 1 : sizeof(nm) - 1;
			memcpy(nm, s + a + 1, n);
			nm[n] = 0;
			if (!strcmp(nm, "frac") || !strcmp(nm, "dfrac") || !strcmp(nm, "tfrac") ||
			    !strcmp(nm, "binom") || !strcmp(nm, "sqrt") || !strcmp(nm, "left") ||
			    !strcmp(nm, "operatorname") || !strcmp(nm, "mathrm"))
				return 0;
		}
	}
	/* then any number of scripts */
	while (i < b) {
		while (i < b && s[i] == ' ')
			i++;
		if (i < b && (s[i] == '\'' || s[i] == '!')) {
			i++;
			continue;
		}
		if (i >= b || (s[i] != '^' && s[i] != '_'))
			break;
		i++;
		while (i < b && s[i] == ' ')
			i++;
		i = skip_token(s, i, b);
	}
	return i >= b;
}

static void render(tex_t *t, uint32_t a, uint32_t b, int style);

static void render_sub(tex_t *t, uint32_t a, uint32_t b, int style)
{
	uint32_t save = t->i;
	render(t, a, b, style);
	t->i = save;
}

/* argument in brackets unless it is simple */
static void bracketed(tex_t *t, uint32_t a, uint32_t b, int style)
{
	if (simple(t->s, a, b)) {
		render_sub(t, a, b, style);
	} else {
		open_(t, "(", style);
		render_sub(t, a, b, style);
		atom(t, ")", 1, style);
	}
}

/* \text{...}: words as they are */
static void literal(tex_t *t, uint32_t a, uint32_t b, int style)
{
	uint32_t i;
	atom_start(t, style);
	for (i = a; i < b; ++i) {
		char c = t->s[i];
		if (c == '$' || c == '{' || c == '}')
			continue;
		if (c == '\\' && i + 1 < b) {
			c = t->s[++i];
			if (is_alpha(c)) {	/* a command inside text: drop it */
				while (i + 1 < b && is_alpha(t->s[i + 1]))
					i++;
				continue;
			}
		}
		put(t, &c, 1, style);
	}
	if (!level(style))
		t->prev = P_ATOM;
}

static const char *blackboard(char c)
{
	switch (c) {
	case 'N': return "\xe2\x84\x95";
	case 'Z': return "\xe2\x84\xa4";
	case 'Q': return "\xe2\x84\x9a";
	case 'R': return "\xe2\x84\x9d";
	case 'C': return "\xe2\x84\x82";
	case 'P': return "\xe2\x84\x99";
	case 'H': return "\xe2\x84\x8d";
	}
	return NULL;
}

static int env_is(const char *name, const char *what)
{
	return strcmp(name, what) == 0;
}

enum { E_NONE, E_PAREN, E_BRACK, E_BRACE, E_VERT, E_DVERT, E_CASES, E_ALIGN };

static int env_kind(const char *nm)
{
	if (env_is(nm, "pmatrix") || env_is(nm, "pmatrix*")) return E_PAREN;
	if (env_is(nm, "bmatrix") || env_is(nm, "bmatrix*")) return E_BRACK;
	if (env_is(nm, "Bmatrix")) return E_BRACE;
	if (env_is(nm, "vmatrix")) return E_VERT;
	if (env_is(nm, "Vmatrix")) return E_DVERT;
	if (env_is(nm, "cases") || env_is(nm, "dcases")) return E_CASES;
	if (env_is(nm, "matrix") || env_is(nm, "smallmatrix") || env_is(nm, "array"))
		return E_NONE;
	return E_ALIGN;
}

static void command(tex_t *t, uint32_t end, int *style)
{
	char nm[32];
	uint32_t a, b, a2, b2;
	const struct sym *sy;
	int i, st = *style;

	read_name(t, end, nm, sizeof(nm));
	if (!nm[0])
		return;

	/* spacing */
	if (env_is(nm, ",") || env_is(nm, ":") || env_is(nm, ";") || env_is(nm, " ") ||
	    env_is(nm, "quad") || env_is(nm, "qquad") || env_is(nm, "enspace") ||
	    env_is(nm, "thinspace") || env_is(nm, "medspace") || env_is(nm, "thickspace")) {
		if (!level(st) && t->prev != P_START) {
			puts_(t, " ", st);
			t->pend_space = 0;
		}
		return;
	}
	if (env_is(nm, "!") || env_is(nm, "displaystyle") || env_is(nm, "textstyle") ||
	    env_is(nm, "scriptstyle") || env_is(nm, "scriptscriptstyle") || env_is(nm, "limits") ||
	    env_is(nm, "nolimits") || env_is(nm, "nonumber") || env_is(nm, "notag") ||
	    env_is(nm, "middle") || env_is(nm, "strut") || env_is(nm, "allowbreak") ||
	    env_is(nm, "mathstrut") || env_is(nm, "it") || env_is(nm, "cal") || env_is(nm, "normalsize") ||
	    env_is(nm, "small") || env_is(nm, "large") || env_is(nm, "Large") || env_is(nm, "tiny"))
		return;
	if (env_is(nm, "rm") || env_is(nm, "sf") || env_is(nm, "tt")) {
		*style |= TEX_UPRIGHT;
		return;
	}
	if (env_is(nm, "bf")) {
		*style |= TEX_BOLD | TEX_UPRIGHT;
		return;
	}
	if (env_is(nm, "left") || env_is(nm, "right") || (nm[0] == 'b' && strncmp(nm, "big", 3) == 0) ||
	    (nm[0] == 'B' && strncmp(nm, "Big", 3) == 0)) {
		skip_space(t, end);
		if (t->i < end && t->s[t->i] == '.')
			t->i++;		/* \left. : no bracket */
		return;
	}
	if (env_is(nm, "\\") || env_is(nm, "cr") || env_is(nm, "newline")) {
		opt_span(t, end, &a, &b);
		if (t->nenv && t->env[t->nenv - 1] == E_ALIGN)
			op(t, ";", st & ~TEX_SUP);	/* aligned lines: "a = b; c = d" */
		else {
			puts_(t, ";", st);
			if (!level(st))
				puts_(t, " ", st);
			t->prev = P_OPEN;
		}
		return;
	}

	/* fractions and friends */
	if (env_is(nm, "frac") || env_is(nm, "dfrac") || env_is(nm, "tfrac") || env_is(nm, "cfrac")) {
		if (!arg_span(t, end, &a, &b))
			return;
		if (!arg_span(t, end, &a2, &b2))
			a2 = b2 = b;
		bracketed(t, a, b, st);
		puts_(t, "/", st);
		t->prev = P_OPEN;
		bracketed(t, a2, b2, st);
		if (!level(st))
			t->prev = P_ATOM;
		return;
	}
	if (env_is(nm, "binom") || env_is(nm, "dbinom") || env_is(nm, "tbinom")) {
		if (!arg_span(t, end, &a, &b))
			return;
		if (!arg_span(t, end, &a2, &b2))
			a2 = b2 = b;
		atom_start(t, st);
		puts_(t, "C", st | TEX_UPRIGHT);
		open_(t, "(", st);
		render_sub(t, a, b, st);
		puts_(t, ", ", st);
		t->prev = P_OPEN;
		render_sub(t, a2, b2, st);
		atom(t, ")", 1, st);
		return;
	}
	if (env_is(nm, "sqrt")) {
		atom_start(t, st);
		if (opt_span(t, end, &a, &b))
			render_sub(t, a, b, (st & ~TEX_SUB) | TEX_SUP);
		if (!arg_span(t, end, &a, &b))
			return;
		puts_(t, "\xe2\x88\x9a", st);
		t->prev = P_OPEN;
		bracketed(t, a, b, st);
		if (!level(st))
			t->prev = P_ATOM;
		return;
	}

	/* fonts and text */
	if (env_is(nm, "text") || env_is(nm, "textrm") || env_is(nm, "mbox") || env_is(nm, "hbox") ||
	    env_is(nm, "textnormal") || env_is(nm, "textsf") || env_is(nm, "texttt") ||
	    env_is(nm, "textup") || env_is(nm, "textit") || env_is(nm, "textbf") || env_is(nm, "emph")) {
		int s2 = st | TEX_UPRIGHT;
		if (env_is(nm, "textit") || env_is(nm, "emph"))
			s2 = st & ~TEX_UPRIGHT;
		if (env_is(nm, "textbf"))
			s2 |= TEX_BOLD;
		if (arg_span(t, end, &a, &b))
			literal(t, a, b, s2);
		return;
	}
	if (env_is(nm, "mathrm") || env_is(nm, "mathup") || env_is(nm, "mathsf") ||
	    env_is(nm, "mathtt") || env_is(nm, "operatorname") || env_is(nm, "mathop") ||
	    env_is(nm, "operatorname*")) {
		if (t->i < end && t->s[t->i] == '*')
			t->i++;
		if (!arg_span(t, end, &a, &b))
			return;
		if (!level(st) && t->prev == P_ATOM && nm[0] == 'o')
			t->pend_space = 1;
		render_sub(t, a, b, st | TEX_UPRIGHT);
		if (nm[0] == 'o' || env_is(nm, "mathop"))
			t->pend_space = !level(st);
		return;
	}
	if (env_is(nm, "mathbf") || env_is(nm, "boldsymbol") || env_is(nm, "bm") || env_is(nm, "pmb") ||
	    env_is(nm, "mathbfit")) {
		if (arg_span(t, end, &a, &b))
			render_sub(t, a, b, st | TEX_BOLD | (nm[4] == 'f' && !nm[6] ? TEX_UPRIGHT : 0));
		return;
	}
	if (env_is(nm, "mathbb") || env_is(nm, "Bbb") || env_is(nm, "mathbbm") || env_is(nm, "symbb")) {
		uint32_t k;
		if (!arg_span(t, end, &a, &b))
			return;
		for (k = a; k < b; ++k) {
			const char *u = blackboard(t->s[k]);
			if (u)
				atom(t, u, (uint32_t)strlen(u), st);
			else if (t->s[k] != ' ')
				atom(t, t->s + k, 1, st | TEX_BOLD | TEX_UPRIGHT);
		}
		return;
	}
	if (env_is(nm, "mathit") || env_is(nm, "mathcal") || env_is(nm, "mathscr") ||
	    env_is(nm, "mathfrak") || env_is(nm, "mathnormal") || env_is(nm, "mathrel") ||
	    env_is(nm, "mathbin") || env_is(nm, "mathord") || env_is(nm, "mathopen") ||
	    env_is(nm, "mathclose") || env_is(nm, "mathpunct") || env_is(nm, "underline") ||
	    env_is(nm, "overbrace") || env_is(nm, "underbrace") || env_is(nm, "boxed") ||
	    env_is(nm, "acute") || env_is(nm, "grave") || env_is(nm, "displaylines") ||
	    env_is(nm, "mathring") || env_is(nm, "smash") || env_is(nm, "cancel")) {
		if (arg_span(t, end, &a, &b))
			render_sub(t, a, b, st);
		return;
	}
	/* accents: the letter, then a small mark after it */
	{
		static const char *const acc[][2] = {
			{"hat", "^"}, {"widehat", "^"}, {"bar", "\xc2\xaf"}, {"overline", "\xc2\xaf"},
			{"tilde", "~"}, {"widetilde", "~"}, {"vec", "\xe2\x86\x92"},
			{"overrightarrow", "\xe2\x86\x92"}, {"overleftarrow", "\xe2\x86\x90"},
			{"dot", "\xc2\xb7"}, {"ddot", "\xc2\xb7\xc2\xb7"}, {"dddot", "\xc2\xb7\xc2\xb7\xc2\xb7"},
			{"check", "v"}, {"breve", "u"}, {NULL, NULL}
		};
		for (i = 0; acc[i][0]; ++i)
			if (env_is(nm, acc[i][0])) {
				if (!arg_span(t, end, &a, &b))
					return;
				if (simple(t->s, a, b))
					render_sub(t, a, b, st);
				else
					bracketed(t, a, b, st);
				puts_(t, acc[i][1], (st & ~TEX_SUB) | TEX_SUP | TEX_UPRIGHT);
				return;
			}
	}
	if (env_is(nm, "stackrel") || env_is(nm, "overset") || env_is(nm, "underset")) {
		if (!arg_span(t, end, &a, &b) || !arg_span(t, end, &a2, &b2))
			return;
		render_sub(t, a2, b2, st);
		render_sub(t, a, b, (st & ~(TEX_SUP | TEX_SUB)) | (nm[0] == 'u' ? TEX_SUB : TEX_SUP));
		return;
	}
	if (env_is(nm, "substack")) {
		if (arg_span(t, end, &a, &b))
			render_sub(t, a, b, st);
		return;
	}
	if (env_is(nm, "color") || env_is(nm, "pagecolor") || env_is(nm, "label") ||
	    env_is(nm, "hspace") || env_is(nm, "vspace") || env_is(nm, "phantom") ||
	    env_is(nm, "hphantom") || env_is(nm, "vphantom") || env_is(nm, "mathchoice")) {
		arg_span(t, end, &a, &b);
		return;
	}
	if (env_is(nm, "textcolor") || env_is(nm, "colorbox")) {
		arg_span(t, end, &a, &b);
		if (arg_span(t, end, &a, &b))
			render_sub(t, a, b, st);
		return;
	}
	if (env_is(nm, "tag")) {
		if (arg_span(t, end, &a, &b)) {
			puts_(t, "   (", st | TEX_UPRIGHT);
			literal(t, a, b, st | TEX_UPRIGHT);
			puts_(t, ")", st | TEX_UPRIGHT);
		}
		return;
	}
	if (env_is(nm, "pmod") || env_is(nm, "pod")) {
		if (!arg_span(t, end, &a, &b))
			return;
		if (!level(st))
			puts_(t, " ", st);
		puts_(t, nm[1] == 'm' ? "(mod " : "(", st | TEX_UPRIGHT);
		t->prev = P_OPEN;
		render_sub(t, a, b, st);
		atom(t, ")", 1, st | TEX_UPRIGHT);
		return;
	}
	if (env_is(nm, "bmod") || env_is(nm, "mod")) {
		op(t, "mod", st | TEX_UPRIGHT);
		return;
	}
	if (env_is(nm, "not")) {
		uint32_t save;
		skip_space(t, end);
		save = t->i;
		if (t->i < end && t->s[t->i] == '=') {
			t->i++;
			op(t, "\xe2\x89\xa0", st);
			return;
		}
		if (t->i < end && t->s[t->i] == '\\') {
			char n2[32];
			t->i++;
			read_name(t, end, n2, sizeof(n2));
			if (env_is(n2, "in")) { op(t, "\xe2\x88\x89", st); return; }
			if (env_is(n2, "equiv")) { op(t, "\xe2\x89\xa2", st); return; }
			if (env_is(n2, "subset")) { op(t, "\xe2\x8a\x84", st); return; }
			if (env_is(n2, "mid")) { op(t, "\xe2\x88\xa4", st); return; }
			t->i = save;
		}
		open_(t, "\xc2\xac", st);
		return;
	}
	if (env_is(nm, "begin") || env_is(nm, "end")) {
		char en[24];
		uint32_t k, n = 0;
		int kind;
		if (!arg_span(t, end, &a, &b))
			return;
		for (k = a; k < b && n + 1 < sizeof(en); ++k)
			en[n++] = t->s[k];
		en[n] = 0;
		kind = env_kind(en);
		if (nm[0] == 'b') {
			if (env_is(en, "array") || env_is(en, "alignedat") || env_is(en, "subarray"))
				arg_span(t, end, &a2, &b2);	/* column spec */
			if (t->nenv < 8)
				t->env[t->nenv++] = kind;
			switch (kind) {
			case E_PAREN: open_(t, "(", st | TEX_UPRIGHT); break;
			case E_BRACK: open_(t, "[", st | TEX_UPRIGHT); break;
			case E_BRACE: case E_CASES: open_(t, "{", st | TEX_UPRIGHT); break;
			case E_VERT: open_(t, "|", st | TEX_UPRIGHT); break;
			case E_DVERT: open_(t, "\xe2\x80\x96", st | TEX_UPRIGHT); break;
			}
		} else {
			if (t->nenv)
				t->nenv--;
			switch (kind) {
			case E_PAREN: atom(t, ")", 1, st | TEX_UPRIGHT); break;
			case E_BRACK: atom(t, "]", 1, st | TEX_UPRIGHT); break;
			case E_BRACE: atom(t, "}", 1, st | TEX_UPRIGHT); break;
			case E_VERT: atom(t, "|", 1, st | TEX_UPRIGHT); break;
			case E_DVERT: atom(t, "\xe2\x80\x96", 3, st | TEX_UPRIGHT); break;
			}
		}
		return;
	}

	/* named operators */
	if (env_is(nm, "limsup") || env_is(nm, "liminf")) {
		int had = t->pend_space;
		atom_start(t, st);
		if (!had && !level(st) && t->prev == P_ATOM)
			puts_(t, " ", st);
		puts_(t, nm[3] == 's' ? "lim sup" : "lim inf", st | TEX_UPRIGHT);
		if (!level(st)) {
			t->prev = P_ATOM;
			t->pend_space = 1;
		}
		return;
	}
	for (i = 0; funcs[i]; ++i)
		if (env_is(nm, funcs[i])) {
			int had = t->pend_space;
			atom_start(t, st);
			if (!had && !level(st) && t->prev == P_ATOM)
				puts_(t, " ", st);
			puts_(t, funcs[i], st | TEX_UPRIGHT);
			if (!level(st)) {
				t->prev = P_ATOM;
				t->pend_space = 1;
			}
			return;
		}

	/* symbols */
	for (sy = syms; sy->name; ++sy)
		if (env_is(nm, sy->name)) {
			int s2 = st | TEX_UPRIGHT;
			/* Greek lower case is italic like Latin letters */
			if ((unsigned char)sy->u[0] == 0xce || (unsigned char)sy->u[0] == 0xcf)
				s2 = (sy->name[0] >= 'a') ? st : st | TEX_UPRIGHT;
			switch (sy->kind) {
			case K_OP:
				op(t, sy->u, s2);
				break;
			case K_BIG: {
				int had = t->pend_space;
				atom_start(t, st);
				if (!had && !level(st) && t->prev == P_ATOM)
					puts_(t, " ", st);
				puts_(t, sy->u, s2);
				if (!level(st)) {
					t->prev = P_OPEN;
					t->pend_space = 1;
				}
				break;
			}
			case K_OPEN:
				open_(t, sy->u, s2);
				break;
			default:
				atom(t, sy->u, (uint32_t)strlen(sy->u), s2);
				break;
			}
			return;
		}

	/* unknown: show the name rather than lose it */
	atom(t, nm, (uint32_t)strlen(nm), st | TEX_UPRIGHT);
}

/* plain TeX infix fractions: {a \over b}, {n \choose k}, {a \atop b};
 * position of the command at brace level 0, or end */
static uint32_t find_infix(const tex_t *t, uint32_t a, uint32_t end, uint32_t *len, int *kind)
{
	static const char *const names[] = { "over", "choose", "atop", NULL };
	int lev = 0, k;
	uint32_t i;
	for (i = a; i < end; ++i) {
		char c = t->s[i];
		if (c == '{')
			lev++;
		else if (c == '}')
			lev--;
		else if (c == '\\' && i + 1 < end) {
			if (!lev)
				for (k = 0; names[k]; ++k) {
					uint32_t n = (uint32_t)strlen(names[k]);
					if (i + 1 + n <= end && !memcmp(t->s + i + 1, names[k], n) &&
					    (i + 1 + n == end || !is_alpha(t->s[i + 1 + n]))) {
						*len = n + 1;
						*kind = k;
						return i;
					}
				}
			i++;	/* skip the escaped character */
		}
	}
	return end;
}

static void render(tex_t *t, uint32_t a, uint32_t end, int style);

static void render_infix(tex_t *t, uint32_t a, uint32_t k, uint32_t len, int kind,
			 uint32_t end, int st)
{
	if (kind == 1) {	/* choose */
		atom_start(t, st);
		puts_(t, "C", st | TEX_UPRIGHT);
		open_(t, "(", st);
		render_sub(t, a, k, st);
		puts_(t, ", ", st);
		t->prev = P_OPEN;
		render_sub(t, k + len, end, st);
		atom(t, ")", 1, st);
	} else {
		bracketed(t, a, k, st);
		puts_(t, kind ? "; " : "/", st);
		t->prev = P_OPEN;
		bracketed(t, k + len, end, st);
		if (!level(st))
			t->prev = P_ATOM;
	}
	t->i = end;
}

static void render(tex_t *t, uint32_t a, uint32_t end, int style)
{
	uint32_t k, len;
	int kind;
	if (++t->depth > 40) {
		t->depth--;
		return;
	}
	k = find_infix(t, a, end, &len, &kind);
	if (k < end) {
		render_infix(t, a, k, len, kind, end, style);
		t->depth--;
		return;
	}
	t->i = a;
	while (t->i < end) {
		char c = t->s[t->i];
		uint32_t x, y;
		switch (c) {
		case ' ': case '\n': case '\t': case '\r':
			t->i++;
			break;
		case '{': {
			uint32_t close = match_brace(t->s, t->i, end);
			render_sub(t, t->i + 1, close, style);
			t->i = close < end ? close + 1 : end;
			break;
		}
		case '}':
			t->i++;
			break;
		case '^': case '_':
			t->i++;
			if (arg_span(t, end, &x, &y)) {
				int s2 = (style & ~(TEX_SUP | TEX_SUB)) | (c == '^' ? TEX_SUP : TEX_SUB);
				int pend = t->pend_space, prev = t->prev;
				t->pend_space = 0;
				render_sub(t, x, y, s2);
				t->pend_space = pend;
				t->prev = prev;
			}
			break;
		case '\\':
			t->i++;
			command(t, end, &style);
			break;
		case '&':
			t->i++;
			if (t->nenv && t->env[t->nenv - 1] != E_ALIGN && !level(style)) {
				/* whitespace collapses in the page, so separate the
				 * cells visibly: [1, 9, −13; 20, 5, −6] */
				puts_(t, ", ", style | TEX_UPRIGHT);
				t->prev = P_OPEN;
				t->pend_space = 0;
			}
			break;
		case '~':
			t->i++;
			if (!level(style))
				puts_(t, " ", style);
			break;
		case '\'':
			t->i++;
			put(t, "\xe2\x80\xb2", 3, style | TEX_UPRIGHT);
			break;
		case '=': case '<': case '>': case '+':
			t->i++;
			op(t, c == '<' ? "<" : c == '>' ? ">" : c == '+' ? "+" : "=", style | TEX_UPRIGHT);
			break;
		case '-':
			t->i++;
			op(t, "\xe2\x88\x92", style | TEX_UPRIGHT);
			break;
		case ',': case ';':
			t->i++;
			put(t, &c, 1, style | TEX_UPRIGHT);
			if (!level(style)) {
				puts_(t, " ", style);
				t->prev = P_OPEN;
				t->pend_space = 0;
			}
			break;
		case '(': case '[':
			t->i++;
			open_(t, c == '(' ? "(" : "[", style | TEX_UPRIGHT);
			break;
		case '$':
			t->i++;
			break;
		default: {
			uint32_t n = utf8_len((unsigned char)c);
			int s2 = style;
			if (t->i + n > end)
				n = end - t->i;
			if (!is_alpha(c))
				s2 |= TEX_UPRIGHT;
			/* a run of digits is one atom */
			if (c >= '0' && c <= '9') {
				uint32_t j = t->i;
				while (j < end && ((t->s[j] >= '0' && t->s[j] <= '9') || t->s[j] == '.'))
					j++;
				n = j - t->i;
			}
			{
				int before = t->prev;
				atom(t, t->s + t->i, n, s2);
				/* an opening bar: |-x| */
				if (c == '|' && !level(style) && before != P_ATOM)
					t->prev = P_OPEN;
			}
			t->i += n;
			break;
		}
		}
	}
	t->depth--;
}

void tex_render(const char *src, uint32_t n, tex_out_fn out, void *ctx)
{
	tex_t t;
	memset(&t, 0, sizeof(t));
	t.s = src;
	t.out = out;
	t.ctx = ctx;
	t.prev = P_START;
	render(&t, 0, n, 0);
	flush(&t);
}
