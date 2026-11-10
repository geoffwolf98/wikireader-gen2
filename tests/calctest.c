/*
 * calctest: checks the calculator core (src/calc.c)
 *   1. fixed expressions with known answers
 *   2. calc_format against printf("%.10g") on random numbers
 * Prints failures; exit status 1 if any.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../src/calc.h"

static int fails;

static int flags = CALC_DEG;

static void check(const char *expr, double ans, const char *want)
{
	double v;
	char got[64];
	int r = calc_eval(expr, ans, flags, &v);
	if (r == CALC_OK)
		calc_format(v, got);
	else
		strcpy(got, calc_error_text(r));
	if (strcmp(got, want)) {
		printf("FAIL  %-24s got %-20s want %s\n", expr, got, want);
		fails++;
	}
}

/* printf's %.10g, with the exponent written without leading zeros */
static void ref_format(double v, char *out)
{
	char t[64], *e;
	double a = v < 0 ? -v : v;
	(void)a;
	sprintf(t, "%.10g", v);
	e = strchr(t, 'e');
	if (e) {
		/* strip trailing zeros of the mantissa, and exponent zeros */
		char mant[64], *p;
		int ex = atoi(e + 1);
		memcpy(mant, t, (size_t)(e - t));
		mant[e - t] = 0;
		if (strchr(mant, '.')) {
			p = mant + strlen(mant) - 1;
			while (*p == '0')
				*p-- = 0;
			if (*p == '.')
				*p = 0;
		}
		sprintf(out, "%se%c%d", mant, ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
	} else {
		strcpy(out, t);
	}
}

int main(void)
{
	int i, n = 0;

	check("1+2", 0, "3");
	check("2+3*4", 0, "14");
	check("(2+3)*4", 0, "20");
	check("10/4", 0, "2.5");
	check("1/3", 0, "0.3333333333");
	check("2/3", 0, "0.6666666667");
	check("-5+2", 0, "-3");
	check("--5", 0, "5");
	check("3*-2", 0, "-6");
	check("2(3+4)", 0, "14");
	check("(1+2)(3+4)", 0, "21");
	check("(2+3", 0, "5");
	check("((2+3)*2", 0, "10");
	check("1+2)", 0, "3");
	check("A*2", 21, "42");
	check("3A", 2, "6");
	check("A", -1.5, "-1.5");
	check("50%", 0, "0.5");
	check("50+10%", 0, "55");
	check("80-25%", 0, "60");
	check("200*10%", 0, "20");
	check("10%*200", 0, "20");
	check("1/0", 0, "Can't divide by 0");
	check("5/(2-2)", 0, "Can't divide by 0");
	check("1+", 0, "Error");
	check("*3", 0, "Error");
	check("()", 0, "Error");
	check("1..2", 0, "Error");
	check(".5+.5", 0, "1");
	check(".", 0, "0");
	check("0.1+0.2", 0, "0.3");
	check("1e3", 0, "8.154845485");	/* 1 x e x 3 */
	check("99999999999*10", 0, "1e+12");
	check("123456789*1000", 0, "1.23456789e+11");
	check("1234567890", 0, "1234567890");
	check("12345678901", 0, "1.23456789e+10");
	check("0.0001", 0, "0.0001");
	check("0.000012", 0, "1.2e-5");
	check("0.0000001", 0, "1e-7");
	check("2-2", 0, "0");
	check("-2+2", 0, "0");
	check("0*-1", 0, "0");
	check("7/7/7", 0, "0.1428571429");
	check("1000000*1000000*1000000", 0, "1e+18");
	check("9.9999999999", 0, "10");
	check("123.456", 0, "123.456");

	/* scientific, degrees */
	check("s(30)", 0, "0.5");
	check("o(60)", 0, "0.5");
	check("t(45)", 0, "1");
	check("s(180)", 0, "0");
	check("o(90)", 0, "0");
	check("s(-90)", 0, "-1");
	check("s(390)", 0, "0.5");
	check("o(-120)", 0, "-0.5");
	check("t(90)", 0, "Math error");
	check("t(135)", 0, "-1");
	check("S(0.5)", 0, "30");
	check("O(0.5)", 0, "60");
	check("T(1)", 0, "45");
	check("O(-1)", 0, "180");
	check("S(1.5)", 0, "Math error");
	check("s(30)^2+o(30)^2", 0, "1");
	/* radians */
	flags = 0;
	check("s(p)", 0, "0");
	check("o(p)", 0, "-1");
	check("s(p/6)", 0, "0.5");
	check("t(p/4)", 0, "1");
	check("4T(1)", 0, "3.141592654");
	check("S(1)", 0, "1.570796327");
	check("s(1E7)", 0, "Math error");
	flags = CALC_DEG;
	/* logs, powers, roots */
	check("l(e)", 0, "1");
	check("g(1000)", 0, "3");
	check("g(0.001)", 0, "-3");
	check("l(0)", 0, "Math error");
	check("l(-1)", 0, "Math error");
	check("r(16)", 0, "4");
	check("r(2)", 0, "1.414213562");
	check("r(-4)", 0, "Math error");
	check("2^10", 0, "1024");
	check("2^-1", 0, "0.5");
	check("-2^2", 0, "-4");
	check("(-2)^2", 0, "4");
	check("(-2)^3", 0, "-8");
	check("2^3^2", 0, "512");
	check("2^0.5", 0, "1.414213562");
	check("(-8)^(1/3)", 0, "Math error");
	check("0^0", 0, "1");
	check("0^-1", 0, "Can't divide by 0");
	check("10^(-400)", 0, "0");
	check("10^400", 0, "Number too big");
	check("e^(1)", 0, "2.718281828");
	check("e^(l(5))", 0, "5");
	check("10^(g(7))", 0, "7");
	check("5!", 0, "120");
	check("0!", 0, "1");
	check("170!", 0, "7.257415615e+306");
	check("171!", 0, "Number too big");
	check("2.5!", 0, "Math error");
	check("(-3)!", 0, "Math error");
	check("3!!", 0, "720");
	check("2p", 0, "6.283185307");
	check("p", 0, "3.141592654");
	check("e", 0, "2.718281828");
	check("2e", 0, "5.436563657");
	check("1.5E3", 0, "1500");
	check("1.5E-3", 0, "0.0015");
	check("6.022E23", 0, "6.022e+23");
	check("1E", 0, "Error");
	check("2s(30)", 0, "1");
	check("r(9)r(16)", 0, "12");
	check("s(30", 0, "0.5");
	check("s30", 0, "Error");
	check("1E400", 0, "Number too big");
	check("1E-400", 0, "0");

	/* functions against libm on random arguments */
	{
		static const struct { int fn; double lo, hi; double (*ref)(double); int deg; } F[] = {
			{ 's', -1000, 1000, sin, 0 }, { 'o', -1000, 1000, cos, 0 },
			{ 't', -1.5, 1.5, tan, 0 }, { 'S', -1, 1, asin, 0 },
			{ 'O', -1, 1, acos, 0 }, { 'T', -50, 50, atan, 0 },
			{ 'l', 1e-10, 1e10, log, 0 }, { 'g', 1e-10, 1e10, log10, 0 },
			{ 'r', 0, 1e12, sqrt, 0 }, { 'x', -700, 700, exp, 0 },
		};
		int f, bad = 0;
		double worst = 0;
		for (f = 0; f < (int)(sizeof(F) / sizeof(F[0])); ++f) {
			for (i = 0; i < 100000; ++i) {
				double u = (double)rand() / RAND_MAX, x, v, w, err;
				if (F[f].fn == 'l' || F[f].fn == 'g' || F[f].fn == 'r')
					x = exp(log(F[f].lo + 1e-300) + u * (log(F[f].hi) - log(F[f].lo + 1e-300)));
				else
					x = F[f].lo + u * (F[f].hi - F[f].lo);
				if (calc_func(F[f].fn, x, 0, &v) != CALC_OK) {
					if (bad++ < 10)
						printf("FUNC %c(%.17g) failed\n", F[f].fn, x);
					continue;
				}
				w = F[f].ref(x);
				/* relative error, or absolute near zero (sin near
				 * multiples of pi is only good to ~1e-16 * |x|) */
				err = fabs(v - w) / (fabs(w) > 1e-3 ? fabs(w) : 1e-3);
				if (F[f].fn == 's' || F[f].fn == 'o' || F[f].fn == 't')
					err = fabs(v - w) / (fabs(w) > 1e-3 ? fabs(w) : 1e-3) /
					      (fabs(x) > 1 ? fabs(x) : 1);
				if (err > worst)
					worst = err;
				if (err > 1e-13 && bad++ < 10)
					printf("FUNC %c(%.17g) = %.17g, libm %.17g\n", F[f].fn, x, v, w);
			}
		}
		/* powers */
		for (i = 0; i < 100000; ++i) {
			double x = (double)rand() / RAND_MAX * 100, y = (double)rand() / RAND_MAX * 40 - 20, v, w, err;
			if (calc_pow(x, y, &v) != CALC_OK)
				continue;
			w = pow(x, y);
			err = fabs(v - w) / (fabs(w) > 1e-300 ? fabs(w) : 1e-300);
			if (err > worst)
				worst = err;
			if (err > 1e-13 && bad++ < 10)
				printf("POW %.17g^%.17g = %.17g, libm %.17g\n", x, y, v, w);
		}
		printf("maths: worst relative error %.2g\n", worst);
		if (bad) {
			printf("%d maths failures\n", bad);
			fails++;
		}
	}

	/* format against printf on random values */
	srand(1);
	for (i = 0; i < 200000; ++i) {
		char a[64], b[64];
		double m = (double)rand() / RAND_MAX * 10.0;
		int ex = rand() % 41 - 20;
		double v = m;
		while (ex > 0) { v *= 10; ex--; }
		while (ex < 0) { v /= 10; ex++; }
		if (rand() & 1)
			v = -v;
		calc_format(v, a);
		ref_format(v, b);
		if (strcmp(a, b)) {
			if (n++ < 15)
				printf("FORMAT %.17g: got %s want %s\n", v, a, b);
		}
	}
	if (n) {
		printf("%d format mismatches\n", n);
		fails++;
	}
	printf("%s\n", fails ? "calctest: FAILED" : "calctest: ok");
	return fails != 0;
}
