/*
 * zimwiki - calculator core (see calc.h)
 *
 * Recursive descent:
 *   expr    = term { (+|-) term }
 *   term    = unary { (*|/) unary | implicit multiply }
 *   unary   = - unary | + unary | power
 *   power   = postfix [ ^ unary ]          (right to left)
 *   postfix = primary { % | ! }
 *   primary = number [E [+|-] digits] | A | p | e | func ( expr [ ) ]
 *           | ( expr [ ) ]
 *
 * The maths functions are written here (the device has no libm).  They
 * avoid looking at the bits of a double (word order differs between
 * soft-float builds): powers of two are found and applied by exact
 * multiplication.  Accuracy is about 1e-15 relative, far more than the
 * ten digits shown; tests/calctest.c checks them against the PC's libm.
 */
#include <stdint.h>
#include <string.h>
#include "calc.h"

#define POW10_MAX 308

static double pow10_tab[POW10_MAX + 1];
static int pow10_ready;

static void pow10_init(void)
{
	int i;
	if (pow10_ready)
		return;
	pow10_tab[0] = 1.0;
	for (i = 1; i <= POW10_MAX; ++i)
		pow10_tab[i] = pow10_tab[i - 1] * 10.0;
	pow10_ready = 1;
}

static int is_bad(double v)
{
	/* NaN, or infinity (inf - inf is NaN) */
	return v != v || v - v != v - v;
}

static double fabs_(double x)
{
	return x < 0 ? -x : x;
}

/* ------------------------------------------------------------------ */
/* maths */

#define PI      3.14159265358979323846
#define PI_2    1.57079632679489661923
#define PI_6    0.52359877559829887308
#define LN2_HI  6.93147180369123816490e-01	/* 33 bits: k*LN2_HI is exact */
#define LN2_LO  1.90821492927058770002e-10
#define LN10    2.30258509299404568402
#define SQRT2   1.41421356237309504880
#define SQRT3   1.73205080756887729353
#define TAN15   0.26794919243112270647
#define E_      2.71828182845904523536
/* pi/2 in three parts, for reducing sin/cos arguments (n * PIO2_1 is
 * exact for n < 2^20) */
#define PIO2_1  1.57079632673412561417e+00
#define PIO2_2  6.07710050630396597660e-11
#define PIO2_3  2.02226624871116645580e-21
#define RAD_MAX 1.6e6			/* beyond: too imprecise */

/* x * 2^k, exactly (barring overflow and underflow) */
static double scale2(double x, int k)
{
	while (k >= 64) {
		x *= 18446744073709551616.0;	/* 2^64 */
		k -= 64;
	}
	while (k <= -64) {
		x *= 5.42101086242752217004e-20;	/* 2^-64 */
		k += 64;
	}
	while (k > 0) {
		x *= 2.0;
		k--;
	}
	while (k < 0) {
		x *= 0.5;
		k++;
	}
	return x;
}

/* k with 2^k <= x < 2^(k+1), for x > 0 */
static int ilog2(double x)
{
	int k = 0;
	while (x >= 18446744073709551616.0) {
		x *= 5.42101086242752217004e-20;
		k += 64;
	}
	while (x < 5.42101086242752217004e-20) {
		x *= 18446744073709551616.0;
		k -= 64;
	}
	while (x >= 2.0) {
		x *= 0.5;
		k++;
	}
	while (x < 1.0) {
		x *= 2.0;
		k--;
	}
	return k;
}

/* nearest integer (halves away from zero), for |x| < 2^62 */
static double round_(double x)
{
	int64_t n = (int64_t)(x < 0 ? x - 0.5 : x + 0.5);
	return (double)n;
}

static double floor_(double x)
{
	double r;
	if (fabs_(x) >= 4503599627370496.0)	/* 2^52: already whole */
		return x;
	r = (double)(int64_t)x;
	return r > x ? r - 1.0 : r;
}

static int is_int(double x)
{
	return fabs_(x) >= 4503599627370496.0 || floor_(x) == x;
}

static double m_sqrt(double x)
{
	int k, i;
	double m, g;
	if (x == 0.0)
		return 0.0;
	k = ilog2(x);
	if (k & 1)
		k--;
	m = scale2(x, -k);	/* [1, 4) */
	g = (1.0 + m) * 0.5;
	for (i = 0; i < 6; ++i)
		g = 0.5 * (g + m / g);
	return scale2(g, k / 2);
}

static double m_exp(double x)
{
	double k, r, t, s;
	int i;
	k = round_(x / (LN2_HI + LN2_LO));
	r = (x - k * LN2_HI) - k * LN2_LO;	/* |r| <= 0.35 */
	s = 1.0;
	t = 1.0;
	for (i = 1; i <= 15; ++i) {
		t *= r / i;
		s += t;
	}
	return scale2(s, (int)k);
}

static double m_ln(double x)
{
	int k = ilog2(x), i;
	double m = scale2(x, -k), s, s2, t, sum;
	if (m > SQRT2) {
		m *= 0.5;
		k++;
	}
	s = (m - 1.0) / (m + 1.0);	/* |s| <= 0.172 */
	s2 = s * s;
	t = s;
	sum = 0.0;
	for (i = 1; i <= 27; i += 2) {
		sum += t / i;
		t *= s2;
	}
	return k * LN2_HI + (2.0 * sum + k * LN2_LO);
}

/* sin and cos of |r| <= pi/4 */
static double k_sin(double r)
{
	double r2 = r * r, t = r, s = r;
	int i;
	for (i = 2; i <= 18; i += 2) {
		t *= -r2 / (i * (i + 1));
		s += t;
	}
	return s;
}

static double k_cos(double r)
{
	double r2 = r * r, t = 1.0, s = 1.0;
	int i;
	for (i = 1; i <= 19; i += 2) {
		t *= -r2 / (i * (i + 1));
		s += t;
	}
	return s;
}

/* reduce x to r in [-pi/4, pi/4] (or [-45, 45] degrees, then to
 * radians) and quadrant q, so x = q*90deg + r */
static int reduce(double x, int deg, double *r, int *q)
{
	double n;
	if (deg) {
		if (fabs_(x) > 1e15)
			return CALC_DOMAIN;
		x -= 360.0 * floor_(x / 360.0);	/* [0, 360) */
		n = round_(x / 90.0);
		*r = (x - 90.0 * n) * (PI / 180.0);
	} else {
		if (fabs_(x) > RAD_MAX)
			return CALC_DOMAIN;
		n = round_(x / PI_2);
		*r = ((x - n * PIO2_1) - n * PIO2_2) - n * PIO2_3;
	}
	*q = (int)((int64_t)n & 3);
	return CALC_OK;
}

static double m_atan(double x)
{
	int neg = x < 0, inv = 0, shift = 0, i;
	double x2, t, s;
	if (neg)
		x = -x;
	if (x > 1.0) {
		x = 1.0 / x;
		inv = 1;
	}
	if (x > TAN15) {
		x = (x * SQRT3 - 1.0) / (x + SQRT3);
		shift = 1;
	}
	x2 = x * x;
	t = x;
	s = 0.0;
	for (i = 1; i <= 31; i += 2) {
		s += t / i;
		t *= -x2;
	}
	if (shift)
		s += PI_6;
	if (inv)
		s = PI_2 - s;
	return neg ? -s : s;
}

/* tiny results of sin/cos/tan of radians from rounding pi (sin(pi) is
 * 1.2e-16, not 0): shown as 0 */
static double snap(double v, double x)
{
	double ax = fabs_(x);
	if (ax > 1.0 && fabs_(v) < 1e-15 * ax)
		return 0.0;
	return v;
}

int calc_func(int fn, double x, int flags, double *out)
{
	int deg = flags & CALC_DEG, q, r;
	double rr, s, c, v;
	*out = 0.0;
	switch (fn) {
	case 's': case 'o': case 't':
		if ((r = reduce(x, deg, &rr, &q)) != CALC_OK)
			return r;
		s = k_sin(rr);
		c = k_cos(rr);
		if (q & 1) {
			v = s;
			s = c;
			c = -v;
		}
		if (q & 2) {
			s = -s;
			c = -c;
		}
		if (fn == 't') {
			if (c == 0.0)
				return CALC_DOMAIN;	/* tan 90 degrees */
			v = s / c;
		} else {
			v = fn == 's' ? s : c;
		}
		*out = deg ? v : snap(v, x);
		return CALC_OK;
	case 'S': case 'O':
		if (x > 1.0 || x < -1.0)
			return CALC_DOMAIN;
		if (fn == 'S')
			v = x == 1.0 ? PI_2 : x == -1.0 ? -PI_2 :
			    m_atan(x / m_sqrt((1.0 - x) * (1.0 + x)));
		else
			v = x == -1.0 ? PI : 2.0 * m_atan(m_sqrt((1.0 - x) / (1.0 + x)));
		*out = deg ? v * (180.0 / PI) : v;
		return CALC_OK;
	case 'T':
		v = m_atan(x);
		*out = deg ? v * (180.0 / PI) : v;
		return CALC_OK;
	case 'l': case 'g':
		if (x <= 0.0)
			return CALC_DOMAIN;
		v = m_ln(x);
		if (fn == 'g') {
			v /= LN10;
			if (fabs_(v - round_(v)) < 1e-14)
				v = round_(v);	/* log(1000) is 3 exactly */
		}
		*out = v;
		return CALC_OK;
	case 'r':
		if (x < 0.0)
			return CALC_DOMAIN;
		*out = m_sqrt(x);
		return CALC_OK;
	case 'x':		/* e^x */
		if (x > 709.78)
			return CALC_RANGE;
		*out = x < -745.0 ? 0.0 : m_exp(x);
		return CALC_OK;
	case '!': {
		int n, i;
		if (x < 0.0 || !is_int(x))
			return CALC_DOMAIN;
		if (x > 170.0)
			return CALC_RANGE;
		n = (int)x;
		v = 1.0;
		for (i = 2; i <= n; ++i)
			v *= i;
		*out = v;
		return CALC_OK;
	}
	}
	return CALC_SYNTAX;
}

int calc_pow(double x, double y, double *out)
{
	double l;
	*out = 0.0;
	if (x == E_)
		return calc_func('x', y, 0, out);
	if (is_int(y) && fabs_(y) <= 2147483647.0) {
		/* whole powers by squaring: exact where possible, any sign */
		int64_t n = (int64_t)y;
		uint64_t k = (uint64_t)(n < 0 ? -n : n);
		double b = x, v = 1.0;
		if (x == 0.0 && n < 0)
			return CALC_DIV0;
		while (k) {
			if (k & 1)
				v *= b;
			b *= b;
			k >>= 1;
			if (is_bad(v) || (k && is_bad(b) && v != 0.0))
				return n < 0 ? (*out = 0.0, CALC_OK) : CALC_RANGE;
		}
		*out = n < 0 ? 1.0 / v : v;
		return is_bad(*out) ? CALC_RANGE : CALC_OK;
	}
	if (x < 0.0)
		return CALC_DOMAIN;	/* (-8)^0.5 */
	if (x == 0.0)
		return y > 0 ? CALC_OK : CALC_DIV0;
	l = y * m_ln(x);
	if (l > 709.78)
		return CALC_RANGE;
	*out = l < -745.0 ? 0.0 : m_exp(l);
	return CALC_OK;
}

/* ------------------------------------------------------------------ */
/* parser */

typedef struct {
	const char *s;
	double ans;
	int flags;
	int err;
} parser_t;

static double p_expr(parser_t *p);
static double p_unary(parser_t *p, int *pct);

static int p_peek(parser_t *p)
{
	return (unsigned char)*p->s;
}

static void p_fail(parser_t *p, int err)
{
	if (!p->err)
		p->err = err;
}

static int is_func(int c)
{
	return c && strchr("sotSOTlgr", c) != NULL;
}

static double p_number(parser_t *p)
{
	uint64_t mant = 0;
	int digits = 0, frac = 0, dot = 0, drop = 0, ex = 0;
	double v;
	while ((*p->s >= '0' && *p->s <= '9') || *p->s == '.') {
		if (*p->s == '.') {
			if (dot) {
				p_fail(p, CALC_SYNTAX);	/* 1..2, 1.2.3 */
				return 0.0;
			}
			dot = 1;
		} else if (digits < 18) {
			if (mant || *p->s != '0')
				digits++;
			mant = mant * 10 + (uint64_t)(*p->s - '0');
			if (dot)
				frac++;
		} else if (!dot) {
			drop++;		/* beyond 18 digits: keep the scale */
		}
		p->s++;
	}
	if (*p->s == 'E') {
		/* 1.5E-3: times ten to the */
		int neg = 0, nd = 0;
		p->s++;
		if (*p->s == '-' || *p->s == '+')
			neg = *p->s++ == '-';
		while (*p->s >= '0' && *p->s <= '9') {
			if (ex < 10000)
				ex = ex * 10 + (*p->s - '0');
			p->s++;
			nd++;
		}
		if (!nd) {
			p_fail(p, CALC_SYNTAX);
			return 0.0;
		}
		if (neg)
			ex = -ex;
	}
	if (!mant)
		return 0.0;	/* "0", "0.000", or "." alone (counts as 0) */
	ex += drop - frac;
	v = (double)mant;
	/* in two steps so 1E-320 style values do not lose everything */
	while (ex > POW10_MAX) {
		v *= pow10_tab[POW10_MAX];
		ex -= POW10_MAX;
	}
	while (ex < -POW10_MAX) {
		v /= pow10_tab[POW10_MAX];
		ex += POW10_MAX;
	}
	if (ex > 0)
		v *= pow10_tab[ex];
	else if (ex < 0)
		v /= pow10_tab[-ex];
	return v;
}

/* can the next character start an operand (for implicit multiply)? */
static int starts_operand(int c)
{
	return (c >= '0' && c <= '9') || c == '.' || c == '(' || c == 'A' ||
	       c == 'p' || c == 'e' || is_func(c);
}

/* ( expr [ ) ] after the ( */
static double p_group(parser_t *p)
{
	double v;
	if (p_peek(p) == ')') {
		p_fail(p, CALC_SYNTAX);	/* "()" */
		return 0.0;
	}
	v = p_expr(p);
	if (p_peek(p) == ')')
		p->s++;
	else if (p_peek(p) != 0)
		p_fail(p, CALC_SYNTAX);
	/* end of input: the bracket is closed for you */
	return v;
}

static double p_primary(parser_t *p)
{
	int c = p_peek(p), r;
	double v;
	if ((c >= '0' && c <= '9') || c == '.')
		return p_number(p);
	if (c == 'A' || c == 'p' || c == 'e') {
		p->s++;
		return c == 'A' ? p->ans : c == 'p' ? PI : E_;
	}
	if (c == '(') {
		p->s++;
		return p_group(p);
	}
	if (is_func(c)) {
		p->s++;
		if (p_peek(p) != '(') {
			p_fail(p, CALC_SYNTAX);
			return 0.0;
		}
		p->s++;
		v = p_group(p);
		if (p->err)
			return 0.0;
		if ((r = calc_func(c, v, p->flags, &v)) != CALC_OK)
			p_fail(p, r);
		return v;
	}
	p_fail(p, CALC_SYNTAX);
	return 0.0;
}

/* pct: set when the operand ended in % (for a + b%) */
static double p_postfix(parser_t *p, int *pct)
{
	double v = p_primary(p);
	int r;
	*pct = 0;
	for (;;) {
		if (p_peek(p) == '%') {
			p->s++;
			v /= 100.0;
			*pct = 1;
		} else if (p_peek(p) == '!') {
			p->s++;
			if (!p->err && (r = calc_func('!', v, 0, &v)) != CALC_OK)
				p_fail(p, r);
			*pct = 0;
		} else {
			return v;
		}
	}
}

static double p_power(parser_t *p, int *pct)
{
	double v = p_postfix(p, pct), y;
	int one, r;
	if (p_peek(p) != '^')
		return v;
	p->s++;
	y = p_unary(p, &one);	/* 2^-1, 2^3^2 = 2^9 */
	if (p->err)
		return 0.0;
	if ((r = calc_pow(v, y, &v)) != CALC_OK)
		p_fail(p, r);
	*pct = 0;
	return v;
}

static double p_unary(parser_t *p, int *pct)
{
	int c = p_peek(p);
	if (c == '-') {
		p->s++;
		return -p_unary(p, pct);
	}
	if (c == '+') {
		p->s++;
		return p_unary(p, pct);
	}
	return p_power(p, pct);
}

static double p_term(parser_t *p, int *pct)
{
	int one;
	double v = p_unary(p, pct);
	for (;;) {
		int c = p_peek(p);
		double r;
		if (p->err)
			return v;
		if (c == '*' || c == '/') {
			p->s++;
			r = p_unary(p, &one);
			if (c == '/') {
				if (r == 0.0 && !p->err) {
					p_fail(p, CALC_DIV0);
					return v;
				}
				v /= r;
			} else {
				v *= r;
			}
			*pct = 0;
		} else if (starts_operand(c)) {
			/* 2(3), (1)(2), 3A, 2p, 4s(30) */
			r = p_unary(p, &one);
			v *= r;
			*pct = 0;
		} else {
			return v;
		}
	}
}

static double p_expr(parser_t *p)
{
	int pct;
	double v = p_term(p, &pct);
	for (;;) {
		int c = p_peek(p);
		double r;
		if (p->err || (c != '+' && c != '-'))
			return v;
		p->s++;
		r = p_term(p, &pct);
		if (pct)
			r = v * r;	/* a + b% is a + a*b/100 */
		v = c == '+' ? v + r : v - r;
	}
}

int calc_eval(const char *expr, double ans, int flags, double *out)
{
	parser_t p;
	double v;
	pow10_init();
	*out = 0.0;
	if (!expr || !*expr)
		return CALC_EMPTY;
	p.s = expr;
	p.ans = ans;
	p.flags = flags;
	p.err = 0;
	v = p_expr(&p);
	while (!p.err && *p.s == ')')
		p.s++;		/* stray closing brackets at the end: ignored */
	if (!p.err && *p.s)
		p.err = CALC_SYNTAX;
	if (p.err)
		return p.err;
	if (is_bad(v))
		return CALC_RANGE;
	if (v == 0.0)
		v = 0.0;	/* no "-0" */
	*out = v;
	return CALC_OK;
}

/* ------------------------------------------------------------------ */

static char *put_uint(char *o, unsigned v)
{
	char d[12];
	int k = 0;
	do {
		d[k++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (k)
		*o++ = d[--k];
	return o;
}

void calc_format(double v, char *out)
{
	char dig[11];
	uint64_t d;
	double m;
	int e, i, nd;
	char *o = out;

	pow10_init();
	if (is_bad(v)) {
		strcpy(out, "Error");
		return;
	}
	if (v == 0.0) {
		strcpy(out, "0");
		return;
	}
	if (v < 0) {
		*o++ = '-';
		v = -v;
	}
	/* decimal exponent: 10^e <= v < 10^(e+1) */
	if (v >= 1.0) {
		e = 0;
		while (e < POW10_MAX && pow10_tab[e + 1] <= v)
			e++;
		m = v / pow10_tab[e];
	} else {
		e = 0;
		while (e > -POW10_MAX && v * pow10_tab[-e] < 1.0)
			e--;
		m = v * pow10_tab[-e];
	}
	/* ten significant digits, rounded */
	d = (uint64_t)(m * 1e9 + 0.5);
	if (d >= 10000000000ull) {
		d = (d + 5) / 10;
		e++;
	} else if (d < 1000000000ull) {
		d = (uint64_t)(m * 1e10 + 0.5);
		e--;
	}
	for (i = 9; i >= 0; --i) {
		dig[i] = (char)('0' + (int)(d % 10));
		d /= 10;
	}
	nd = 10;
	while (nd > 1 && dig[nd - 1] == '0')
		nd--;

	if (e >= 10 || e < -4) {
		/* 1.2345e+15 */
		*o++ = dig[0];
		if (nd > 1) {
			*o++ = '.';
			for (i = 1; i < nd; ++i)
				*o++ = dig[i];
		}
		*o++ = 'e';
		*o++ = e < 0 ? '-' : '+';
		o = put_uint(o, (unsigned)(e < 0 ? -e : e));
	} else if (e >= 0) {
		/* 1234.5 */
		for (i = 0; i <= e; ++i)
			*o++ = i < nd ? dig[i] : '0';
		if (nd > e + 1) {
			*o++ = '.';
			for (i = e + 1; i < nd; ++i)
				*o++ = dig[i];
		}
	} else {
		/* 0.00123 */
		*o++ = '0';
		*o++ = '.';
		for (i = -1; i > e; --i)
			*o++ = '0';
		for (i = 0; i < nd; ++i)
			*o++ = dig[i];
	}
	*o = 0;
}

const char *calc_error_text(int err)
{
	switch (err) {
	case CALC_DIV0: return "Can't divide by 0";
	case CALC_RANGE: return "Number too big";
	case CALC_DOMAIN: return "Math error";
	case CALC_SYNTAX: return "Error";
	default: return "";
	}
}
