/* stage by stage zstd decoding, for comparing PC and C33 */
#include "../../third_party/zstd/zstddeclib.c"
#include "../../src/zw.h"

void out(const char *s);
int load_file(const char *name, uint8_t **data, uint32_t *len);

static char buf[256];
static void hex8(char *p, uint32_t v)
{
	int i;
	for (i = 7; i >= 0; --i) { p[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
}
static void pv(const char *tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	char *p = buf;
	while (*tag) *p++ = *tag++;
	*p++ = ' '; hex8(p, a); p += 8;
	*p++ = ' '; hex8(p, b); p += 8;
	*p++ = ' '; hex8(p, c); p += 8;
	*p++ = ' '; hex8(p, d); p += 8;
	*p++ = '\n'; *p = 0;
	out(buf);
}

static uint32_t h32(const void *p, size_t n)
{
	const uint8_t *b = p;
	uint32_t h = 2166136261u;
	while (n--) { h ^= *b++; h *= 16777619u; }
	return h;
}

void zstdstage(void)
{
	uint8_t *src, *dst;
	uint32_t len;
	ZSTD_frameHeader fh;
	ZSTD_DCtx *dc;
	size_t r, pos = 0, op = 0;
	int k;

	if (load_file("SMALL.ZST", &src, &len) != 0) { out("no file\n"); return; }
	r = ZSTD_getFrameHeader(&fh, src, len);
	pv("hdr", (uint32_t)r, (uint32_t)fh.windowSize, (uint32_t)fh.frameContentSize, fh.headerSize);
	pv("hdr2", fh.dictID, fh.checksumFlag, (uint32_t)fh.frameType, (uint32_t)fh.blockSizeMax);

	/* XXH64 and the bit stream helpers on known data */
	pv("xxh64", (uint32_t)(XXH64(src, len, 0) >> 32), (uint32_t)XXH64(src, len, 0),
	   (uint32_t)XXH64(src, 7, 1), ZSTD_highbit32(0x00012345));
	pv("bits", ZSTD_countTrailingZeros32(0x00012340), ZSTD_countLeadingZeros32(0x00012340),
	   ZSTD_highbit32(1), ZSTD_highbit32(0x80000000u));
	{
		BIT_DStream_t bd;
		size_t e = BIT_initDStream(&bd, src + 20, 64);
		uint32_t v1 = (uint32_t)BIT_readBits(&bd, 5);
		uint32_t v2 = (uint32_t)BIT_readBits(&bd, 13);
		BIT_reloadDStream(&bd);
		pv("bitstream", (uint32_t)e, v1, v2, (uint32_t)BIT_readBits(&bd, 9));
	}

	dc = ZSTD_createDCtx();
	dst = malloc(200000);
	ZSTD_decompressBegin(dc);
	for (k = 0; k < 40; ++k) {
		size_t want = ZSTD_nextSrcSizeToDecompress(dc);
		if (!want)
			break;
		r = ZSTD_decompressContinue(dc, dst + op, 200000 - op, src + pos, want);
		pv("step", (uint32_t)want, (uint32_t)(ZSTD_isError(r) ? 0xdead0000u + ZSTD_getErrorCode(r) : r),
		   (uint32_t)pos, h32(dst, op + (ZSTD_isError(r) ? 0 : r)));
		if (ZSTD_isError(r))
			break;
		pos += want;
		op += r;
	}
	/* literals of the first block, by hand */
	{
		blockProperties_t bp;
		size_t bs, hs = ZSTD_frameHeaderSize(src, len);
		const BYTE *ip;
		bs = ZSTD_getcBlockSize(src + hs, len - hs, &bp);
		pv("block", (uint32_t)hs, (uint32_t)bs, bp.blockType, bp.lastBlock);
		ip = src + hs + ZSTD_blockHeaderSize;
		ZSTD_decompressBegin(dc);
		r = ZSTD_decodeLiteralsBlock(dc, ip, bs, dst, 200000, not_streaming);
		pv("lits", (uint32_t)(ZSTD_isError(r) ? 0xdead0000u + ZSTD_getErrorCode(r) : r),
		   (uint32_t)dc->litSize, h32(dc->litPtr, dc->litSize), dc->litEntropy);
		if (!ZSTD_isError(r)) {
			int nbSeq;
			size_t s = ZSTD_decodeSeqHeaders(dc, &nbSeq, ip + r, bs - r);
			pv("seqhdr", (uint32_t)(ZSTD_isError(s) ? 0xdead0000u + ZSTD_getErrorCode(s) : s),
			   (uint32_t)nbSeq, h32(dc->entropy.LLTable, sizeof(dc->entropy.LLTable)),
			   h32(dc->entropy.MLTable, sizeof(dc->entropy.MLTable)));
			pv("oftable", h32(dc->entropy.OFTable, sizeof(dc->entropy.OFTable)),
			   h32(dc->entropy.hufTable, sizeof(dc->entropy.hufTable)), 0, 0);
		}
	}
	out("zstdstage end\n");
}
