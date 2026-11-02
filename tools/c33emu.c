/*
 * c33emu - run the real WikiReader build of zimwiki (an ELF for the
 * Epson C33 PE core) on a PC.
 *
 *   c33emu [-t] INIT.ELF CARD.img SCRIPT [STATEDIR]
 *
 * It emulates the CPU instructions gcc 3.3 -mc33pe emits, and answers
 * grifo's system calls (int 1 + syscall number) from the host: SD
 * sectors come from CARD.img, the frame buffer is captured for
 * screenshots, touches and buttons come from SCRIPT (same commands as
 * zimwiki-headless).  Any illegal or misaligned access stops with a
 * register dump.
 *
 * It also counts instructions and memory accesses, which gives a rough
 * idea of the time on the real 48 MHz device.
 */
#define _FILE_OFFSET_BITS 64
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#define SDRAM_BASE 0x10000000u
#define SDRAM_SIZE (32u << 20)
#define LOW_SIZE   (1u << 20)
#define FB_ADDR    0x00080000u
#define SCRATCH    0x00001000u
#define MAGIC_RET  0x00000100u
#define MAIN_STACK (SDRAM_BASE + SDRAM_SIZE - 4)
#define DP_USER    0x10040000u
#define MHZ        48

static uint8_t *sdram, *low;
static FILE *card;
static const char *state_dir = "build/emu-state";

typedef struct cpu {
	uint32_t r[16], sp, psr, alr, ahr, pc;
	uint32_t ext[2];
	int next;
} cpu_t;

static cpu_t C;
static uint64_t icount, mem_reads, mem_writes, taken;
static uint64_t last_fb_write;
static uint64_t now_cycles(void);
static int trace, halted, failures, profile;
static int32_t *prof_map;
static uint64_t *prof_count;
static uint32_t prof_base, prof_len;
static uint32_t heap_start, heap_end;

/* ------------------------------------------------------------------ */
/* memory */

static void fault(const char *what, uint32_t addr) __attribute__((noreturn));

static uint8_t *ptr(uint32_t a, uint32_t n)
{
	if (a >= SDRAM_BASE && a - SDRAM_BASE <= SDRAM_SIZE - n)
		return sdram + (a - SDRAM_BASE);
	if (a < LOW_SIZE && a + n <= LOW_SIZE && a >= 0x200)
		return low + a;
	fault("bad address", a);
}

static uint32_t rd8(uint32_t a) { mem_reads++; return *ptr(a, 1); }
static uint32_t rd16(uint32_t a)
{
	uint8_t *p;
	if (a & 1) fault("misaligned 16 bit read", a);
	mem_reads++;
	p = ptr(a, 2);
	return p[0] | (p[1] << 8);
}
static uint32_t rd32(uint32_t a)
{
	uint8_t *p;
	if (a & 3) fault("misaligned 32 bit read", a);
	mem_reads++;
	p = ptr(a, 4);
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void fbmark(uint32_t a) { if (a - FB_ADDR < 208 * 32) last_fb_write = now_cycles(); }
static void wr8(uint32_t a, uint32_t v) { mem_writes++; fbmark(a); *ptr(a, 1) = (uint8_t)v; }
static void wr16(uint32_t a, uint32_t v)
{
	uint8_t *p;
	if (a & 1) fault("misaligned 16 bit write", a);
	mem_writes++;
	fbmark(a);
	p = ptr(a, 2);
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint32_t a, uint32_t v)
{
	uint8_t *p;
	if (a & 3) fault("misaligned 32 bit write", a);
	mem_writes++;
	fbmark(a);
	p = ptr(a, 4);
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------ */
/* symbols (for fault reports) */

typedef struct sym { uint32_t addr; char *name; } sym_t;
static sym_t *syms;
static int nsyms;

static int sym_cmp(const void *a, const void *b)
{
	const sym_t *x = a, *y = b;
	return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static const char *sym_name(uint32_t a, uint32_t *off)
{
	int lo = 0, hi = nsyms - 1, best = -1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (syms[mid].addr <= a) { best = mid; lo = mid + 1; } else hi = mid - 1;
	}
	if (best < 0) { *off = a; return "?"; }
	*off = a - syms[best].addr;
	return syms[best].name;
}

static uint32_t sym_addr(const char *name)
{
	int i;
	for (i = 0; i < nsyms; ++i)
		if (!strcmp(syms[i].name, name))
			return syms[i].addr;
	return 0;
}

/* the last jumps, calls and returns, for crash reports */
#define HIST 24
static uint32_t hist_from[HIST], hist_to[HIST];
static unsigned hist_n;
static void note_jump(uint32_t from, uint32_t to)
{
	hist_from[hist_n % HIST] = from;
	hist_to[hist_n % HIST] = to;
	hist_n++;
}

static void fault(const char *what, uint32_t addr)
{
	uint32_t off;
	const char *s = sym_name(C.pc, &off);
	int i;
	fprintf(stderr, "\n*** %s 0x%08x at pc 0x%08x (%s+0x%x) after %llu instructions\n",
		what, addr, C.pc, s, off, (unsigned long long)icount);
	for (i = 0; i < 16; ++i)
		fprintf(stderr, "r%-2d %08x%s", i, C.r[i], i % 4 == 3 ? "\n" : "  ");
	fprintf(stderr, "sp  %08x  psr %08x  alr %08x  ahr %08x\n", C.sp, C.psr, C.alr, C.ahr);
	/* return addresses on the stack */
	fprintf(stderr, "stack:");
	for (i = 0; i < 24 && C.sp + i * 4 < MAIN_STACK; ++i) {
		uint32_t v, o;
		uint8_t *p = sdram + (C.sp + i * 4 - SDRAM_BASE);
		v = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
		if (v >= 0x10040000 && v < heap_start)
			fprintf(stderr, " %s+0x%x", sym_name(v, &o), o), (void)o;
	}
	fprintf(stderr, "\nlast jumps (oldest first):\n");
	for (i = (int)(hist_n > HIST ? hist_n - HIST : 0); i < (int)hist_n; ++i) {
		uint32_t o1, o2;
		const char *a = sym_name(hist_from[i % HIST], &o1);
		fprintf(stderr, "  %s+0x%x", a, o1);
		a = sym_name(hist_to[i % HIST], &o2);
		fprintf(stderr, " -> %s+0x%x\n", a, o2);
	}
	exit(3);
}

/* ------------------------------------------------------------------ */
/* ELF loading (section headers, like grifo's loader) */

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static uint32_t load_elf(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t *e;
	long n;
	uint32_t shoff, shnum, shentsize, i, entry, end = 0;
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	e = malloc((size_t)n);
	if (fread(e, 1, (size_t)n, f) != (size_t)n) exit(1);
	fclose(f);
	if (memcmp(e, "\177ELF", 4)) { fprintf(stderr, "not ELF\n"); exit(1); }
	entry = le32(e + 24);
	shoff = le32(e + 32);
	shentsize = le16(e + 46);
	shnum = le16(e + 48);
	for (i = 0; i < shnum; ++i) {
		const uint8_t *s = e + shoff + i * shentsize;
		uint32_t type = le32(s + 4), flags = le32(s + 8), addr = le32(s + 12);
		uint32_t off = le32(s + 16), size = le32(s + 20), link = le32(s + 24);
		if (type == 2) {	/* SHT_SYMTAB */
			const uint8_t *st = e + off;
			const uint8_t *ls = e + shoff + link * shentsize;
			const char *strtab = (const char *)e + le32(ls + 16);
			uint32_t k, cnt = size / 16;
			syms = malloc(sizeof(sym_t) * cnt);
			for (k = 0; k < cnt; ++k) {
				const uint8_t *y = st + k * 16;
				uint32_t v = le32(y + 4);
				const char *nm = strtab + le32(y);
				if (v >= 0x10040000 && *nm && nm[0] != '$') {
					syms[nsyms].addr = v;
					syms[nsyms].name = strdup(nm);
					nsyms++;
				}
			}
		}
		if (!(flags & 2) || !size)	/* SHF_ALLOC */
			continue;
		if (type == 8)		/* NOBITS */
			memset(ptr(addr, size), 0, size);
		else
			memcpy(ptr(addr, size), e + off, size);
		if (addr >= SDRAM_BASE && addr + size > end)
			end = addr + size;
	}
	qsort(syms, (size_t)nsyms, sizeof(sym_t), sym_cmp);
	free(e);
	heap_start = (end + 1023) & ~1023u;
	return entry;
}

/* ------------------------------------------------------------------ */
/* heap for memory_allocate: first fit over a sorted block list */

typedef struct blk { uint32_t addr, size; int used; } blk_t;
static blk_t *blks;
static int nblks, capblks;
static uint32_t heap_in_use, heap_peak;

static void heap_init(void)
{
	heap_end = MAIN_STACK - (1u << 20);
	capblks = 1024;
	blks = malloc(sizeof(blk_t) * capblks);
	blks[0].addr = heap_start;
	blks[0].size = heap_end - heap_start;
	blks[0].used = 0;
	nblks = 1;
}

static uint32_t heap_alloc(uint32_t size)
{
	int i;
	size = (size + 15) & ~15u;
	if (!size) size = 16;
	for (i = 0; i < nblks; ++i) {
		if (blks[i].used || blks[i].size < size)
			continue;
		if (blks[i].size > size) {
			if (nblks == capblks) {
				capblks *= 2;
				blks = realloc(blks, sizeof(blk_t) * capblks);
			}
			memmove(blks + i + 1, blks + i, sizeof(blk_t) * (nblks - i));
			nblks++;
			blks[i + 1].addr = blks[i].addr + size;
			blks[i + 1].size = blks[i].size - size;
			blks[i + 1].used = 0;
			blks[i].size = size;
		}
		blks[i].used = 1;
		heap_in_use += size;
		if (heap_in_use > heap_peak) heap_peak = heap_in_use;
		/* fill with junk so code relying on zeroed memory is caught */
		memset(ptr(blks[i].addr, size), 0xa5, size);
		return blks[i].addr;
	}
	return 0;
}

static void heap_free(uint32_t a)
{
	int i;
	if (!a) return;
	for (i = 0; i < nblks; ++i) {
		if (blks[i].addr == a && blks[i].used) {
			blks[i].used = 0;
			heap_in_use -= blks[i].size;
			if (i + 1 < nblks && !blks[i + 1].used) {
				blks[i].size += blks[i + 1].size;
				memmove(blks + i + 1, blks + i + 2, sizeof(blk_t) * (nblks - i - 2));
				nblks--;
			}
			if (i > 0 && !blks[i - 1].used) {
				blks[i - 1].size += blks[i].size;
				memmove(blks + i, blks + i + 1, sizeof(blk_t) * (nblks - i - 1));
				nblks--;
			}
			return;
		}
	}
	fault("free of unknown pointer", a);
}

/* ------------------------------------------------------------------ */
/* events and time */

enum { EV_NONE, EV_KEY, EV_DOWN, EV_MOVE, EV_UP, EV_BDOWN, EV_BUP };
typedef struct gev { int type, x, y, code; uint64_t due; } gev_t;
static gev_t q[4096];
static int qh, qt;
static uint64_t sleep_cycles;	/* time spent asleep in event_wait */

static uint64_t now_cycles(void)
{
	/* one cycle per instruction plus one per memory access is a crude
	 * but useful model for code running from SDRAM */
	return icount + mem_reads + mem_writes + taken + sleep_cycles;
}

static uint32_t now_ms(void) { return (uint32_t)(now_cycles() / (MHZ * 1000)); }

static void enqueue(int type, int x, int y, int code, uint64_t due)
{
	q[qt].type = type; q[qt].x = x; q[qt].y = y; q[qt].code = code; q[qt].due = due;
	qt = (qt + 1) % 4096;
}

static int deliver(uint32_t evp)
{
	gev_t *e;
	if (qh == qt || q[qh].due > now_cycles())
		return 0;
	e = &q[qh];
	qh = (qh + 1) % 4096;
	wr32(evp, (uint32_t)e->type);
	wr32(evp + 4, (uint32_t)(now_cycles() * 60 / MHZ));
	if (e->type == EV_DOWN || e->type == EV_MOVE || e->type == EV_UP) {
		wr32(evp + 8, (uint32_t)e->x);
		wr32(evp + 12, (uint32_t)e->y);
	} else if (e->type == EV_KEY) {
		wr8(evp + 8, (uint32_t)e->code);
	} else {
		wr32(evp + 8, (uint32_t)e->code);
	}
	return e->type;
}

/* ------------------------------------------------------------------ */
/* host side of the script */

static FILE *script;
static int script_done;
static void run_until(uint32_t stop_pc, uint64_t max);

static void guest_str(uint32_t a, char *out, int cap)
{
	int i;
	for (i = 0; i < cap - 1; ++i) {
		out[i] = (char)*ptr(a + i, 1);
		if (!out[i]) break;
	}
	out[cap - 1] = 0;
}

/* call a guest function with up to 2 arguments from inside a syscall */
static uint32_t guest_call(uint32_t fn, uint32_t a0, uint32_t a1)
{
	cpu_t saved = C;
	uint32_t r;
	C.r[6] = a0;
	C.r[7] = a1;
	C.next = 0;
	C.sp -= 64;
	C.sp -= 4;
	wr32(C.sp, MAGIC_RET);
	C.pc = fn;
	run_until(MAGIC_RET, 2000000000ull);
	r = C.r[4];
	C = saved;
	return r;
}

static void describe(char *out, int cap)
{
	uint32_t fn = sym_addr("app_describe");
	if (!fn) {
		fprintf(stderr, "c33emu: this binary has no symbols; use device/build/init.elf, "
			"not the stripped init.app\n");
		exit(2);
	}
	guest_call(fn, SCRATCH, 400);
	guest_str(SCRATCH, out, cap);
}

static void write_png(const char *path)
{
	int w = 480, h = 416, x, y;
	size_t rl = (size_t)(w * 3 + 1) * h;
	uint8_t *raw = malloc(rl), *z, hdr[13];
	uLongf zl = compressBound(rl);
	FILE *f;
	uint8_t *fb = low + FB_ADDR;
	for (y = 0; y < h; ++y) {
		uint8_t *row = raw + (size_t)y * (w * 3 + 1);
		row[0] = 0;
		for (x = 0; x < w; ++x) {
			int sx = x / 2, sy = y / 2;
			int on = fb[sy * 32 + (sx >> 3)] & (0x80 >> (sx & 7));
			row[1 + x * 3] = on ? 0x20 : 0xc8;
			row[2 + x * 3] = on ? 0x28 : 0xd0;
			row[3 + x * 3] = on ? 0x24 : 0xc0;
		}
	}
	z = malloc(zl);
	compress2(z, &zl, raw, rl, 9);
	f = fopen(path, "wb");
	if (!f) return;
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
#define CHUNK(t, d, n) do { uint8_t b[8]; uint32_t c, L = (uint32_t)(n); \
	b[0]=L>>24;b[1]=L>>16;b[2]=L>>8;b[3]=L;memcpy(b+4,t,4);fwrite(b,1,8,f); \
	if (L) fwrite(d,1,L,f); c=crc32(0,b+4,4); c=crc32(c,d,L); \
	b[0]=c>>24;b[1]=c>>16;b[2]=c>>8;b[3]=c;fwrite(b,1,4,f);} while (0)
	hdr[0] = 0; hdr[1] = 0; hdr[2] = w >> 8; hdr[3] = w & 255;
	hdr[4] = 0; hdr[5] = 0; hdr[6] = h >> 8; hdr[7] = h & 255;
	hdr[8] = 8; hdr[9] = 2; hdr[10] = hdr[11] = hdr[12] = 0;
	CHUNK("IHDR", hdr, 13);
	CHUNK("IDAT", z, zl);
	CHUNK("IEND", "", 0);
	fclose(f);
	free(raw);
	free(z);
}

static uint64_t line_icount, line_cycles, line_sectors;
static uint64_t sectors_read;
static char cur_line[1024];

static void report(void)
{
	char d[512];
	double ms;
	if (!cur_line[0])
		return;
	describe(d, sizeof(d));
	ms = last_fb_write > line_cycles ? (double)(last_fb_write - line_cycles) / (MHZ * 1000.0) : 0;
	printf("[screen %7.1f ms, %9llu instr, %5llu sectors] %-30.30s -> %s\n", ms,
	       (unsigned long long)(icount - line_icount),
	       (unsigned long long)(sectors_read - line_sectors), cur_line, d);
	cur_line[0] = 0;
}

static void prof_show(void)
{
	if (!profile)
		return;
	{
		/* fold local labels (.L123) into the function before them */
		uint64_t tot = 0;
		int i, j, top[25] = { 0 };
		for (i = 0; i < nsyms; ++i) {
			if (syms[i].name[0] == '.' && i > 0) {
				int k = i - 1;
				while (k > 0 && syms[k].name[0] == '.') k--;
				prof_count[k] += prof_count[i];
				prof_count[i] = 0;
			}
			tot += prof_count[i];
		}
		for (j = 0; j < 25; ++j) {
			int best = -1;
			for (i = 0; i < nsyms; ++i) {
				int used = 0, m;
				for (m = 0; m < j; ++m) if (top[m] == i) used = 1;
				if (!used && (best < 0 || prof_count[i] > prof_count[best])) best = i;
			}
			top[j] = best;
			if (best >= 0 && prof_count[best])
				printf("  %5.1f%%  %12llu  %s\n", 100.0 * prof_count[best] / (tot ? tot : 1),
				       (unsigned long long)prof_count[best], syms[best].name);
		}
	}
}

static void prof_reset(void)
{
	if (profile)
		memset(prof_count, 0, sizeof(uint64_t) * ((size_t)nsyms + 1));
}

/* run script commands until something is queued; returns 0 at the end */
static int script_step(void)
{
	char line[1024], cmd[32], arg[900], d[512];
	int a, b, c2, dd, e;
	while (fgets(line, sizeof(line), script)) {
		uint64_t t = now_cycles();
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#')
			continue;
		report();
		strncpy(cur_line, line, sizeof(cur_line) - 1);
		line_icount = icount;
		line_cycles = now_cycles();
		line_sectors = sectors_read;
		arg[0] = 0;
		sscanf(line, "%31s %899[^\n]", cmd, arg);
		if (!strcmp(cmd, "tap") && sscanf(arg, "%d %d", &a, &b) == 2) {
			enqueue(EV_DOWN, a, b, 0, t);
			enqueue(EV_UP, a, b, 0, t + 60 * MHZ * 1000ull);
			return 1;
		} else if (!strcmp(cmd, "drag") && sscanf(arg, "%d %d %d %d %d", &a, &b, &c2, &dd, &e) == 5) {
			int steps = e / 20 > 1 ? e / 20 : 1, i;
			enqueue(EV_DOWN, a, b, 0, t);
			for (i = 1; i <= steps; ++i)
				enqueue(EV_MOVE, a + (c2 - a) * i / steps, b + (dd - b) * i / steps, 0,
					t + (uint64_t)i * 20 * MHZ * 1000);
			enqueue(EV_UP, c2, dd, 0, t + (uint64_t)steps * 20 * MHZ * 1000 + 1);
			return 1;
		} else if (!strcmp(cmd, "button")) {
			int code = !strcmp(arg, "random") ? 0 : !strcmp(arg, "search") ? 1 :
				   !strcmp(arg, "history") ? 2 : 3;
			uint64_t tb = t + 150 * MHZ * 1000ull;	/* nobody presses again within 80 ms */
			enqueue(EV_BDOWN, 0, 0, code, tb);
			enqueue(EV_BUP, 0, 0, code, tb + 80 * MHZ * 1000ull);
			return 1;
		} else if (!strcmp(cmd, "bounce")) {
			/* a light press with contact bounce: down/up/down/up */
			int code = !strcmp(arg, "random") ? 0 : !strcmp(arg, "search") ? 1 :
				   !strcmp(arg, "history") ? 2 : 3;
			uint64_t tb = t + 150 * MHZ * 1000ull;	/* nobody presses again within 80 ms */
			uint64_t ms = MHZ * 1000ull;
			enqueue(EV_BDOWN, 0, 0, code, tb);
			enqueue(EV_BUP, 0, 0, code, tb + 5 * ms);
			enqueue(EV_BDOWN, 0, 0, code, tb + 12 * ms);
			enqueue(EV_BUP, 0, 0, code, tb + 60 * ms);
			enqueue(EV_BDOWN, 0, 0, code, tb + 70 * ms);
			enqueue(EV_BUP, 0, 0, code, tb + 75 * ms);
			return 1;
		} else if (!strcmp(cmd, "hold")) {
			char nm[32];
			int ms = 700, code;
			sscanf(arg, "%31s %d", nm, &ms);
			code = !strcmp(nm, "random") ? 0 : !strcmp(nm, "search") ? 1 :
			       !strcmp(nm, "history") ? 2 : 3;
			uint64_t tb = t + 150 * MHZ * 1000ull;
			enqueue(EV_BDOWN, 0, 0, code, tb);
			enqueue(EV_BUP, 0, 0, code, tb + (uint64_t)ms * MHZ * 1000);
			return 1;
		} else if (!strcmp(cmd, "type")) {
			char *p;
			for (p = arg; *p; ++p)
				enqueue(EV_KEY, 0, 0, (unsigned char)*p, t);
			return 1;
		} else if (!strcmp(cmd, "wait") && sscanf(arg, "%d", &a) == 1) {
			sleep_cycles += (uint64_t)a * MHZ * 1000;
		} else if (!strcmp(cmd, "shot")) {
			write_png(arg);
		} else if (!strcmp(cmd, "expect")) {
			describe(d, sizeof(d));
			if (!strstr(d, arg)) {
				printf("FAIL expect \"%s\" got: %s\n", arg, d);
				failures++;
			}
			cur_line[0] = 0;
		} else if (!strcmp(cmd, "profreset")) {
			prof_reset();
			cur_line[0] = 0;
		} else if (!strcmp(cmd, "profshow")) {
			prof_show();
			cur_line[0] = 0;
		} else if (!strcmp(cmd, "describe")) {
			/* reported */
		} else if (!strcmp(cmd, "stats")) {
			printf("    %llu instructions, %llu reads, %llu writes, %u KB heap in use (peak %u KB), "
			       "%llu sectors\n", (unsigned long long)icount, (unsigned long long)mem_reads,
			       (unsigned long long)mem_writes, heap_in_use >> 10, heap_peak >> 10,
			       (unsigned long long)sectors_read);
			cur_line[0] = 0;
		} else {
			printf("? %s\n", line);
			failures++;
			cur_line[0] = 0;
		}
	}
	report();
	script_done = 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* grifo system calls */

#define FILES 8
static FILE *files[FILES];

static void state_path(uint32_t name, char *out, size_t cap)
{
	char n[64];
	guest_str(name, n, sizeof(n));
	snprintf(out, cap, "%s/%s", state_dir, n);
}

/* grifo's watchdog resets the unit after 20 s without a kernel call that
 * feeds it (event_get, event_wait, delay_us, watchdog).  The emulator's
 * timing is an estimate (real hardware may be 2 to 3 times slower), so it
 * complains at a quarter of that. */
#define WATCHDOG_LIMIT (5ull * MHZ * 1000000)
static uint64_t last_kick, worst_gap, last_frame;
static int nframes;

static void watchdog_kick(void)
{
	uint64_t now = now_cycles(), gap = now - last_kick;
	if (gap > worst_gap)
		worst_gap = gap;
	if (gap > WATCHDOG_LIMIT) {
		printf("FAIL watchdog: %.1f s without feeding it (resets after 20 s on the device)\n",
		       (double)gap / (MHZ * 1e6));
		failures++;
	}
	last_kick = now;
}

static void grifo_syscall(uint32_t num)
{
	uint32_t *r = C.r, a = r[6], b = r[7], c = r[8];
	char path[600];
	if (num == 30 || num == 39 || num == 44 || num == 45)
		watchdog_kick();
	switch (num) {
	case 39:		/* watchdog */
		if (getenv("C33_FRAMES") && last_fb_write > last_frame) {
			/* C33_FRAMES=prefix: a PNG whenever the screen changed
			 * between watchdog calls (to see progress displays) */
			char fn[600];
			snprintf(fn, sizeof(fn), "%s%03d.png", getenv("C33_FRAMES"), nframes++);
			write_png(fn);
			last_frame = now_cycles();
		}
		break;
	case 1: {		/* debug_print */
		char s[256];
		guest_str(a, s, sizeof(s));
		fprintf(stderr, "%s", s);
		break;
	}
	case 18:		/* power_off */
		report();
		printf("power off\n");
		halted = 1;
		break;
	case 30:		/* delay_us */
		sleep_cycles += (uint64_t)a * MHZ;
		break;
	case 34:		/* timer_get: 60 counts per microsecond */
		r[4] = (uint32_t)(now_cycles() * 60 / MHZ);
		break;
	case 44:		/* event_get */
		r[4] = (uint32_t)deliver(a);
		break;
	case 45:		/* event_wait: sleep until the next event */
		for (;;) {
			if (qh != qt) {
				if (q[qh].due > now_cycles())
					sleep_cycles += q[qh].due - now_cycles();
				r[4] = (uint32_t)deliver(a);
				break;
			}
			if (script_done || !script_step()) {
				report();
				halted = 1;
				r[4] = 0;
				break;
			}
		}
		break;
	case 50: case 52:	/* lcd_get_framebuffer, lcd_set_default_framebuffer */
		r[4] = FB_ADDR;
		break;
	case 100: {		/* file_close_all */
		int i;
		for (i = 0; i < FILES; ++i)
			if (files[i]) { fclose(files[i]); files[i] = NULL; }
		break;
	}
	case 103: {		/* file_size(name, &len) */
		struct stat st;
		state_path(a, path, sizeof(path));
		if (stat(path, &st) == 0) {
			wr32(b, (uint32_t)st.st_size);
			r[4] = 0;
		} else {
			r[4] = (uint32_t)-2;
		}
		break;
	}
	case 102:		/* file_delete */
		state_path(a, path, sizeof(path));
		r[4] = unlink(path) == 0 ? 0 : (uint32_t)-4;
		break;
	case 110: case 111: {	/* file_create, file_open */
		int i;
		state_path(a, path, sizeof(path));
		if (num == 110) {
			char *slash = strrchr(path, '/');
			if (slash && slash > path + strlen(state_dir)) {
				*slash = 0;
				mkdir(path, 0777);
				*slash = '/';
			}
		}
		r[4] = (uint32_t)-6;
		if (num == 110 && access(path, F_OK) == 0) {
			r[4] = (uint32_t)-8;	/* like FatFs FA_CREATE_NEW: FR_EXIST */
			break;
		}
		for (i = 0; i < FILES; ++i) {
			if (!files[i]) {
				files[i] = fopen(path, num == 110 ? "wb" : (b & 2) ? "r+b" : "rb");
				r[4] = files[i] ? (uint32_t)i : (uint32_t)-2;
				break;
			}
		}
		break;
	}
	case 112:		/* file_close */
		if (a < FILES && files[a]) { fclose(files[a]); files[a] = NULL; r[4] = 0; }
		else r[4] = (uint32_t)-12;
		break;
	case 113:		/* file_read */
		r[4] = a < FILES && files[a] ? (uint32_t)fread(ptr(b, c), 1, c, files[a]) : (uint32_t)-12;
		break;
	case 114:		/* file_write */
		r[4] = a < FILES && files[a] ? (uint32_t)fwrite(ptr(b, c), 1, c, files[a]) : (uint32_t)-12;
		break;
	case 115:		/* file_sync */
		if (a < FILES && files[a]) fflush(files[a]);
		r[4] = 0;
		break;
	case 130:		/* sector_read(lba, buf, count) */
		if (fseeko(card, (off_t)a * 512, SEEK_SET) == 0 && c > 0 && c <= 255 &&
		    fread(ptr(b, c * 512), 512, c, card) == c) {
			r[4] = 0;
			sectors_read += c;
			/* SD over SPI: ~1 MB/s plus a command overhead */
			sleep_cycles += (uint64_t)c * 512 * MHZ + 300 * MHZ;
		} else {
			r[4] = (uint32_t)-8;
		}
		break;
	case 140:		/* memory_allocate */
		r[4] = heap_alloc(a);
		break;
	case 141:		/* memory_free */
		heap_free(a);
		break;
	default:
		fault("unimplemented syscall", num);
	}
}

/* ------------------------------------------------------------------ */
/* the CPU */

enum { N = 1, Z = 2, V = 4, CF = 8 };

static void nz(uint32_t v)
{
	C.psr &= ~(uint32_t)(N | Z);
	if (v & 0x80000000u) C.psr |= N;
	if (!v) C.psr |= Z;
}

static uint32_t do_add(uint32_t a, uint32_t b)
{
	uint32_t r = a + b;
	C.psr &= ~(uint32_t)(N | Z | V | CF);
	if (r & 0x80000000u) C.psr |= N;
	if (!r) C.psr |= Z;
	if (((a ^ r) & (b ^ r)) & 0x80000000u) C.psr |= V;
	if (r < a) C.psr |= CF;
	return r;
}

static uint32_t do_sub(uint32_t a, uint32_t b)
{
	uint32_t r = a - b;
	C.psr &= ~(uint32_t)(N | Z | V | CF);
	if (r & 0x80000000u) C.psr |= N;
	if (!r) C.psr |= Z;
	if (((a ^ b) & (a ^ r)) & 0x80000000u) C.psr |= V;
	if (a < b) C.psr |= CF;
	return r;
}

static uint32_t logic(uint32_t v)
{
	nz(v);
	C.psr &= ~(uint32_t)V;
	return v;
}

static uint32_t shift(int op, uint32_t v, uint32_t n)
{
	n &= 31;
	switch (op) {
	case 0: v = v >> n; break;				/* srl */
	case 1: case 3: v = v << n; break;			/* sll, sla */
	case 2: v = (uint32_t)((int32_t)v >> n); break;		/* sra */
	case 4: v = n ? (v >> n) | (v << (32 - n)) : v; break;	/* rr */
	case 5: v = n ? (v << n) | (v >> (32 - n)) : v; break;	/* rl */
	default: fault("bad shift", (uint32_t)op);
	}
	nz(v);
	return v;
}

static int cond(int c)
{
	int n = !!(C.psr & N), z = !!(C.psr & Z), v = !!(C.psr & V), cf = !!(C.psr & CF);
	switch (c) {
	case 4: return !z && n == v;	/* gt */
	case 5: return n == v;		/* ge */
	case 6: return n != v;		/* lt */
	case 7: return z || n != v;	/* le */
	case 8: return !cf && !z;	/* ugt */
	case 9: return !cf;		/* uge */
	case 10: return cf;		/* ult */
	case 11: return cf || z;	/* ule */
	case 12: return z;		/* eq */
	case 13: return !z;		/* ne */
	}
	return 1;
}

static uint32_t sext(uint32_t v, int bits)
{
	uint32_t m = 1u << (bits - 1);
	v &= (bits == 32) ? 0xffffffffu : ((1u << bits) - 1);
	return (v ^ m) - m;
}

/* immediate for class 3 (imm6 / sign6 with up to two ext) */
static uint32_t imm6x(uint32_t imm6, int is_signed)
{
	switch (C.next) {
	case 0: return is_signed ? sext(imm6, 6) : imm6;
	case 1: { uint32_t v = (C.ext[0] << 6) | imm6; return is_signed ? sext(v, 19) : v; }
	default: return (C.ext[0] << 19) | (C.ext[1] << 6) | imm6;
	}
}

/* displacement for [%rb] and 3 operand class 1 forms */
static uint32_t disp_rb(void)
{
	return C.next == 0 ? 0 : C.next == 1 ? C.ext[0] : (C.ext[0] << 13) | C.ext[1];
}

static uint32_t sp_disp(uint32_t imm6, uint32_t size)
{
	switch (C.next) {
	case 0: return imm6 * size;
	case 1: return (C.ext[0] << 6) | imm6;
	default: return (C.ext[0] << 19) | (C.ext[1] << 6) | imm6;
	}
}

static uint32_t jdisp(uint32_t sign8)
{
	switch (C.next) {
	case 0: return sext(sign8, 8) << 1;
	case 1: return sext((C.ext[0] << 9) | (sign8 << 1), 22);
	default: return ((C.ext[0] >> 3) << 22) | (C.ext[1] << 9) | (sign8 << 1);
	}
}

static uint32_t load(int kind, uint32_t a)
{
	switch (kind) {
	case 0: return sext(rd8(a), 8);
	case 1: return rd8(a);
	case 2: return sext(rd16(a), 16);
	case 3: return rd16(a);
	default: return rd32(a);
	}
}

static void store(int kind, uint32_t a, uint32_t v)
{
	if (kind == 5) wr8(a, v);
	else if (kind == 6) wr16(a, v);
	else wr32(a, v);
}

static uint32_t *special(int n)
{
	switch (n) {
	case 0: return &C.psr;
	case 1: return &C.sp;
	case 2: return &C.alr;
	case 3: return &C.ahr;
	}
	fault("bad special register", (uint32_t)n);
}

static void push(uint32_t v) { C.sp -= 4; wr32(C.sp, v); }
static uint32_t pop(void) { uint32_t v = rd32(C.sp); C.sp += 4; return v; }

static void step(void);

/* branch helper: delayed forms run the next instruction first */
static void branch(int delayed, int take, uint32_t target, int is_call)
{
	uint32_t ret = C.pc + (delayed ? 4 : 2);
	if (delayed) {
		C.next = 0;
		C.pc += 2;
		step();
		/* step advanced pc past the slot */
	} else {
		C.pc += 2;
	}
	if (take) {
		if (is_call)
			push(ret);
		note_jump(ret - (delayed ? 4 : 2), target);
		C.pc = target;
		taken++;
	}
}

static void step(void)
{
	uint32_t op = rd16(C.pc);
	uint32_t pc = C.pc;
	uint32_t *r = C.r;
	int cls = (int)(op >> 13), op1, op2, rd, rs;

	icount++;
	if (profile && pc >= prof_base && pc - prof_base < prof_len) {
		int32_t k = prof_map[(pc - prof_base) >> 1];
		if (k >= 0) prof_count[k]++;
	}
	if (trace)
		fprintf(stderr, "%08x %04x r4=%08x r5=%08x sp=%08x\n", pc, op, r[4], r[5], C.sp);

	if (cls == 6) {		/* ext */
		if (C.next == 2) fault("third ext", pc);
		C.ext[C.next++] = op & 0x1fff;
		C.pc += 2;
		return;
	}
	op1 = (int)((op >> 10) & 7);
	op2 = (int)((op >> 8) & 3);
	rs = (int)((op >> 4) & 15);
	rd = (int)(op & 15);

	switch (cls) {
	case 0: {
		int o = (int)((op >> 9) & 15), d = (int)((op >> 8) & 1);
		if (o >= 4) {
			uint32_t target = pc + jdisp(op & 0xff);
			C.next = 0;
			if (o == 14) branch(d, 1, target, 1);
			else if (o == 15) branch(d, 1, target, 0);
			else branch(d, cond(o), target, 0);
			return;
		}
		{
			int sub = (int)((op >> 6) & 3), grp = (int)((op >> 4) & 3);
			int key = (o << 3) | (d << 2) | sub;
			C.next = 0;
			if (grp == 1 && o == 0 && d == 0 && sub < 2) {
				if (sub == 0) push(r[rd]);
				else r[rd] = pop();
				C.pc += 2;
				return;
			}
			if (grp != 0)
				fault("unknown class 0 instruction", op);
			switch (key) {
			case 0: case 1: case 2:		/* nop, slp, halt */
				C.pc += 2;
				return;
			case 8: {			/* pushn %rN: rN..r0 */
				int i;
				for (i = rd; i >= 0; --i) push(r[i]);
				C.pc += 2;
				return;
			}
			case 9: {			/* popn %rN */
				int i;
				for (i = 0; i <= rd; ++i) r[i] = pop();
				C.pc += 2;
				return;
			}
			case 18:			/* int imm2 */
				if ((op & 3) != 1)
					fault("int other than 1", op);
				grifo_syscall(rd16(pc + 2));
				C.pc = pop();		/* back to the stub's caller */
				return;
			case 24: branch(0, 1, r[rd], 1); return;	/* call %rb */
			case 25: { uint32_t f = C.pc; C.pc = pop(); note_jump(f, C.pc); taken++; return; }	/* ret */
			case 26: branch(0, 1, r[rd], 0); return;	/* jp %rb */
			case 28: branch(1, 1, r[rd], 1); return;	/* call.d %rb */
			case 29: {				/* ret.d */
				uint32_t t = pop(), f = C.pc;
				C.pc += 2;
				step();
				note_jump(f, t);
				C.pc = t;
				return;
			}
			case 30: branch(1, 1, r[rd], 0); return;	/* jp.d %rb */
			}
			fault("unknown class 0 instruction", op);
		}
	}
	case 1:
		if (op2 <= 1) {
			uint32_t a = r[rs] + (op2 == 0 ? disp_rb() : 0);
			static const uint32_t size[8] = { 1, 1, 2, 2, 4, 1, 2, 4 };
			if (op2 == 1 && C.next) fault("ext before post increment", pc);
			if (op1 <= 4) r[rd] = load(op1, a);
			else store(op1, a, r[rd]);
			if (op2 == 1) r[rs] += size[op1];
		} else if (op2 == 2) {
			uint32_t b = C.next ? disp_rb() : r[rs];
			uint32_t a = C.next ? r[rs] : r[rd];
			switch (op1) {
			case 0: r[rd] = do_add(a, b); break;
			case 1: r[rd] = do_sub(a, b); break;
			case 2: if (C.next) fault("ext cmp reg", pc); do_sub(r[rd], r[rs]); break;
			case 3: if (C.next) fault("ext ld.w reg", pc); r[rd] = r[rs]; break;
			case 4: r[rd] = logic(a & b); break;
			case 5: r[rd] = logic(a | b); break;
			case 6: r[rd] = logic(a ^ b); break;
			case 7: if (C.next) fault("ext not reg", pc); r[rd] = logic(~r[rs]); break;
			}
		} else {
			if (C.next) fault("ext before shift", pc);
			if (op1 > 5) fault("unknown class 1 op", op);
			r[rd] = shift(op1, r[rd], (uint32_t)rs + 16);
		}
		break;
	case 2: {
		uint32_t imm6 = (op >> 4) & 0x3f;
		static const uint32_t size[8] = { 1, 1, 2, 2, 4, 1, 2, 4 };
		uint32_t a = C.sp + sp_disp(imm6, size[op1]);
		if (op1 <= 4) r[rd] = load(op1, a);
		else store(op1, a, r[rd]);
		break;
	}
	case 3: {
		uint32_t imm6 = (op >> 4) & 0x3f;
		switch (op1) {
		case 0: r[rd] = do_add(r[rd], imm6x(imm6, 0)); break;
		case 1: r[rd] = do_sub(r[rd], imm6x(imm6, 0)); break;
		case 2: do_sub(r[rd], imm6x(imm6, 1)); break;
		case 3: r[rd] = imm6x(imm6, 1); break;
		case 4: r[rd] = logic(r[rd] & imm6x(imm6, 1)); break;
		case 5: r[rd] = logic(r[rd] | imm6x(imm6, 1)); break;
		case 6: r[rd] = logic(r[rd] ^ imm6x(imm6, 1)); break;
		case 7: r[rd] = logic(~imm6x(imm6, 1)); break;
		}
		break;
	}
	case 4:
		if (C.next) fault("ext before class 4", pc);
		if (op1 <= 1) {
			uint32_t imm10 = (op & 0x3ff) * 4;
			C.sp = op1 ? C.sp - imm10 : C.sp + imm10;
		} else if (op2 <= 1) {
			uint32_t n = op2 == 0 ? (uint32_t)rs : r[rs];
			r[rd] = shift(op1 - 2, r[rd], n);
		} else {
			fault("unknown class 4 instruction", op);
		}
		break;
	case 5:
		if (C.next) fault("ext before class 5", pc);
		if (op2 == 0 && op1 == 0) {
			*special(rd) = r[rs];			/* ld.w %sd,%rs */
		} else if (op2 == 0 && op1 == 1) {
			r[rd] = *special(rs);			/* ld.w %rd,%ss */
		} else if (op2 == 1 && op1 <= 3) {
			static const int k[4] = { 0, 1, 2, 3 };
			uint32_t v = r[rs];
			r[rd] = k[op1] == 0 ? sext(v, 8) : k[op1] == 1 ? (v & 0xff) :
				k[op1] == 2 ? sext(v, 16) : (v & 0xffff);
		} else if (op2 == 2 && op1 <= 3) {
			if (op1 == 0) C.alr = (uint32_t)((int32_t)(int16_t)r[rd] * (int32_t)(int16_t)r[rs]);
			else if (op1 == 1) C.alr = (r[rd] & 0xffff) * (r[rs] & 0xffff);
			else {
				uint64_t m = op1 == 2 ? (uint64_t)((int64_t)(int32_t)r[rd] * (int64_t)(int32_t)r[rs])
						      : (uint64_t)r[rd] * (uint64_t)r[rs];
				C.alr = (uint32_t)m;
				C.ahr = (uint32_t)(m >> 32);
				icount += 4;	/* multi-cycle */
			}
		} else if (op1 == 7 && op2 == 3) {
			uint32_t bit = 1u << (op & 15);
			if (op & 0x40) C.psr |= bit;		/* psrset */
			else C.psr &= ~bit;			/* psrclr */
		} else {
			fault("unknown class 5 instruction", op);
		}
		break;
	default:
		fault("unknown class 7 instruction", op);
	}
	C.next = 0;
	C.pc += 2;
}

static void run_until(uint32_t stop_pc, uint64_t max)
{
	uint64_t start = icount;
	while (!halted && C.pc != stop_pc) {
		step();
		if (icount - start > max)
			fault("instruction limit reached", (uint32_t)(icount - start));
	}
}

int main(int argc, char **argv)
{
	int a = 1;
	uint32_t entry;
	if (argc > 1 && !strcmp(argv[1], "-t")) { trace = 1; a++; }
	if (argc > a && !strcmp(argv[a], "-p")) { profile = 1; a++; }
	if (argc - a < 3) {
		fprintf(stderr, "usage: c33emu [-t] INIT.ELF CARD.img SCRIPT [STATEDIR]\n");
		return 2;
	}
	if (argc - a > 3) state_dir = argv[a + 3];
	mkdir(state_dir, 0777);
	sdram = calloc(1, SDRAM_SIZE);
	low = calloc(1, LOW_SIZE);
	entry = load_elf(argv[a]);
	heap_init();
	if (profile) {
		uint32_t i;
		int k = 0;
		prof_base = 0x10040000;
		prof_len = heap_start - prof_base;
		prof_map = malloc(sizeof(int32_t) * (prof_len / 2));
		prof_count = calloc((size_t)nsyms + 1, sizeof(uint64_t));
		for (i = 0; i < prof_len / 2; ++i) {
			uint32_t a2 = prof_base + i * 2;
			while (k + 1 < nsyms && syms[k + 1].addr <= a2) k++;
			prof_map[i] = (nsyms && syms[k].addr <= a2) ? k : -1;
		}
	}
	card = fopen(argv[a + 1], "rb");
	script = fopen(argv[a + 2], "r");
	if (!card || !script) { fprintf(stderr, "cannot open card or script\n"); return 1; }
	memset(&C, 0, sizeof(C));
	C.sp = MAIN_STACK;
	C.r[15] = DP_USER;
	C.psr = 0x10;
	/* grifo_main(argc, argv) returning to a magic address */
	C.r[6] = 0;
	C.r[7] = 0;
	push(MAGIC_RET);
	C.pc = entry;
	strcpy(cur_line, "boot");
	run_until(MAGIC_RET, 1ull << 40);
	if (profile)
		prof_show();
	watchdog_kick();
	printf("%s: %llu instructions, ~%.1f s of device time, heap peak %u KB, longest without kernel %.1f s\n",
	       failures ? "SOME CHECKS FAILED" : "all checks passed",
	       (unsigned long long)icount, now_cycles() / (MHZ * 1e6), heap_peak >> 10,
	       (double)worst_gap / (MHZ * 1e6));
	return failures ? 1 : 0;
}
