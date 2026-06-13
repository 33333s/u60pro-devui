/* Minimal DRM framebuffer test — paints a simple pattern. */
#include "drm_disp.h"
#include <string.h>
#include <unistd.h>

int main(void)
{
    drm_disp_t d;
    if (drm_disp_init(&d) != 0) return 1;
    
    int W = d.width, H = d.height, pp = d.pitch_px;
    
    /* Fill with dark gray (#15161a) */
    uint16_t bg = 0x10A3;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            d.fb[(size_t)(H-1-y)*pp + (W-1-x)] = bg;
    
    /* White crosshair */
    for (int x = 0; x < W; x++) d.fb[(size_t)(H/2-1)*pp + (W-1-x)] = 0xFFFF;
    for (int y = 0; y < H; y++) d.fb[(size_t)(H-1-y)*pp + (W/2)] = 0xFFFF;
    
    /* Green rect top-left 40x40 */
    for (int y = 10; y < 50; y++)
        for (int x = 10; x < 50; x++)
            d.fb[(size_t)(H-1-y)*pp + (W-1-x)] = 0x07E0;
    
    /* Blue rect top-right 40x40 */
    for (int y = 10; y < 50; y++)
        for (int x = W-50; x < W-10; x++)
            d.fb[(size_t)(H-1-y)*pp + (W-1-x)] = 0x001F;
    
    /* Red rect bottom-left 40x40 */
    for (int y = H-50; y < H-10; y++)
        for (int x = 10; x < 50; x++)
            d.fb[(size_t)(H-1-y)*pp + (W-1-x)] = 0xF800;
    
    /* White rect bottom-right 40x40 */
    for (int y = H-50; y < H-10; y++)
        for (int x = W-50; x < W-10; x++)
            d.fb[(size_t)(H-1-y)*pp + (W-1-x)] = 0xFFFF;
    
    drm_disp_dirty(&d, 0, 0, W-1, H-1);
    sleep(10);
    drm_disp_close(&d);
    return 0;
}
