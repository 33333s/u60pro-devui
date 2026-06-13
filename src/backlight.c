/*
 * backlight.c - LCD backlight control via /sys/class/leds.
 *
 * SPDX-License-Identifier: MIT
 */
#include "backlight.h"

#include <stdio.h>

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

int backlight_get(void) { return read_int(BL_BRIGHTNESS, 0); }
int backlight_max(void) { return read_int(BL_MAX, 255); }
