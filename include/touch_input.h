/*
 * touch_input.h - Linux evdev touchscreen backend.
 *
 * Auto-probes /dev/input/event* for a multitouch-capable device, reads raw
 * ABS events, and exposes a single-touch (x, y, pressed) state scaled to the
 * display resolution for the GUI layer to poll.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ZWRT_DEVUI_TOUCH_INPUT_H
#define ZWRT_DEVUI_TOUCH_INPUT_H

#define TOUCH_INPUT_QUEUE_LEN 16

typedef struct {
    int fd;
    int screen_w;
    int screen_h;
    int raw_min_x, raw_max_x;
    int raw_min_y, raw_max_y;
    int use_mt;           /* 1 = ABS_MT_POSITION_*, 0 = ABS_X/Y */
    int rot180;           /* rotate samples 180 degrees (DEVUI_TOUCH_ROTATE_180 by default) */
    /* latest raw + resolved sample */
    int raw_cur_x, raw_cur_y;
    int cur_x, cur_y;
    int pressed;
    /* tap-edge latch: a full press->release that completes between two polls
     * (e.g. while a slow render blocks the loop) would otherwise be lost, since
     * read() collapses to the final state. We record completed taps in a small
     * queue so callers never miss a fast tap. */
    int press_x, press_y, in_tap;
    int tapq_x[TOUCH_INPUT_QUEUE_LEN], tapq_y[TOUCH_INPUT_QUEUE_LEN];
    int tapq_head, tapq_tail;
    int strokeq_x0[TOUCH_INPUT_QUEUE_LEN], strokeq_y0[TOUCH_INPUT_QUEUE_LEN];
    int strokeq_x1[TOUCH_INPUT_QUEUE_LEN], strokeq_y1[TOUCH_INPUT_QUEUE_LEN];
    int strokeq_head, strokeq_tail;
} touch_input_t;

/* Probe and open a touch device scaled to screen_w x screen_h. 0 on success. */
int  touch_input_init(touch_input_t *t, int screen_w, int screen_h);

/* Drain pending events and return the latest pointer state.
 * x/y are in display pixels, pressed is 0/1. */
void touch_input_read(touch_input_t *t, int *x, int *y, int *pressed);

/* Pop the oldest completed tap (press->release, small movement). Returns 1 and
 * fills x/y (press position) if one was queued, else 0. Drain in a loop to
 * apply every tap that landed during a blocking render. */
int  touch_input_take_tap(touch_input_t *t, int *x, int *y);

/* Drain queued taps and return only the newest one. Use this for normal UI
 * actions so fast repeated input does not replay slowly after a render stall. */
int  touch_input_take_latest_tap(touch_input_t *t, int *x, int *y);

/* Pop the oldest completed stroke (press->release), returning start/end points.
 * This lets callers recognize gestures even if the full drag lands between two
 * polling iterations. */
int  touch_input_take_stroke(touch_input_t *t, int *x0, int *y0, int *x1, int *y1);

/* Drain queued strokes and return only the newest one. */
int  touch_input_take_latest_stroke(touch_input_t *t, int *x0, int *y0, int *x1, int *y1);

/* Drop the release event that the caller has already handled through the live
 * pressed->released edge. Keeps later queued taps/strokes intact. */
void touch_input_drop_replayed_release(touch_input_t *t, int drop_tap);

/* Drop any queued taps/strokes (call when switching context, e.g. entering a pad). */
void touch_input_clear_taps(touch_input_t *t);

/* Total SYN_REPORT samples seen (for measuring touch report rate). */
unsigned long touch_input_report_count(void);

void touch_input_close(touch_input_t *t);

#endif /* ZWRT_DEVUI_TOUCH_INPUT_H */
