#ifndef APP_H
#define APP_H

/* run the application; returns when the platform asks to quit */
int app_main(void);

/* for tests: one line describing what is on screen */
void app_describe(char *out, int cap);

/* for tests: handle one event and redraw if needed, then run idle work
 * until there is nothing left to do (no blocking) */
void app_init(void);
void app_event(const void *plat_event);
void app_settle(void);

#endif
