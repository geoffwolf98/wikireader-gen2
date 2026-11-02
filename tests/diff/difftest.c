/*
 * difftest - the same checks compiled for the PC and for the C33.
 * The outputs must be identical; the first difference shows what the
 * old compiler (or the emulator) gets wrong.
 *
 * Reads SMALL.ZST and CLUSTER.ZST (zstd frames) through the platform
 * file functions.
 */
#include "../../src/zw.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

void out(const char *s);
int load_file(const char *name, uint8_t **data, uint32_t *len);

static char line[200];

static void hex(char *p, uint32_t v)
{
	int i;
	for (i = 7; i >= 0; --i) {
		p[i] = "0123456789abcdef"[v & 15];
		v >>= 4;
	}
}

/* "name v1 v2 ..." with 64 bit values in hex */
static void put(const char *name, int n, const uint64_t *v)
{
	char *p = line;
	int i;
	while (*name)
		*p++ = *name++;
	for (i = 0; i < n; ++i) {
		*p++ = ' ';
		hex(p, (uint32_t)(v[i] >> 32));
		hex(p + 8, (uint32_t)v[i]);
		p += 16;
	}
	*p++ = '\n';
	*p = 0;
	out(line);
}

static uint64_t rng = 0x123456789abcdef1ull;
static uint64_t next64(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static uint32_t fnv(const uint8_t *p, uint32_t n)
{
	uint32_t h = 2166136261u;
	while (n--) {
		h ^= *p++;
		h *= 16777619u;
	}
	return h;
}

static volatile int vshift;	/* keep shift amounts out of the optimiser's reach */

static void arith(void)
{
	int i;
	for (i = 0; i < 40; ++i) {
		uint64_t a = next64(), b = next64(), v[8];
		uint32_t x = (uint32_t)a, y = (uint32_t)b | 1;
		int32_t sx = (int32_t)x, sy = (int32_t)y;
		int64_t sa = (int64_t)a, sb = (int64_t)(b | 1);
		if (i & 1) {
			b >>= (i % 40);
			y >>= (i % 31);
			y |= 1;
			sy = (int32_t)y;
		}
		v[0] = a + b; v[1] = a - b; v[2] = a * b;
		v[3] = b ? a / (b | 1) : 0; v[4] = a % (b | 1);
		v[5] = (uint64_t)(sa / sb); v[6] = (uint64_t)(sa % sb);
		v[7] = (a < b) | ((sa < (int64_t)b) << 1) | ((a == b) << 2) | ((a > b) << 3);
		put("a64", 8, v);
		v[0] = x / y; v[1] = x % y; v[2] = (uint32_t)(sx / sy); v[3] = (uint32_t)(sx % sy);
		v[4] = (uint64_t)x * y; v[5] = (uint64_t)((int64_t)sx * sy);
		v[6] = (x < y) | ((sx < sy) << 1) | ((x >= y) << 2) | ((sx >= sy) << 3);
		v[7] = (uint16_t)x * (uint16_t)y;
		put("a32", 8, v);
	}
	for (i = 0; i < 64; ++i) {
		uint64_t a = next64(), v[6];
		uint32_t x = (uint32_t)a;
		vshift = i;
		v[0] = a << vshift; v[1] = a >> vshift; v[2] = (uint64_t)((int64_t)a >> vshift);
		vshift = i & 31;
		v[3] = x << vshift; v[4] = x >> vshift; v[5] = (uint32_t)((int32_t)x >> vshift);
		put("sh", 6, v);
	}
	{
		static const uint8_t bytes[] = { 0x80, 0x7f, 0xff, 0x01, 0xfe, 0x00, 0x81, 0x55 };
		volatile const uint8_t *bp = bytes;
		uint64_t v[6];
		v[0] = (uint64_t)(int64_t)(int8_t)bp[0];
		v[1] = (uint64_t)(int64_t)(int16_t)(bp[0] | (bp[2] << 8));
		v[2] = (uint64_t)(bp[1] | (bp[2] << 8) | (bp[3] << 16) | ((uint32_t)bp[4] << 24));
		v[3] = (uint64_t)(int32_t)(int8_t)bp[6] * 3;
		v[4] = (uint64_t)(uint16_t)(bp[6] << 8 | bp[7]);
		v[5] = (uint64_t)(-(int64_t)(bp[7]));
		put("bytes", 6, v);
	}
}

static void *zalloc(void *o, size_t n) { (void)o; return zw_alloc(n); }
static void zfree(void *o, void *p) { (void)o; zw_free(p); }

static void zstd_test(const char *name, uint32_t expect_len)
{
	uint8_t *src;
	uint32_t len;
	uint64_t v[4];
	ZSTD_customMem mem = { zalloc, zfree, NULL };
	if (load_file(name, &src, &len) != 0) {
		out("cannot load test file\n");
		return;
	}
	{
		/* one shot */
		ZSTD_DCtx *dc = ZSTD_createDCtx_advanced(mem);
		uint8_t *dst = zw_alloc(expect_len + 64);
		size_t r = ZSTD_decompressDCtx(dc, dst, expect_len + 64, src, len);
		v[0] = (uint64_t)(ZSTD_isError(r) ? 0xdead0000u + ZSTD_getErrorCode(r) : r);
		v[1] = ZSTD_isError(r) ? 0 : fnv(dst, (uint32_t)r);
		put(name, 2, v);
		ZSTD_freeDCtx(dc);
		zw_free(dst);
	}
	{
		/* streaming, like zim.c: 16 KiB input pieces, stable output */
		ZSTD_DStream *ds = ZSTD_createDStream_advanced(mem);
		uint8_t *dst = zw_alloc(expect_len + 64);
		ZSTD_inBuffer in;
		ZSTD_outBuffer o;
		size_t r = 0;
		uint32_t fed = 0;
		ZSTD_DCtx_setParameter(ds, ZSTD_d_stableOutBuffer, 1);
		o.dst = dst;
		o.size = expect_len + 64;
		o.pos = 0;
		while (fed < len) {
			uint32_t n = len - fed > 16384 ? 16384 : len - fed;
			in.src = src + fed;
			in.size = n;
			in.pos = 0;
			while (in.pos < in.size) {
				r = ZSTD_decompressStream(ds, &o, &in);
				if (ZSTD_isError(r))
					break;
				if (r == 0)
					break;
			}
			if (ZSTD_isError(r) || r == 0)
				break;
			fed += n;
		}
		v[0] = (uint64_t)(ZSTD_isError(r) ? 0xdead0000u + ZSTD_getErrorCode(r) : o.pos);
		v[1] = fnv(dst, (uint32_t)o.pos);
		v[2] = fed;
		put("stream", 3, v);
		ZSTD_freeDStream(ds);
		zw_free(dst);
	}
	zw_free(src);
}

void difftest(void)
{
	out("difftest start\n");
	arith();
	zstd_test("SMALL.ZST", 50000);
	zstd_test("CLUSTER.ZST", 2094509);
	out("difftest end\n");
}
