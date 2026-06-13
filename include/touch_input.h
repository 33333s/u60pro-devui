/*
 * touch_input.h - Linux evdev touchscreen backend.
 *
 * Auto-probes /dev/input/event* for a multitouch-capable device, reads raw
 * ABS events, and exposes a single-touch (x, y, pressed) state scaled to the
 * display resolution for the GUI layer to poll.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_TOUCH_INPUT_H
#define U60PRO_TOUCH_INPUT_H

typedef struct {
    int fd;
    int screen_w;
    int screen_h;
    int raw_min_x, raw_max_x;
    int raw_min_y, raw_max_y;
    int use_mt;           /* 1 = ABS_MT_POSITION_*, 0 = ABS_X/Y */
    /* latest raw + resolved sample */
    int raw_cur_x, raw_cur_y;
    int cur_x, cur_y;
    int pressed;
} touch_input_t;

/* Probe and open a touch device scaled to screen_w x screen_h. 0 on success. */
int  touch_input_init(touch_input_t *t, int screen_w, int screen_h);

/* Drain pending events and return the latest pointer state.
 * x/y are in display pixels, pressed is 0/1. */
void touch_input_read(touch_input_t *t, int *x, int *y, int *pressed);

/* Total SYN_REPORT samples seen (for measuring touch report rate). */
unsigned long touch_input_report_count(void);

void touch_input_close(touch_input_t *t);

#endif /* U60PRO_TOUCH_INPUT_H */
