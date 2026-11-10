/*
 * zimwiki - calculator core: expression evaluation, maths functions and
 * number formatting (no libc maths, no snprintf: runs as is on the
 * WikiReader, which has no FPU: doubles are software floating point)
 *
 * Expressions are ASCII:
 *   digits and '.', E for "times ten to the" (1.5E-3)
 *   + - * / %   ^ (power, right to left: 2^3^2 = 2^9)   ! (factorial)
 *   ( )         A = Ans   p = pi   e = 2.71828...
 *   functions, always followed by '(':
 *     s sin  o cos  t tan  S asin  O acos  T atan
 *     l ln   g log10  r square root
 * Unclosed brackets are closed at the end.  Juxtaposition multiplies:
 * 2(3+4), (1+2)(3+4), 3A, 2p, 4s(30).  -2^2 is -4.
 * Percent: x% is x/100, except a +/- b% which is a +/- a*b/100
 * (50+10% = 55, 80-25% = 60), as on most pocket calculators.
 */
#ifndef ZW_CALC_H
#define ZW_CALC_H

enum {
	CALC_OK = 0,
	CALC_SYNTAX = -1,	/* malformed expression */
	CALC_DIV0 = -2,		/* division by zero */
	CALC_RANGE = -3,	/* overflow */
	CALC_EMPTY = -4,	/* nothing to evaluate */
	CALC_DOMAIN = -5	/* sqrt(-1), ln(0), asin(2), 2.5!, tan(90 deg) */
};

#define CALC_DEG 1		/* flags: angles in degrees (else radians) */

/* evaluate expr; ans is the value of 'A'.  Returns CALC_OK or an error */
int calc_eval(const char *expr, double ans, int flags, double *out);

/* one function by its letter (s o t S O T l g r, '!', 'x' for e^x),
 * for tests.  Returns CALC_OK or an error */
int calc_func(int fn, double x, int flags, double *out);

/* x^y */
int calc_pow(double x, double y, double *out);

/* format v with up to 10 significant digits: "1234.5", "-0.001",
 * "1.25e+15" from 1e10 up and below 0.0001 (like printf %.10g).
 * out needs 24 bytes */
void calc_format(double v, char *out);

/* error code as a short message: "Error", "Can't divide by 0", ... */
const char *calc_error_text(int err);

#endif
