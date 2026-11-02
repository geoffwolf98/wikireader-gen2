/*
 * zimwiki-sdl CARD.img [STATEDIR] [SCALE]
 *
 * Interactive simulator.  CARD.img is an image of the SD card (e.g.
 * `sudo dd if=/dev/sdX of=card.img`, or one made with mkfs.vfat and
 * mtools).  Mouse = finger.  F1 = SEARCH, F2 = HISTORY (tap = back,
 * hold = list), F3 = RANDOM,
 * typing goes to the search box, Backspace = back in an article,
 * Esc = quit.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat_host.h"
#include "../src/app.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int scale = 3;
static int mouse_down;

void plat_present_hook(void);

static void show(void)
{
	static uint32_t px[SCREEN_W * SCREEN_H];
	int x, y;
	for (y = 0; y < SCREEN_H; ++y)
		for (x = 0; x < SCREEN_W; ++x)
			px[y * SCREEN_W + x] = (host_fb[y * SCREEN_STRIDE + (x >> 3)] & (0x80 >> (x & 7)))
					       ? 0xff202824u : 0xffc8d0c0u;
	SDL_UpdateTexture(tex, NULL, px, SCREEN_W * 4);
	SDL_RenderClear(ren);
	SDL_RenderCopy(ren, tex, NULL, NULL);
	SDL_RenderPresent(ren);
}

static int translate(const SDL_Event *s, plat_event_t *e)
{
	memset(e, 0, sizeof(*e));
	e->ms = plat_ms();
	switch (s->type) {
	case SDL_QUIT:
		e->type = PE_QUIT;
		return 1;
	case SDL_MOUSEBUTTONDOWN:
		if (s->button.button != SDL_BUTTON_LEFT)
			return 0;
		mouse_down = 1;
		e->type = PE_TOUCH_DOWN;
		e->x = s->button.x / scale;
		e->y = s->button.y / scale;
		return 1;
	case SDL_MOUSEMOTION:
		if (!mouse_down)
			return 0;
		e->type = PE_TOUCH_MOVE;
		e->x = s->motion.x / scale;
		e->y = s->motion.y / scale;
		return 1;
	case SDL_MOUSEBUTTONUP:
		if (s->button.button != SDL_BUTTON_LEFT)
			return 0;
		mouse_down = 0;
		e->type = PE_TOUCH_UP;
		e->x = s->button.x / scale;
		e->y = s->button.y / scale;
		return 1;
	case SDL_MOUSEWHEEL:
		return 0;
	case SDL_TEXTINPUT:
		if ((unsigned char)s->text.text[0] < 128) {
			e->type = PE_KEY;
			e->code = (unsigned char)s->text.text[0];
			return 1;
		}
		return 0;
	case SDL_KEYUP:
		if (s->key.repeat)
			return 0;
		switch (s->key.keysym.sym) {
		case SDLK_F1: e->type = PE_BUTTON_UP; e->code = BTN_SEARCH; return 1;
		case SDLK_F2: e->type = PE_BUTTON_UP; e->code = BTN_HISTORY; return 1;
		case SDLK_F3: e->type = PE_BUTTON_UP; e->code = BTN_RANDOM; return 1;
		}
		return 0;
	case SDL_KEYDOWN:
		if (s->key.repeat && s->key.keysym.sym >= SDLK_F1 && s->key.keysym.sym <= SDLK_F3)
			return 0;	/* a held button is one press */
		switch (s->key.keysym.sym) {
		case SDLK_F1: e->type = PE_BUTTON_DOWN; e->code = BTN_SEARCH; return 1;
		case SDLK_F2: e->type = PE_BUTTON_DOWN; e->code = BTN_HISTORY; return 1;
		case SDLK_F3: e->type = PE_BUTTON_DOWN; e->code = BTN_RANDOM; return 1;
		case SDLK_BACKSPACE: e->type = PE_KEY; e->code = 8; return 1;
		case SDLK_RETURN: e->type = PE_KEY; e->code = '\n'; return 1;
		case SDLK_ESCAPE: e->type = PE_QUIT; return 1;
		}
		return 0;
	case SDL_WINDOWEVENT:
		if (s->window.event == SDL_WINDOWEVENT_EXPOSED)
			show();
		return 0;
	}
	return 0;
}

int plat_poll(plat_event_t *e)
{
	SDL_Event s;
	static uint32_t shown;
	if (shown != host_presents) {
		shown = host_presents;
		show();
	}
	while (SDL_PollEvent(&s))
		if (translate(&s, e))
			return 1;
	return 0;
}

int plat_wait(plat_event_t *e)
{
	SDL_Event s;
	if (plat_poll(e))
		return 1;
	if (SDL_WaitEventTimeout(&s, 50) && translate(&s, e))
		return 1;
	if (host_powered_off) {
		e->type = PE_QUIT;
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *state = argc > 2 ? argv[2] : "zimwiki-state";
	if (argc < 2) {
		fprintf(stderr, "usage: zimwiki-sdl CARD.img [STATEDIR] [SCALE]\n");
		return 2;
	}
	if (argc > 3)
		scale = atoi(argv[3]) > 0 ? atoi(argv[3]) : 3;
	if (host_open_card(argv[1], state) < 0) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "SDL: %s\n", SDL_GetError());
		return 1;
	}
	win = SDL_CreateWindow("WikiReader (zimwiki)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			       SCREEN_W * scale, SCREEN_H * scale, 0);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
	tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
				SCREEN_W, SCREEN_H);
	SDL_StartTextInput();
	fprintf(stderr, "mouse = finger, F1 search, F2 back (hold: history), F3 random, Esc quit\n");
	app_main();
	SDL_Quit();
	host_close_card();
	return 0;
}
