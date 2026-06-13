/*
 * main.c - u60pro-devui entry point.
 *
 * Wires the clean-room DRM display and evdev touch backends into LVGL, then
 * runs the GUI event loop. No vendor libraries are linked.
 *
 * SPDX-License-Identifier: MIT
 */
#include "drm_disp.h"
#include "touch_input.h"
#include "devui_config.h"
#include "ui.h"
#include "lvgl.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static drm_disp_t    g_disp;
static touch_input_t g_touch;
unsigned long        g_frame_count = 0;   /* flushes pushed to the panel */
static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig) { (void)sig; g_run = 0; }

/* Monotonic millisecond tick source for LVGL. */
static uint32_t millis_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

/* Copy an LVGL-rendered area into the DRM framebuffer (RGB565), applying the
 * panel's 180° mounting rotation, then mark the region dirty. */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t *src = (const uint16_t *)px_map;
    const int aw       = area->x2 - area->x1 + 1;
    const int W        = g_disp.width;
    const int H        = g_disp.height;
    const int pitch_px = g_disp.pitch_px;

    int dx1, dy1, dx2, dy2;

#if DEVUI_ROTATE_180
    /* 180°: dest row decreases, and within a row dest x decreases — walk both
     * with pointer arithmetic (no per-pixel multiplies). */
    for (int y = area->y1; y <= area->y2; y++) {
        const uint16_t *sp = src + (size_t)(y - area->y1) * aw;
        uint16_t *dp = g_disp.fb + (size_t)((H - 1) - y) * pitch_px + ((W - 1) - area->x1);
        for (int i = 0; i < aw; i++) *dp-- = *sp++;
    }
    dx1 = (W - 1) - area->x2; dx2 = (W - 1) - area->x1;
    dy1 = (H - 1) - area->y2; dy2 = (H - 1) - area->y1;
#else
    /* No rotation: straight row memcpy. */
    for (int y = area->y1; y <= area->y2; y++) {
        const uint16_t *sp = src + (size_t)(y - area->y1) * aw;
        uint16_t *dp = g_disp.fb + (size_t)y * pitch_px + area->x1;
        memcpy(dp, sp, (size_t)aw * sizeof(uint16_t));
    }
    dx1 = area->x1; dx2 = area->x2;
    dy1 = area->y1; dy2 = area->y2;
#endif
    drm_disp_dirty(&g_disp, dx1, dy1, dx2, dy2);
    g_frame_count++;

    lv_display_flush_ready(disp);
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    int x, y, pressed;
    touch_input_read(&g_touch, &x, &y, &pressed);
    data->point.x = x;
    data->point.y = y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

int main(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (drm_disp_init(&g_disp) != 0) {
        fprintf(stderr, "fatal: display init failed\n");
        return 1;
    }
    /* Touch is optional: the UI still renders without it. */
    if (touch_input_init(&g_touch, g_disp.width, g_disp.height) != 0)
        fprintf(stderr, "warning: continuing without touch input\n");

    lv_init();
    lv_tick_set_cb(millis_cb);

    lv_display_t *disp = lv_display_create(g_disp.width, g_disp.height);
    lv_display_set_flush_cb(disp, disp_flush_cb);

    /* Full-screen double buffers: a full redraw (e.g. a page swipe) becomes a
     * single flush + one DIRTYFB instead of many small ones. */
    static uint16_t buf1[DEVUI_FALLBACK_WIDTH * DEVUI_FALLBACK_HEIGHT];
    static uint16_t buf2[DEVUI_FALLBACK_WIDTH * DEVUI_FALLBACK_HEIGHT];
    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);

    ui_create();

    while (g_run) {
        uint32_t idle = lv_timer_handler();
        if (idle > 8) idle = 8;         /* render promptly during animations */
        usleep((useconds_t)idle * 1000);
    }

    drm_disp_close(&g_disp);
    touch_input_close(&g_touch);
    return 0;
}
