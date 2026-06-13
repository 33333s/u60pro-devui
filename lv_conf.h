/*
 * lv_conf.h - LVGL v9 configuration for u60pro-devui.
 *
 * Only options that differ from LVGL's built-in defaults are set here; every
 * other option falls back to lv_conf_internal.h. Targets a 320x480 RGB565
 * command-mode panel with no RTOS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* RGB565 matches the panel's dumb buffer exactly. */
#define LV_COLOR_DEPTH 16

/* Bare-metal main loop, no OS. */
#define LV_USE_OS LV_OS_NONE

/* Self-contained heap so we don't depend on libc malloc for LVGL objects. */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (128 * 1024U)

/* Logging to stderr during development; flip to 0 for production. */
#define LV_USE_LOG   1
#define LV_LOG_PRINTF 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN

/* Software renderer (no GPU). */
#define LV_USE_DRAW_SW 1

/* Refresh/input cadence (ms) — snappier animations on this slow panel. */
#define LV_DEF_REFR_PERIOD 20

/* Fonts we use in the demo UI. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

/* QR code widget for the WiFi share page. */
#define LV_USE_QRCODE 1

/* FreeType for CJK (Chinese) text — loads a TTF from the device at runtime. */
#define LV_USE_FREETYPE 1
#define LV_FREETYPE_USE_LVGL_PORT 0
#define LV_FREETYPE_CACHE_FT_GLYPH_CNT 256

/* No bundled demos/examples in the binary. */
#define LV_USE_DEMO_WIDGETS 0
#define LV_BUILD_EXAMPLES   0

#endif /* LV_CONF_H */
