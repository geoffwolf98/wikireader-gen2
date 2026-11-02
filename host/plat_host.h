#ifndef PLAT_HOST_H
#define PLAT_HOST_H

#include "../src/plat.h"
#include "../src/gfx.h"

/* shared by the host simulators */
int host_open_card(const char *image, const char *state_dir);
void host_close_card(void);
extern uint8_t host_fb[SCREEN_H * SCREEN_STRIDE];
extern int host_virtual_clock;	/* 1: plat_ms() returns host_clock */
extern uint32_t host_clock;
extern int host_powered_off;
extern uint32_t host_presents;

/* event queue used by both front ends */
void host_queue(const plat_event_t *e);
int host_dequeue(plat_event_t *e);

int host_write_pbm(const char *path);
int host_write_png(const char *path, int scale);

#endif
