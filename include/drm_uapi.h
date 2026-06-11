/*
 * drm_uapi.h - Minimal, self-contained DRM/KMS userspace ABI definitions.
 *
 * Only the structures and ioctls this project needs are declared here, so the
 * build does not depend on <drm/drm.h> or libdrm. These are stable Linux kernel
 * UAPI definitions (interface facts), not derived from any vendor binary.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_DRM_UAPI_H
#define U60PRO_DRM_UAPI_H

#include <stdint.h>

/* --- ioctl number construction (asm-generic, matches arm64) --- */
#ifndef DRM_IOC_BITS
#define DRM_IOC_NRBITS    8
#define DRM_IOC_TYPEBITS  8
#define DRM_IOC_SIZEBITS  14
#define DRM_IOC_DIRBITS   2
#define DRM_IOC_NRSHIFT   0
#define DRM_IOC_TYPESHIFT (DRM_IOC_NRSHIFT + DRM_IOC_NRBITS)
#define DRM_IOC_SIZESHIFT (DRM_IOC_TYPESHIFT + DRM_IOC_TYPEBITS)
#define DRM_IOC_DIRSHIFT  (DRM_IOC_SIZESHIFT + DRM_IOC_SIZEBITS)
#define DRM_IOC_DIR_READ  2U
#define DRM_IOC_DIR_WRITE 1U
#define DRM_IOC(dir, type, nr, size) \
    (((dir) << DRM_IOC_DIRSHIFT) | ((type) << DRM_IOC_TYPESHIFT) | \
     ((nr) << DRM_IOC_NRSHIFT) | ((size) << DRM_IOC_SIZESHIFT))
#define DRM_IOC_BITS 1
#endif

#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr, type) \
    DRM_IOC(DRM_IOC_DIR_READ | DRM_IOC_DIR_WRITE, DRM_IOCTL_BASE, nr, sizeof(type))

/* --- mode structures --- */
struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char     name[32];
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;     /* 1 = connected, 2 = disconnected */
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

struct drm_clip_rect {
    uint16_t x1;
    uint16_t y1;
    uint16_t x2;
    uint16_t y2;
};

struct drm_mode_fb_dirty_cmd {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
};

#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC      DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC      DRM_IOWR(0xA2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETENCODER   DRM_IOWR(0xA6, struct drm_mode_get_encoder)
#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB        DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_DIRTYFB      DRM_IOWR(0xB1, struct drm_mode_fb_dirty_cmd)
#define DRM_IOCTL_MODE_CREATE_DUMB  DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB     DRM_IOWR(0xB3, struct drm_mode_map_dumb)

#define DRM_MODE_CONNECTED 1

#endif /* U60PRO_DRM_UAPI_H */
