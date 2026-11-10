/*
 * zimwiki - host platform pieces shared by the simulators: a FAT card
 * image for storage, a directory for state files, a clock, an event
 * queue and screenshots.
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "plat_host.h"
#include "../src/zw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

uint8_t host_fb[SCREEN_H * SCREEN_STRIDE];
int host_virtual_clock;
uint32_t host_clock;
int host_powered_off;
uint32_t host_presents;

static FILE *card;
static char state_dir[1024];

#define QMAX 4096
static plat_event_t queue[QMAX];
static int q_head, q_tail;

int host_open_card(const char *image, const char *dir)
{
	card = fopen(image, "rb");
	if (!card)
		return -1;
	snprintf(state_dir, sizeof(state_dir), "%s", dir);
	mkdir(state_dir, 0777);
	return 0;
}

void host_close_card(void)
{
	if (card)
		fclose(card);
	card = NULL;
}

int plat_sector_read(uint32_t lba, void *buf, uint32_t count)
{
	if (!card || fseeko(card, (off_t)lba * 512, SEEK_SET) != 0)
		return -1;
	return fread(buf, 512, count, card) == count ? 0 : -1;
}

/* state files live in a directory next to the image, and are read from
 * the card image when not there (e.g. WIKIFONT.BIN) */
int plat_load(const char *name, uint8_t **data, uint32_t *len)
{
	char path[1200];
	FILE *f;
	long n;
	snprintf(path, sizeof(path), "%s/%s", state_dir, name);
	f = fopen(path, "rb");
	if (!f)
		return ZW_ENOTFOUND;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	*data = zw_alloc((size_t)n + 1);
	if (!*data) {
		fclose(f);
		return ZW_ENOMEM;
	}
	{
		/* in pieces with progress, like the device (spinner for the font) */
		long done = 0;
		while (done < n) {
			size_t k = n - done > 16384 ? 16384 : (size_t)(n - done);
			if (fread(*data + done, 1, k, f) != k) {
				fclose(f);
				zw_free(*data);
				return ZW_EIO;
			}
			done += (long)k;
			ZW_PROGRESS(4, (uint32_t)done, (uint32_t)n);
		}
	}
	fclose(f);
	*len = (uint32_t)n;
	return ZW_OK;
}

int plat_save(const char *name, const void *data, uint32_t len)
{
	char path[1200];
	FILE *f;
	snprintf(path, sizeof(path), "%s/%s", state_dir, name);
	{
		/* like the card: the wiki's folder exists, mirror it here */
		char *slash = strrchr(path, '/');
		if (slash && slash > path + strlen(state_dir)) {
			*slash = 0;
			mkdir(path, 0777);
			*slash = '/';
		}
	}
	f = fopen(path, "wb");
	if (!f)
		return ZW_EIO;
	if (fwrite(data, 1, len, f) != len) {
		fclose(f);
		return ZW_EIO;
	}
	fclose(f);
	return ZW_OK;
}

void plat_present(const uint8_t *fb)
{
	memcpy(host_fb, fb, sizeof(host_fb));
	host_presents++;
}

uint32_t plat_ms(void)
{
	struct timespec ts;
	if (host_virtual_clock)
		return host_clock;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void plat_power_off(void)
{
	host_powered_off = 1;
}

void host_queue(const plat_event_t *e)
{
	int n = (q_tail + 1) % QMAX;
	if (n == q_head)
		return;
	queue[q_tail] = *e;
	q_tail = n;
}

int host_dequeue(plat_event_t *e)
{
	if (q_head == q_tail)
		return 0;
	*e = queue[q_head];
	q_head = (q_head + 1) % QMAX;
	return 1;
}

int host_write_pbm(const char *path)
{
	FILE *f = fopen(path, "wb");
	int y;
	if (!f)
		return -1;
	fprintf(f, "P4\n%d %d\n", SCREEN_W, SCREEN_H);
	for (y = 0; y < SCREEN_H; ++y)
		fwrite(host_fb + y * SCREEN_STRIDE, 1, SCREEN_W / 8, f);
	fclose(f);
	return 0;
}

/* PNG that looks like the WikiReader's reflective LCD */
static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8];
	uint32_t crc;
	put32(hdr, len);
	memcpy(hdr + 4, type, 4);
	fwrite(hdr, 1, 8, f);
	if (len)
		fwrite(data, 1, len, f);
	crc = (uint32_t)crc32(0, hdr + 4, 4);
	crc = (uint32_t)crc32(crc, data, len);
	put32(hdr, crc);
	fwrite(hdr, 1, 4, f);
}

int host_write_png(const char *path, int scale)
{
	int w = SCREEN_W * scale, h = SCREEN_H * scale, x, y;
	size_t raw_len = (size_t)(w * 3 + 1) * h;
	uint8_t *raw = malloc(raw_len), *z;
	uLongf zlen = compressBound(raw_len);
	uint8_t ihdr[13];
	FILE *f;
	static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

	for (y = 0; y < h; ++y) {
		uint8_t *row = raw + (size_t)y * (w * 3 + 1);
		row[0] = 0;
		for (x = 0; x < w; ++x) {
			int sx = x / scale, sy = y / scale;
			int on = host_fb[sy * SCREEN_STRIDE + (sx >> 3)] & (0x80 >> (sx & 7));
			uint8_t *px = row + 1 + x * 3;
			if (on) {
				px[0] = 0x20; px[1] = 0x28; px[2] = 0x24;
			} else {
				px[0] = 0xc8; px[1] = 0xd0; px[2] = 0xc0;
			}
		}
	}
	z = malloc(zlen);
	compress2(z, &zlen, raw, raw_len, 9);
	f = fopen(path, "wb");
	if (!f)
		return -1;
	fwrite(sig, 1, 8, f);
	put32(ihdr, (uint32_t)w);
	put32(ihdr + 4, (uint32_t)h);
	ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);
	chunk(f, "IDAT", z, (uint32_t)zlen);
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(raw);
	free(z);
	return 0;
}

void plat_keepalive(void)
{
}
