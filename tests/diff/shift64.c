/* constant 64 bit shifts and rotates, as XXH64 uses */
#include "../../src/zw.h"
void out(const char *s);
static char b[64];
static void hx(uint64_t v, const char *tag, int n)
{
	int i; char *p = b;
	*p++ = tag[0]; *p++ = (char)('0' + n / 10); *p++ = (char)('0' + n % 10); *p++ = ' ';
	for (i = 15; i >= 0; --i) p[15 - i] = "0123456789abcdef"[(v >> (i * 4)) & 15];
	p[16] = '\n'; p[17] = 0;
	out(b);
}
static volatile uint64_t seed = 0x9e3779b185ebca87ull;
#define T(n) { uint64_t x = seed; hx(x << n, "l", n); hx(x >> n, "r", n); \
	hx((x << n) | (x >> (64 - n)), "o", n); hx((uint64_t)((int64_t)x >> n), "a", n); }
void shift64(void)
{
	uint64_t x = seed, m = 0xc2b2ae3d27d4eb4full;
	T(1) T(5) T(7) T(8) T(9) T(11) T(12) T(15) T(16) T(17) T(18) T(23) T(24) T(27)
	T(29) T(31) T(32) T(33) T(37) T(40) T(47) T(48) T(55) T(56) T(63)
	hx(x * m, "m", 0); hx(x * 0x165667b19e3779f9ull, "m", 1); hx(x + m, "p", 0);
	hx(x - m, "s", 0); hx(x ^ (x >> 33), "x", 0);
	{ uint32_t y = (uint32_t)x; hx((y << 13) | (y >> 19), "w", 13); hx((y >> 7) | (y << 25), "w", 7); }
	out("shift64 end\n");
}
