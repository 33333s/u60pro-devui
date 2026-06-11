/*
 * drm_disp.h - DRM/KMS dumb-buffer display backend.
 *
 * Opens the DRM device, enumerates the connected panel + crtc at runtime,
 * allocates a single dumb framebuffer mapped into userspace, and pushes
 * updates to command-mode panels via DIRTYFB.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_DRM_DISP_H
#define U60PRO_DRM_DISP_H

#include <stdint.h>

typedef struct {
    int       fd;             /* /dev/dri/cardN */
    uint16_t *fb;             /* mmap'd RGB565 scanout buffer */
    int       width;          /* visible width  (px) */
    int       height;         /* visible height (px) */
    int       pitch;          /* bytes per row */
    int       pitch_px;       /* uint16_t per row (pitch / 2) */
    uint32_t  fb_id;
    uint32_t  crtc_id;
    uint32_t  connector_id;
    uint64_t  map_size;
} drm_disp_t;

/* Returns 0 on success, negative errno-style code on failure. */
int  drm_disp_init(drm_disp_t *d);

/* Push the given rectangle (inclusive coords, framebuffer space) to the panel. */
void drm_disp_dirty(drm_disp_t *d, int x1, int y1, int x2, int y2);

void drm_disp_close(drm_disp_t *d);

#endif /* U60PRO_DRM_DISP_H */
