/*
 * backlight.c - LCD backlight control via /sys/class/leds.
 *
 * SPDX-License-Identifier: MIT
 */
#include "backlight.h"

#include <stdio.h>
#include <unistd.h>
#include <time.h>

#ifndef BL_DIR
#define BL_DIR "/sys/class/leds/led:lcd"
#endif
#define BL_BRIGHTNESS BL_DIR "/brightness"
#define BL_MAX        BL_DIR "/max_brightness"

static int s_on_level = 255;   /* brightness to restore when turning on */
static int s_is_on    = 1;

static int read_int(const char *path, int def)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return def;
    int v = def;
    if (fscanf(fp, "%d", &v) != 1) v = def;
    fclose(fp);
    return v;
}

static void write_int(const char *path, int v)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "%d", v);
    fclose(fp);
}

void backlight_init(void)
{
    int cur = read_int(BL_BRIGHTNESS, 0);
    int max = read_int(BL_MAX, 255);
    if (cur > 0) { s_on_level = cur; s_is_on = 1; }
    else         { s_on_level = max; s_is_on = 0; }
}

void backlight_on(void)
{
    if (s_on_level <= 0) s_on_level = read_int(BL_MAX, 255);
    write_int(BL_BRIGHTNESS, s_on_level);
    s_is_on = 1;
}

void backlight_off(void)
{
    int cur = read_int(BL_BRIGHTNESS, 0);
    if (cur > 0) s_on_level = cur;   /* remember the level we had */
    write_int(BL_BRIGHTNESS, 0);
    s_is_on = 0;
}

void backlight_toggle(void)
{
    if (s_is_on) backlight_off();
    else         backlight_on();
}

int backlight_is_on(void) { return s_is_on; }

void backlight_set(int level)
{
    int max = read_int(BL_MAX, 255);
    if (level < 0) level = 0;
    if (level > max) level = max;
    write_int(BL_BRIGHTNESS, level);
    if (level > 0) { s_on_level = level; s_is_on = 1; }
    else           { s_is_on = 0; }
}

/* Report the remembered user level (not the live, possibly-mid-fade value) so
 * the brightness slider never jumps around during the off/on fade animation. */
int backlight_get(void) { return s_on_level; }
int backlight_max(void) { return read_int(BL_MAX, 255); }

static long now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Ramp the live brightness from->to over dur_ms, paced by real time (sysfs
 * writes are slow, so we drive value = f(elapsed) and stop at dur_ms — total
 * duration stays correct regardless of write latency). Does not touch s_on_level. */
static void fade_to(int from, int to, int dur_ms)
{
    if (dur_ms < 1) dur_ms = 1;
    long t0 = now_ms(), e;
    while ((e = now_ms() - t0) < dur_ms) {
        write_int(BL_BRIGHTNESS, from + (to - from) * (int)e / dur_ms);
        usleep(4000);
    }
    write_int(BL_BRIGHTNESS, to);
}

void backlight_fade_off(void)
{
    int max = read_int(BL_MAX, 255); if (max <= 0) max = 255;
    int lvl = s_on_level > 0 ? s_on_level : max;
    int dur = 125 * lvl / max; if (dur < 20) dur = 20;   /* ≈0.125s at full, scaled */
    fade_to(lvl, 1, dur);
    write_int(BL_BRIGHTNESS, 0);
    s_is_on = 0;
}

void backlight_predim(void) { write_int(BL_BRIGHTNESS, 1); }

void backlight_fade_on(void)
{
    int max = read_int(BL_MAX, 255); if (max <= 0) max = 255;
    int lvl = s_on_level > 0 ? s_on_level : max;
    int dur = 125 * lvl / max; if (dur < 20) dur = 20;   /* ≈0.125s at full, scaled */
    s_is_on = 1;
    write_int(BL_BRIGHTNESS, 1);   /* soft-start: enable boost at minimum... */
    usleep(20000);                 /* ...let it settle (reduces low-brightness overshoot) */
    fade_to(1, lvl, dur);
}
