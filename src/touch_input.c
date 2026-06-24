/*
 * touch_input.c - Linux evdev touchscreen backend (clean-room).
 *
 * Built only on the standard Linux input UAPI. No vendor gesture library.
 *
 * SPDX-License-Identifier: MIT
 */
#include "touch_input.h"
#include "devui_config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* --- minimal Linux input UAPI (interface facts, not vendor-derived) --- */
struct input_event_compat {
    unsigned long sec;      /* arm64: 8 bytes */
    unsigned long usec;     /* arm64: 8 bytes */
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

struct input_absinfo_compat {
    int32_t value;
    int32_t minimum;
    int32_t maximum;
    int32_t fuzz;
    int32_t flat;
    int32_t resolution;
};

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_ABS 0x03

#define SYN_REPORT 0x00
#define BTN_TOUCH  0x14a

#define ABS_X                0x00
#define ABS_Y                0x01
#define ABS_MT_SLOT          0x2f
#define ABS_MT_POSITION_X    0x35
#define ABS_MT_POSITION_Y    0x36
#define ABS_MT_TRACKING_ID   0x39

#define EVIOCGBIT_(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGABS_(abs)     _IOC(_IOC_READ, 'E', 0x40 + (abs), sizeof(struct input_absinfo_compat))

static unsigned long s_report_count = 0;   /* total SYN_REPORT samples */

static int test_bit(const unsigned long *arr, int bit)
{
    return (arr[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

/* Does this fd look like a touchscreen (EV_ABS with X/Y or MT X/Y)? */
static int probe_is_touch(int fd, int *use_mt)
{
    unsigned long abs_bits[(ABS_MT_TRACKING_ID / (8 * sizeof(long))) + 1];
    memset(abs_bits, 0, sizeof(abs_bits));
    if (ioctl(fd, EVIOCGBIT_(EV_ABS, sizeof(abs_bits)), abs_bits) < 0)
        return 0;

    if (test_bit(abs_bits, ABS_MT_POSITION_X) && test_bit(abs_bits, ABS_MT_POSITION_Y)) {
        *use_mt = 1;
        return 1;
    }
    if (test_bit(abs_bits, ABS_X) && test_bit(abs_bits, ABS_Y)) {
        *use_mt = 0;
        return 1;
    }
    return 0;
}

static void load_axis_range(int fd, int axis, int *lo, int *hi)
{
    struct input_absinfo_compat a;
    memset(&a, 0, sizeof(a));
    if (ioctl(fd, EVIOCGABS_(axis), &a) == 0 && a.maximum > a.minimum) {
        *lo = a.minimum;
        *hi = a.maximum;
    }
}

int touch_input_init(touch_input_t *t, int screen_w, int screen_h)
{
    memset(t, 0, sizeof(*t));
    t->fd = -1;
    t->screen_w = screen_w;
    t->screen_h = screen_h;
    t->raw_min_x = 0; t->raw_max_x = screen_w - 1;
    t->raw_min_y = 0; t->raw_max_y = screen_h - 1;

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        fprintf(stderr, "touch: opendir /dev/input failed: %s\n", strerror(errno));
        return -1;
    }

    struct dirent *de;
    char path[288];
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0)
            continue;
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        int use_mt = 0;
        if (probe_is_touch(fd, &use_mt)) {
            t->fd = fd;
            t->use_mt = use_mt;
            load_axis_range(fd, use_mt ? ABS_MT_POSITION_X : ABS_X,
                            &t->raw_min_x, &t->raw_max_x);
            load_axis_range(fd, use_mt ? ABS_MT_POSITION_Y : ABS_Y,
                            &t->raw_min_y, &t->raw_max_y);
            fprintf(stderr, "touch: using %s (%s, x[%d..%d] y[%d..%d])\n",
                    path, use_mt ? "MT" : "ST",
                    t->raw_min_x, t->raw_max_x, t->raw_min_y, t->raw_max_y);
            break;
        }
        close(fd);
    }
    closedir(dir);

    if (t->fd < 0) {
        fprintf(stderr, "touch: no touchscreen found under /dev/input\n");
        return -1;
    }
    return 0;
}

static int scale(int v, int lo, int hi, int out_max)
{
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    long s = (long)(v - lo) * out_max / (hi - lo);
    return (int)s;
}

static void apply_transform(touch_input_t *t, int *x, int *y)
{
#if DEVUI_TOUCH_SWAP_XY
    int tmp = *x; *x = *y; *y = tmp;
#endif
#if DEVUI_TOUCH_INVERT_X
    *x = (t->screen_w - 1) - *x;
#endif
#if DEVUI_TOUCH_INVERT_Y
    *y = (t->screen_h - 1) - *y;
#endif
#if DEVUI_TOUCH_ROTATE_180
    *x = (t->screen_w - 1) - *x;
    *y = (t->screen_h - 1) - *y;
#endif
    (void)t;
}

void touch_input_read(touch_input_t *t, int *x, int *y, int *pressed)
{
    if (t->fd < 0) {
        *x = t->cur_x; *y = t->cur_y; *pressed = 0;
        return;
    }

    struct input_event_compat ev;
    int new_press = t->pressed;

    /* Drain everything currently available (non-blocking). Accumulate the
     * latest raw axis values, then resolve once per SYN_REPORT so the X/Y
     * transform is always applied to a coherent pair. */
    while (read(t->fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            switch (ev.code) {
            case ABS_MT_POSITION_X: if (t->use_mt)  t->raw_cur_x = ev.value; break;
            case ABS_MT_POSITION_Y: if (t->use_mt)  t->raw_cur_y = ev.value; break;
            case ABS_X:  if (!t->use_mt) t->raw_cur_x = ev.value; break;
            case ABS_Y:  if (!t->use_mt) t->raw_cur_y = ev.value; break;
            case ABS_MT_TRACKING_ID: new_press = (ev.value >= 0); break;
            default: break;
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            new_press = (ev.value != 0);
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            int sx = scale(t->raw_cur_x, t->raw_min_x, t->raw_max_x, t->screen_w - 1);
            int sy = scale(t->raw_cur_y, t->raw_min_y, t->raw_max_y, t->screen_h - 1);
            apply_transform(t, &sx, &sy);
            int was = t->pressed;
            t->cur_x = sx;
            t->cur_y = sy;
            t->pressed = new_press;
            if (!was && new_press) {              /* press down: remember origin */
                t->press_x = sx; t->press_y = sy; t->in_tap = 1;
            } else if (was && !new_press && t->in_tap) {   /* release: latch a tap if it barely moved */
                int dx = sx - t->press_x, dy = sy - t->press_y;
                int ns = (t->strokeq_tail + 1) % 8;
                if (ns != t->strokeq_head) {
                    t->strokeq_x0[t->strokeq_tail] = t->press_x;
                    t->strokeq_y0[t->strokeq_tail] = t->press_y;
                    t->strokeq_x1[t->strokeq_tail] = sx;
                    t->strokeq_y1[t->strokeq_tail] = sy;
                    t->strokeq_tail = ns;
                }
                if (dx * dx + dy * dy <= 14 * 14) {
                    int nx = (t->tapq_tail + 1) % 8;
                    if (nx != t->tapq_head) {      /* enqueue (drop if full) */
                        t->tapq_x[t->tapq_tail] = t->press_x;
                        t->tapq_y[t->tapq_tail] = t->press_y;
                        t->tapq_tail = nx;
                    }
                }
                t->in_tap = 0;
            }
            s_report_count++;
        }
    }

    *x = t->cur_x;
    *y = t->cur_y;
    *pressed = t->pressed;
}

int touch_input_take_tap(touch_input_t *t, int *x, int *y)
{
    if (t->tapq_head == t->tapq_tail) return 0;   /* empty */
    *x = t->tapq_x[t->tapq_head];
    *y = t->tapq_y[t->tapq_head];
    t->tapq_head = (t->tapq_head + 1) % 8;
    return 1;
}

int touch_input_take_stroke(touch_input_t *t, int *x0, int *y0, int *x1, int *y1)
{
    if (t->strokeq_head == t->strokeq_tail) return 0;
    *x0 = t->strokeq_x0[t->strokeq_head];
    *y0 = t->strokeq_y0[t->strokeq_head];
    *x1 = t->strokeq_x1[t->strokeq_head];
    *y1 = t->strokeq_y1[t->strokeq_head];
    t->strokeq_head = (t->strokeq_head + 1) % 8;
    return 1;
}

void touch_input_clear_taps(touch_input_t *t)
{
    t->tapq_head = t->tapq_tail = 0;
    t->strokeq_head = t->strokeq_tail = 0;
    t->in_tap = 0;
}

unsigned long touch_input_report_count(void) { return s_report_count; }

void touch_input_close(touch_input_t *t)
{
    if (t->fd >= 0)
        close(t->fd);
    t->fd = -1;
}
