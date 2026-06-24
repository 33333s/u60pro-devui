/*
 * devui_ext.h - optional external display channel for u60pro-devui.
 *
 * The normal /data/ui page flow remains the owner of the screen until a local
 * client submits an external frame. External content is held in a logical
 * RGB565 canvas and can be closed explicitly or by TTL.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_DEVUI_EXT_H
#define U60PRO_DEVUI_EXT_H

#include "drm_disp.h"

#include <stdint.h>

#define DEVUI_EXT_STATUSBAR_H 26
#define DEVUI_EXT_SOCK_PATH  "/tmp/u60-devui.sock"
#define DEVUI_EXT_EVENT_PATH "/tmp/u60-devui-events.log"

typedef struct {
    int srv_fd;
    int active;
    int mode;
    int screen_w;
    int screen_h;
    int content_y;
    int w;          /* logical content width (status bar excluded) */
    int h;          /* logical content height (status bar excluded) */
    uint32_t until_ms;
    unsigned tap_seq;
    uint16_t *canvas;
    char *draw_script;
} devui_ext_t;

int  devui_ext_init(devui_ext_t *s, int w, int h);
void devui_ext_close(devui_ext_t *s);

/* Poll the local control socket and TTL. Returns 1 when active state/content changed. */
int  devui_ext_poll(devui_ext_t *s, uint32_t now_ms);

int  devui_ext_active(const devui_ext_t *s);
void devui_ext_deactivate(devui_ext_t *s);
int  devui_ext_content_point(const devui_ext_t *s, int screen_x, int screen_y, int *out_x, int *out_y);
void devui_ext_render(devui_ext_t *s, drm_disp_t *disp);
void devui_ext_handle_tap(devui_ext_t *s, int x, int y, uint32_t now_ms);

#endif /* U60PRO_DEVUI_EXT_H */
