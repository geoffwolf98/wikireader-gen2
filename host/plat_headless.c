/*
 * zimwiki-headless CARD.img SCRIPT [STATEDIR]
 *
 * Runs the real application against a FAT card image, driven by a
 * script, with a virtual clock.  Script commands (one per line):
 *
 *   type TEXT              type on the host keyboard path
 *   tap X Y                quick tap
 *   press X Y MS           hold for MS milliseconds
 *   down X Y / up X Y      finger down or up (to screenshot in between)
 *   drag X1 Y1 X2 Y2 MS    finger drag taking MS milliseconds
 *   button search|history|random|power   (a short press)
 *   hold NAME MS           hold a button for MS milliseconds
 *   bounce NAME            a light press with contact bounce
 *   wait MS                let time pass
 *   shot FILE.png          screenshot (LCD colours, 2x)
 *   expect TEXT            fail unless the screen description contains TEXT
 *   describe               print the screen description
 *   restart                quit and start the app again (state persists)
 *   stats                  print storage / decompression counters
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plat_host.h"
#include "../src/app.h"

static int failures;

int plat_poll(plat_event_t *e)
{
	return host_dequeue(e);
}

int plat_wait(plat_event_t *e)
{
	return host_dequeue(e);
}

static void ev(int type, int x, int y, int code)
{
	plat_event_t e;
	memset(&e, 0, sizeof(e));
	e.type = type;
	e.x = x;
	e.y = y;
	e.code = code;
	e.ms = host_clock;
	app_event(&e);
	app_settle();
}

static void advance(uint32_t ms)
{
	uint32_t end = host_clock + ms;
	while (host_clock < end) {
		host_clock += 10;
		app_settle();
	}
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	FILE *s;
	char line[1024], desc[512];
	const char *state = argc > 3 ? argv[3] : "build/state";

	if (argc < 3) {
		fprintf(stderr, "usage: zimwiki-headless CARD.img SCRIPT [STATEDIR]\n");
		return 2;
	}
	if (host_open_card(argv[1], state) < 0) {
		fprintf(stderr, "cannot open card image %s\n", argv[1]);
		return 1;
	}
	s = strcmp(argv[2], "-") ? fopen(argv[2], "r") : stdin;
	if (!s)
		return 1;
	host_virtual_clock = 1;
	host_clock = 1000;
	{
		double t0 = now_ms();
		app_init();
		app_settle();
		printf("[%7.1f ms host] start: ", now_ms() - t0);
		app_describe(desc, sizeof(desc));
		printf("%s\n", desc);
	}
	while (fgets(line, sizeof(line), s)) {
		char cmd[32], arg[900];
		int a, b, c, d, e;
		double t0 = now_ms();
		zw_stats_t before = zw_stats;
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#')
			continue;
		arg[0] = 0;
		sscanf(line, "%31s %899[^\n]", cmd, arg);
		if (!strcmp(cmd, "type")) {
			char *p;
			for (p = arg; *p; ++p)
				ev(PE_KEY, 0, 0, (unsigned char)*p);
		} else if (!strcmp(cmd, "tap") && sscanf(arg, "%d %d", &a, &b) == 2) {
			ev(PE_TOUCH_DOWN, a, b, 0);
			advance(60);
			ev(PE_TOUCH_UP, a, b, 0);
		} else if (!strcmp(cmd, "down") && sscanf(arg, "%d %d", &a, &b) == 2) {
			ev(PE_TOUCH_DOWN, a, b, 0);
		} else if (!strcmp(cmd, "up") && sscanf(arg, "%d %d", &a, &b) == 2) {
			ev(PE_TOUCH_UP, a, b, 0);
		} else if (!strcmp(cmd, "press") && sscanf(arg, "%d %d %d", &a, &b, &c) == 3) {
			ev(PE_TOUCH_DOWN, a, b, 0);
			advance((uint32_t)c);
			ev(PE_TOUCH_UP, a, b, 0);
		} else if (!strcmp(cmd, "drag") && sscanf(arg, "%d %d %d %d %d", &a, &b, &c, &d, &e) == 5) {
			int steps = e / 20 > 1 ? e / 20 : 1, i;
			ev(PE_TOUCH_DOWN, a, b, 0);
			for (i = 1; i <= steps; ++i) {
				host_clock += 20;
				ev(PE_TOUCH_MOVE, a + (c - a) * i / steps, b + (d - b) * i / steps, 0);
			}
			ev(PE_TOUCH_UP, c, d, 0);
		} else if (!strcmp(cmd, "button")) {
			int code = !strcmp(arg, "search") ? BTN_SEARCH : !strcmp(arg, "history") ? BTN_HISTORY :
				   !strcmp(arg, "random") ? BTN_RANDOM : BTN_POWER;
			ev(PE_BUTTON_DOWN, 0, 0, code);
			advance(60);
			ev(PE_BUTTON_UP, 0, 0, code);
			advance(150);
		} else if (!strcmp(cmd, "bounce")) {
			int code = !strcmp(arg, "search") ? BTN_SEARCH : !strcmp(arg, "history") ? BTN_HISTORY :
				   !strcmp(arg, "random") ? BTN_RANDOM : BTN_POWER;
			ev(PE_BUTTON_DOWN, 0, 0, code);
			advance(5);
			ev(PE_BUTTON_UP, 0, 0, code);
			advance(7);
			ev(PE_BUTTON_DOWN, 0, 0, code);
			advance(48);
			ev(PE_BUTTON_UP, 0, 0, code);
			advance(10);
			ev(PE_BUTTON_DOWN, 0, 0, code);
			advance(5);
			ev(PE_BUTTON_UP, 0, 0, code);
			advance(150);
		} else if (!strcmp(cmd, "hold")) {
			char nm[32];
			int ms = 700, code;
			sscanf(arg, "%31s %d", nm, &ms);
			code = !strcmp(nm, "search") ? BTN_SEARCH : !strcmp(nm, "history") ? BTN_HISTORY :
			       !strcmp(nm, "random") ? BTN_RANDOM : BTN_POWER;
			ev(PE_BUTTON_DOWN, 0, 0, code);
			advance((uint32_t)ms);
			ev(PE_BUTTON_UP, 0, 0, code);
			advance(150);
		} else if (!strcmp(cmd, "wait") && sscanf(arg, "%d", &a) == 1) {
			advance((uint32_t)a);
		} else if (!strcmp(cmd, "shot")) {
			host_write_png(arg, 2);
		} else if (!strcmp(cmd, "expect")) {
			app_describe(desc, sizeof(desc));
			if (!strstr(desc, arg)) {
				printf("FAIL expect \"%s\" got: %s\n", arg, desc);
				failures++;
			}
			continue;
		} else if (!strcmp(cmd, "describe")) {
			/* printed below */
		} else if (!strcmp(cmd, "restart")) {
			plat_event_t q;
			memset(&q, 0, sizeof(q));
			q.type = PE_QUIT;
			app_event(&q);
			host_clock += 1000;
			app_init();
			app_settle();
		} else if (!strcmp(cmd, "stats")) {
			printf("    stats: %u read cmds, %u sectors, %u block hits, %u KB decompressed, "
			       "%u clusters started, %u resumed, %u cluster hits\n",
			       zw_stats.sector_reads, zw_stats.sectors_read, zw_stats.cache_hits,
			       zw_stats.bytes_decompressed / 1024, zw_stats.clusters_started,
			       zw_stats.clusters_resumed, zw_stats.cluster_cache_hits);
			continue;
		} else {
			printf("? %s\n", line);
			failures++;
			continue;
		}
		app_describe(desc, sizeof(desc));
		printf("[%7.1f ms host, %4u sectors, %5u KB unz] %-40.40s -> %s\n", now_ms() - t0,
		       zw_stats.sectors_read - before.sectors_read,
		       (zw_stats.bytes_decompressed - before.bytes_decompressed) / 1024, line, desc);
	}
	printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
	host_close_card();
	return failures ? 1 : 0;
}
