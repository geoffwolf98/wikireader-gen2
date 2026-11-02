/*
 * zimwiki - platform interface.  Implemented by device/plat_grifo.c on
 * the WikiReader and by host/plat_*.c for the simulators.
 */
#ifndef PLAT_H
#define PLAT_H

#include "zw.h"

enum plat_event_type {
	PE_NONE, PE_TOUCH_DOWN, PE_TOUCH_MOVE, PE_TOUCH_UP,
	PE_BUTTON_DOWN, PE_BUTTON_UP, PE_KEY, PE_QUIT, PE_IDLE_TIMEOUT,
};

enum plat_button {
	BTN_SEARCH, BTN_HISTORY, BTN_RANDOM, BTN_POWER,
};

typedef struct plat_event {
	int type;
	int x, y;		/* touch */
	int code;		/* button or key (host keyboard, ASCII / 8 = backspace) */
	uint32_t ms;
} plat_event_t;

/* storage */
int plat_sector_read(uint32_t lba, void *buf, uint32_t count);

/* small files for state: name is 8.3 in the card root */
int plat_load(const char *name, uint8_t **data, uint32_t *len);	/* zw_alloc'd */
int plat_save(const char *name, const void *data, uint32_t len);

/* display: SCREEN_H rows of SCREEN_STRIDE bytes */
void plat_present(const uint8_t *fb);

/* input */
int plat_poll(plat_event_t *e);			/* non-blocking: 0 if none */
int plat_wait(plat_event_t *e);			/* block until something happens */
uint32_t plat_ms(void);

void plat_power_off(void);

/* long work in progress: lets the device's watchdog know (no-op on PC) */
void plat_keepalive(void);

#endif
