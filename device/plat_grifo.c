/*
 * zimwiki - WikiReader platform layer on the grifo kernel
 *
 * Built as init.app: the boot ROM loads kernel.elf (grifo) from the
 * card and grifo chains straight to init.app.
 */
#include <grifo.h>
#include <stdarg.h>
#include "../src/plat.h"
#include "../src/gfx.h"
#include "../src/app.h"

zw_stats_t zw_stats;

/* ------------------------------------------------------------------ */
/* memory: grifo's page heap */

void *zw_alloc(size_t size)
{
	return memory_allocate(size ? size : 1, "zw");
}

void *zw_calloc(size_t size)
{
	void *p = zw_alloc(size);
	if (p)
		memset(p, 0, size);
	return p;
}

void zw_free(void *p)
{
	if (p)
		memory_free(p, "zw");
}

void *zw_realloc(void *p, size_t old_size, size_t new_size)
{
	void *n = zw_alloc(new_size);
	if (!n)
		return NULL;
	if (p) {
		memcpy(n, p, old_size < new_size ? old_size : new_size);
		zw_free(p);
	}
	return n;
}

/* for library code that calls the C allocator directly */
void *malloc(size_t size) { return zw_alloc(size); }
void *calloc(size_t n, size_t size) { return zw_calloc(n * size); }
void free(void *p) { zw_free(p); }

/* faster than mini-libc's for the small and word aligned copies that
 * dominate here (screen rows, text, buffers) */
void *memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (n >= 8 && (((uint32_t)d ^ (uint32_t)s) & 3) == 0) {
		uint32_t *dw;
		const uint32_t *sw;
		while ((uint32_t)d & 3) {
			*d++ = *s++;
			n--;
		}
		dw = (uint32_t *)d;
		sw = (const uint32_t *)s;
		while (n >= 16) {
			uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
			dw += 4;
			sw += 4;
			n -= 16;
		}
		while (n >= 4) {
			*dw++ = *sw++;
			n -= 4;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n >= 4) {
		d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
		d += 4;
		s += 4;
		n -= 4;
	}
	while (n--)
		*d++ = *s++;
	return dst;
}

/* debug serial port log.  The format string is printed as it is: the
 * mini libc's printf family is not dependable (snprintf crashed). */
/* debug serial port log.  The format string is printed as it is: the
 * mini libc's printf family is not dependable (snprintf crashed). */
void zw_log(const char *fmt, ...)
{
	debug_print(fmt);
	debug_print("\n");
}

/* ------------------------------------------------------------------ */
/* storage */

int plat_sector_read(uint32_t lba, void *buf, uint32_t count)
{
	while (count) {
		uint32_t n = count > 128 ? 128 : count;
		if (sector_read(lba, buf, (int)n) != FILE_ERROR_OK)
			return ZW_EIO;
		lba += n;
		buf = (uint8_t *)buf + n * 512;
		count -= n;
	}
	return ZW_OK;
}

int plat_load(const char *name, uint8_t **data, uint32_t *len)
{
	unsigned long size = 0;
	int h;
	ssize_t n;
	if (file_size(name, &size) != FILE_ERROR_OK)
		return ZW_ENOTFOUND;
	h = file_open(name, FILE_OPEN_READ);
	if (h < 0)
		return ZW_ENOTFOUND;
	*data = zw_alloc(size + 1);
	if (!*data) {
		file_close(h);
		return ZW_ENOMEM;
	}
	/* in pieces, feeding the watchdog: the fallback font is ~2 MB */
	{
		unsigned long done = 0;
		n = 0;
		while (done < size) {
			unsigned long k = size - done > 65536 ? 65536 : size - done;
			ssize_t r = file_read(h, *data + done, k);
			if (r <= 0)
				break;
			done += (unsigned long)r;
			plat_keepalive();
		}
		n = (ssize_t)done;
	}
	file_close(h);
	if (n != (ssize_t)size) {
		zw_free(*data);
		return ZW_EIO;
	}
	*len = size;
	return ZW_OK;
}

/* Save by overwriting in place, never by allocating.  grifo's file_create
 * is FatFs FA_CREATE_NEW (fails if the file exists), and allocating space
 * makes FatFs search the FAT for a free cluster from the start, which on
 * a nearly full card takes long enough to freeze the unit or trip the
 * watchdog.  So:
 *   - an existing file that is big enough is overwritten from the start
 *     with just the new data: its size stays as it is, and whatever old
 *     bytes are left after the data are ignored (the loaders read their
 *     own counts), so there is no need to write padding every time;
 *   - an existing file that is too small (a save that was cut off leaves
 *     an empty one) is left alone: growing it would allocate;
 *   - a missing file is created once, padded to SAVE_MIN so that later
 *     saves fit.  zimprep makes NAME.HIS and NAME.EXT in advance and the
 *     release has ZWSTATE.DAT, so on a prepared card this never happens. */
#define SAVE_MIN (64u * 1024u)

int plat_save(const char *name, const void *data, uint32_t len)
{
	static uint8_t zeros[512];
	unsigned long old = 0;
	uint32_t pad;
	ssize_t n;
	int h = -1;

	if (file_size(name, &old) == 0) {
		if (old < len) {
			zw_log("not saving %s: %lu bytes on the card, %u needed", name, old,
			       (unsigned)len);
			return ZW_ENOMEM;
		}
		h = file_open(name, FILE_OPEN_WRITE);
		if (h < 0)
			return ZW_EIO;
		pad = 0;
	} else {
		h = file_create(name, FILE_OPEN_WRITE);
		if (h < 0)
			return ZW_EIO;
		pad = len < SAVE_MIN ? SAVE_MIN - len : 0;
	}
	n = file_write(h, (void *)data, len);
	while (pad && n == (ssize_t)len) {
		uint32_t k = pad < sizeof(zeros) ? pad : sizeof(zeros);
		if (file_write(h, zeros, k) != (ssize_t)k)
			break;
		pad -= k;
	}
	file_sync(h);
	file_close(h);
	/* the original WikiReader software waits after saving: writes are
	 * not reliable without it */
	delay_us(200000);
	return n == (ssize_t)len ? ZW_OK : ZW_EIO;
}

/* ------------------------------------------------------------------ */
/* display */

void plat_present(const uint8_t *fb)
{
	memcpy(lcd_get_framebuffer(), fb, SCREEN_H * SCREEN_STRIDE);
}

/* ------------------------------------------------------------------ */
/* time: 60 timer counts per microsecond, wraps every ~71 s */

uint32_t plat_ms(void)
{
	static unsigned long last;
	static uint32_t frac, ms;
	unsigned long now = timer_get();
	frac += (uint32_t)(now - last);
	last = now;
	if (frac >= 60000u * 16) {
		ms += frac / 60000u;	/* after a long gap */
		frac %= 60000u;
	} else {
		while (frac >= 60000u) {	/* usual case: no division */
			frac -= 60000u;
			ms++;
		}
	}
	return ms;
}

/* ------------------------------------------------------------------ */
/* input */

static int translate(const event_t *g, plat_event_t *e)
{
	memset(e, 0, sizeof(*e));
	e->ms = plat_ms();
	switch (g->item_type) {
	case EVENT_TOUCH_DOWN:
		e->type = PE_TOUCH_DOWN;
		break;
	case EVENT_TOUCH_MOTION:
		e->type = PE_TOUCH_MOVE;
		break;
	case EVENT_TOUCH_UP:
		e->type = PE_TOUCH_UP;
		break;
	case EVENT_BUTTON_DOWN:
	case EVENT_BUTTON_UP:
		e->type = g->item_type == EVENT_BUTTON_DOWN ? PE_BUTTON_DOWN : PE_BUTTON_UP;
		switch (g->button.code) {
		case BUTTON_SEARCH: e->code = BTN_SEARCH; break;
		case BUTTON_HISTORY: e->code = BTN_HISTORY; break;
		case BUTTON_RANDOM: e->code = BTN_RANDOM; break;
		default: e->code = BTN_POWER; break;
		}
		return 1;
	case EVENT_KEY:
		/* serial console keys: handy for testing over the debug port */
		e->type = PE_KEY;
		e->code = g->key.code == 0x7f ? 8 : g->key.code;
		return 1;
	default:
		return 0;
	}
	e->x = g->touch.x;
	e->y = g->touch.y;
	if (e->x < 0) e->x = 0;
	if (e->x >= SCREEN_W) e->x = SCREEN_W - 1;
	if (e->y < 0) e->y = 0;
	if (e->y >= SCREEN_H) e->y = SCREEN_H - 1;
	return 1;
}

int plat_poll(plat_event_t *e)
{
	event_t g;
	while (event_get(&g) != EVENT_NONE)
		if (translate(&g, e))
			return 1;
	return 0;
}

/* called by grifo when the CPU has been suspended for its auto power off
 * time (SUSPEND_AUTO_POWER_OFF_SECONDS) with nothing happening: save,
 * then let it power off */
static bool idle_callback(void *arg)
{
	plat_event_t e;
	(void)arg;
	memset(&e, 0, sizeof(e));
	e.type = PE_IDLE_TIMEOUT;
	app_event(&e);
	return false;
}

int plat_wait(plat_event_t *e)
{
	event_t g;
	if (plat_poll(e))
		return 1;
	event_wait(&g, idle_callback, NULL);
	return translate(&g, e);
}

void plat_power_off(void)
{
	file_close_all();
	power_off();
}

/* ------------------------------------------------------------------ */

void (*zw_progress)(int stage, uint32_t done, uint32_t total);

void plat_keepalive(void)
{
	watchdog(WATCHDOG_KEY);
}

int grifo_main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	lcd_set_default_framebuffer();
	return app_main();
}
