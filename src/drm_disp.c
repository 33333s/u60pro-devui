/*
 * drm_disp.c - Runtime-enumerated DRM/KMS dumb-buffer display backend.
 *
 * Clean-room implementation built only on the standard Linux DRM UAPI. No
 * vendor library, no libdrm. Hardware ids (connector/crtc/mode) are discovered
 * by walking DRM resources; hard-coded fallbacks are used only if that fails.
 *
 * SPDX-License-Identifier: MIT
 */
#include "drm_disp.h"
#include "drm_uapi.h"
#include "devui_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, (int)req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

/*
 * Fill `mode` with the preferred mode of the first connected connector, and
 * report its id. Also resolves a usable crtc id for that connector.
 * Returns 0 on success.
 */
static int pick_connector_crtc(int fd,
                               uint32_t *out_connector,
                               uint32_t *out_crtc,
                               struct drm_mode_modeinfo *out_mode)
{
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return -1;

    uint32_t *crtcs      = NULL;
    uint32_t *connectors = NULL;
    uint32_t *encoders   = NULL;
    if (res.count_crtcs)      crtcs      = calloc(res.count_crtcs, sizeof(uint32_t));
    if (res.count_connectors) connectors = calloc(res.count_connectors, sizeof(uint32_t));
    if (res.count_encoders)   encoders   = calloc(res.count_encoders, sizeof(uint32_t));

    res.crtc_id_ptr      = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    res.encoder_id_ptr   = (uint64_t)(uintptr_t)encoders;
    res.fb_id_ptr        = 0;

    int rc = -1;
    if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        goto out;

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector conn;
        memset(&conn, 0, sizeof(conn));
        conn.connector_id = connectors[i];

        /* First pass: learn how many modes/encoders this connector has. */
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
            continue;
        if (conn.connection != DRM_MODE_CONNECTED || conn.count_modes == 0)
            continue;

        struct drm_mode_modeinfo *modes =
            calloc(conn.count_modes, sizeof(struct drm_mode_modeinfo));
        uint32_t *conn_encoders = conn.count_encoders
            ? calloc(conn.count_encoders, sizeof(uint32_t)) : NULL;
        conn.modes_ptr    = (uint64_t)(uintptr_t)modes;
        conn.encoders_ptr = (uint64_t)(uintptr_t)conn_encoders;
        conn.props_ptr = 0;
        conn.prop_values_ptr = 0;
        conn.count_props = 0;

        /* Second pass: actually fetch the mode list. */
        if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 || conn.count_modes == 0) {
            free(modes);
            free(conn_encoders);
            continue;
        }

        /* Resolve a crtc: prefer the active encoder's crtc, else the first
         * crtc permitted by any of the connector's encoders. */
        uint32_t crtc_id = 0;
        struct drm_mode_get_encoder enc;
        if (conn.encoder_id) {
            memset(&enc, 0, sizeof(enc));
            enc.encoder_id = conn.encoder_id;
            if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
                crtc_id = enc.crtc_id;
        }
        if (!crtc_id && conn_encoders) {
            for (uint32_t e = 0; e < conn.count_encoders && !crtc_id; e++) {
                memset(&enc, 0, sizeof(enc));
                enc.encoder_id = conn_encoders[e];
                if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
                    continue;
                for (uint32_t c = 0; c < res.count_crtcs; c++) {
                    if (enc.possible_crtcs & (1u << c)) {
                        crtc_id = crtcs[c];
                        break;
                    }
                }
            }
        }
        if (!crtc_id && res.count_crtcs)
            crtc_id = crtcs[0];

        if (crtc_id) {
            *out_connector = conn.connector_id;
            *out_crtc      = crtc_id;
            *out_mode      = modes[0];   /* preferred mode is listed first */
            rc = 0;
        }
        free(modes);
        free(conn_encoders);
        if (rc == 0)
            break;
    }

out:
    free(crtcs);
    free(connectors);
    free(encoders);
    return rc;
}

static void fill_fallback_mode(struct drm_mode_modeinfo *m, int w, int h)
{
    memset(m, 0, sizeof(*m));
    m->clock        = 1;
    m->hdisplay     = (uint16_t)w;
    m->hsync_start  = (uint16_t)w;
    m->hsync_end    = (uint16_t)w;
    m->htotal       = (uint16_t)w;
    m->vdisplay     = (uint16_t)h;
    m->vsync_start  = (uint16_t)h;
    m->vsync_end    = (uint16_t)h;
    m->vtotal       = (uint16_t)h;
    m->vrefresh     = 1;
    m->type         = 0x48;
    snprintf(m->name, sizeof(m->name), "%dx%d", w, h);
}

int drm_disp_init(drm_disp_t *d)
{
    memset(d, 0, sizeof(*d));

    d->fd = open(DEVUI_DRM_CARD, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) {
        fprintf(stderr, "drm: open %s failed: %s\n", DEVUI_DRM_CARD, strerror(errno));
        return -1;
    }

    struct drm_mode_modeinfo mode;
    uint32_t connector_id = 0, crtc_id = 0;

    if (pick_connector_crtc(d->fd, &connector_id, &crtc_id, &mode) == 0) {
        fprintf(stderr, "drm: enumerated connector=%u crtc=%u mode=%s (%ux%u)\n",
                connector_id, crtc_id, mode.name, mode.hdisplay, mode.vdisplay);
    } else {
        connector_id = DEVUI_FALLBACK_CONNECTOR_ID;
        crtc_id      = DEVUI_FALLBACK_CRTC_ID;
        fill_fallback_mode(&mode, DEVUI_FALLBACK_WIDTH, DEVUI_FALLBACK_HEIGHT);
        fprintf(stderr, "drm: enumeration failed, using fallback connector=%u crtc=%u %ux%u\n",
                connector_id, crtc_id, mode.hdisplay, mode.vdisplay);
    }

    d->connector_id = connector_id;
    d->crtc_id      = crtc_id;
    d->width        = mode.hdisplay;
    d->height       = mode.vdisplay;

    /* Allocate a 16bpp (RGB565) dumb buffer. */
    struct drm_mode_create_dumb dumb;
    memset(&dumb, 0, sizeof(dumb));
    dumb.width  = d->width;
    dumb.height = d->height;
    dumb.bpp    = 16;
    if (xioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) < 0) {
        fprintf(stderr, "drm: CREATE_DUMB failed: %s\n", strerror(errno));
        goto fail;
    }
    d->pitch    = dumb.pitch;
    d->pitch_px = dumb.pitch / 2;
    d->map_size = dumb.size;

    struct drm_mode_fb_cmd fb;
    memset(&fb, 0, sizeof(fb));
    fb.width  = d->width;
    fb.height = d->height;
    fb.pitch  = dumb.pitch;
    fb.bpp    = 16;
    fb.depth  = 16;
    fb.handle = dumb.handle;
    if (xioctl(d->fd, DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        fprintf(stderr, "drm: ADDFB failed: %s\n", strerror(errno));
        goto fail;
    }
    d->fb_id = fb.fb_id;

    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = dumb.handle;
    if (xioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        fprintf(stderr, "drm: MAP_DUMB failed: %s\n", strerror(errno));
        goto fail;
    }

    void *addr = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      d->fd, (off_t)map.offset);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "drm: mmap failed: %s\n", strerror(errno));
        goto fail;
    }
    d->fb = (uint16_t *)addr;
    memset(d->fb, 0, dumb.size);

    /* Scan out our framebuffer on the chosen crtc + connector. */
    struct drm_mode_crtc set;
    memset(&set, 0, sizeof(set));
    uint32_t conn = connector_id;
    set.set_connectors_ptr = (uint64_t)(uintptr_t)&conn;
    set.count_connectors   = 1;
    set.crtc_id            = crtc_id;
    set.fb_id              = d->fb_id;
    set.mode_valid         = 1;
    set.mode               = mode;
    if (xioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
        fprintf(stderr, "drm: SETCRTC failed: %s\n", strerror(errno));
        goto fail;
    }

    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
    return 0;

fail:
    drm_disp_close(d);
    return -1;
}

void drm_disp_dirty(drm_disp_t *d, int x1, int y1, int x2, int y2)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= d->width)  x2 = d->width - 1;
    if (y2 >= d->height) y2 = d->height - 1;
    if (x2 < x1 || y2 < y1)
        return;

    struct drm_clip_rect clip;
    clip.x1 = (uint16_t)x1;
    clip.y1 = (uint16_t)y1;
    clip.x2 = (uint16_t)(x2 + 1);   /* exclusive */
    clip.y2 = (uint16_t)(y2 + 1);

    struct drm_mode_fb_dirty_cmd dirty;
    memset(&dirty, 0, sizeof(dirty));
    dirty.fb_id     = d->fb_id;
    dirty.num_clips = 1;
    dirty.clips_ptr = (uint64_t)(uintptr_t)&clip;
    /* Some KMS drivers don't implement DIRTYFB; ignore -EINVAL/-ENOSYS. */
    (void)xioctl(d->fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
}

void drm_disp_close(drm_disp_t *d)
{
    if (d->fb && d->fb != MAP_FAILED && d->map_size)
        munmap(d->fb, d->map_size);
    if (d->fd >= 0)
        close(d->fd);
    d->fb = NULL;
    d->fd = -1;
}
