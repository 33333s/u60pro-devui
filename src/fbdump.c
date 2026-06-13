/* fbdump.c - tiny daemon that reads the DRM framebuffer and writes raw
 * RGB565 to /tmp/fb.dump at a configurable rate. Used for remote screen
 * mirroring during debugging — the computer pulls /tmp/fb.dump periodically.
 */
#include "drm_disp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int hz = 4;  /* default 4 fps */
    if (argc > 1) hz = atoi(argv[1]);

    drm_disp_t d;
    if (drm_disp_init(&d) != 0) return 1;

    fprintf(stderr, "fbdump: %dx%d @ %d Hz\n", d.width, d.height, hz);
    const int W = d.width, H = d.height, pp = d.pitch_px;

    while (1) {
        FILE *f = fopen("/tmp/fb.dump", "wb");
        if (f) {
            /* dump unrotated: row 0 first, within each row x=0 first */
            for (int y = 0; y < H; y++) {
                uint16_t *row = d.fb + (size_t)(H - 1 - y) * pp;
                fwrite(row, sizeof(uint16_t), W, f);
            }
            fclose(f);
        }
        usleep(1000000 / hz);
    }
    return 0;
}
