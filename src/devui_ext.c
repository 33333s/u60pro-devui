/*
 * devui_ext.c - local external display channel.
 *
 * Protocol: one Unix-socket connection carries one command.
 *
 *   PING
 *   CLOSE
 *   FRAME rgb565 <w> <h> <ttl_ms>\n<little-endian RGB565 payload>
 *   IMAGE <ttl_ms> <fit|stretch|cover> <path>
 *   DRAW <ttl_ms> [len]\n<ops text, or lines ending with END>
 *   TEXT <ttl_ms> [len]\n<plain UTF-8 text>
 *
 * ttl_ms = 0 means "stay until CLOSE or power-key escape".
 *
 * SPDX-License-Identifier: MIT
 */
#include "devui_ext.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define STBI_MAX_DIMENSIONS 4096
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

extern void html_view_set_scroll(int y);
extern int  html_view_render_to_size(uint16_t *buf, int w, int h, const char *html);
extern int  html_view_text_width_px(const char *text, int size);
extern void html_view_draw_text_px(int x, int y, const char *text, int size, int bold,
                                   int r, int g, int b, int a);

enum {
    EXT_MODE_NONE = 0,
    EXT_MODE_CANVAS = 1,
    EXT_MODE_DRAW = 2,
};

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static uint16_t rgb565(int r, int g, int b)
{
    r = clampi(r, 0, 255);
    g = clampi(g, 0, 255);
    b = clampi(b, 0, 255);
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void rgb565_split(uint16_t px, int *r, int *g, int *b)
{
    *r = ((px >> 11) & 0x1f) * 255 / 31;
    *g = ((px >> 5) & 0x3f) * 255 / 63;
    *b = (px & 0x1f) * 255 / 31;
}

static uint16_t blend565(uint16_t dst, int r, int g, int b, int a)
{
    if (a >= 255) return rgb565(r, g, b);
    if (a <= 0) return dst;
    int dr, dg, db;
    rgb565_split(dst, &dr, &dg, &db);
    r = (r * a + dr * (255 - a)) / 255;
    g = (g * a + dg * (255 - a)) / 255;
    b = (b * a + db * (255 - a)) / 255;
    return rgb565(r, g, b);
}

static void canvas_clear(devui_ext_t *s, int r, int g, int b)
{
    if (!s->canvas) return;
    uint16_t px = rgb565(r, g, b);
    for (int i = 0; i < s->w * s->h; i++) s->canvas[i] = px;
}

static void canvas_px(devui_ext_t *s, int x, int y, int r, int g, int b, int a)
{
    if (!s->canvas || x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    uint16_t *p = &s->canvas[y * s->w + x];
    *p = blend565(*p, r, g, b, a);
}

static void canvas_rect(devui_ext_t *s, int x, int y, int w, int h, int r, int g, int b, int a)
{
    if (w <= 0 || h <= 0) return;
    int x1 = clampi(x, 0, s->w);
    int y1 = clampi(y, 0, s->h);
    int x2 = clampi(x + w, 0, s->w);
    int y2 = clampi(y + h, 0, s->h);
    for (int yy = y1; yy < y2; yy++)
        for (int xx = x1; xx < x2; xx++)
            canvas_px(s, xx, yy, r, g, b, a);
}

static int in_round(int x, int y, int rx, int ry, int rw, int rh, int rad)
{
    if (x < rx || y < ry || x >= rx + rw || y >= ry + rh) return 0;
    if (rad <= 0) return 1;
    int cx = 0, cy = 0;
    if (x < rx + rad && y < ry + rad) {
        cx = rx + rad; cy = ry + rad;
    } else if (x >= rx + rw - rad && y < ry + rad) {
        cx = rx + rw - 1 - rad; cy = ry + rad;
    } else if (x >= rx + rw - rad && y >= ry + rh - rad) {
        cx = rx + rw - 1 - rad; cy = ry + rh - 1 - rad;
    } else if (x < rx + rad && y >= ry + rh - rad) {
        cx = rx + rad; cy = ry + rh - 1 - rad;
    } else {
        return 1;
    }
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= rad * rad;
}

static void canvas_round_rect(devui_ext_t *s, int x, int y, int w, int h, int rad,
                              int r, int g, int b, int a)
{
    if (w <= 0 || h <= 0) return;
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    int x1 = clampi(x, 0, s->w);
    int y1 = clampi(y, 0, s->h);
    int x2 = clampi(x + w, 0, s->w);
    int y2 = clampi(y + h, 0, s->h);
    for (int yy = y1; yy < y2; yy++)
        for (int xx = x1; xx < x2; xx++)
            if (in_round(xx, yy, x, y, w, h, rad))
                canvas_px(s, xx, yy, r, g, b, a);
}

static void canvas_line(devui_ext_t *s, int x0, int y0, int x1, int y1,
                        int r, int g, int b, int thick)
{
    if (thick < 1) thick = 1;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int yy = 0; yy < thick; yy++)
            for (int xx = 0; xx < thick; xx++)
                canvas_px(s, x0 + xx - thick / 2, y0 + yy - thick / 2, r, g, b, 255);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int wait_readable(int fd, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc;
    do {
        rc = select(fd + 1, &rfds, NULL, NULL, &tv);
    } while (rc < 0 && errno == EINTR);
    return rc > 0;
}

static int read_exact(int fd, unsigned char *buf, size_t len, int timeout_ms)
{
    size_t got = 0;
    while (got < len) {
        if (!wait_readable(fd, timeout_ms)) return 0;
        ssize_t r = read(fd, buf + got, len - got);
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

static int read_line(int fd, char *out, size_t cap, int timeout_ms)
{
    if (cap == 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        unsigned char c;
        if (!read_exact(fd, &c, 1, timeout_ms)) return -1;
        if (c == '\n') break;
        out[n++] = (char)c;
    }
    out[n] = 0;
    if (n > 0 && out[n - 1] == '\r') out[n - 1] = 0;
    return (int)n;
}

static int append_bytes(char **buf, size_t *len, size_t *cap, const void *src, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap * 2 : 4096;
        while (*len + n + 1 > nc) nc *= 2;
        if (nc > 65536) return 0;
        char *nb = realloc(*buf, nc);
        if (!nb) return 0;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, src, n);
    *len += n;
    (*buf)[*len] = 0;
    return 1;
}

static char *read_rest(int fd, long len)
{
    char *buf = NULL;
    size_t used = 0, cap = 0;
    if (len >= 0) {
        if (len > 65535) return NULL;
        buf = malloc((size_t)len + 1);
        if (!buf) return NULL;
        if (!read_exact(fd, (unsigned char *)buf, (size_t)len, 500)) {
            free(buf);
            return NULL;
        }
        buf[len] = 0;
        return buf;
    }

    for (;;) {
        unsigned char tmp[1024];
        if (!wait_readable(fd, 150)) break;
        ssize_t r = read(fd, tmp, sizeof tmp);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!append_bytes(&buf, &used, &cap, tmp, (size_t)r)) {
            free(buf);
            return NULL;
        }
    }
    if (!buf) {
        buf = malloc(1);
        if (buf) buf[0] = 0;
    }
    return buf;
}

static char *read_draw_body(int fd, long len)
{
    if (len >= 0) return read_rest(fd, len);

    char *buf = NULL;
    size_t used = 0, cap = 0;
    for (;;) {
        char line[1024];
        int n = read_line(fd, line, sizeof line, 500);
        if (n < 0) break;
        if (!strcmp(line, "END")) break;
        if (!append_bytes(&buf, &used, &cap, line, (size_t)n) ||
            !append_bytes(&buf, &used, &cap, "\n", 1)) {
            free(buf);
            return NULL;
        }
    }
    if (!buf) {
        buf = malloc(1);
        if (buf) buf[0] = 0;
    }
    return buf;
}

static void reply_ok(int fd, const char *msg)
{
    dprintf(fd, "OK %s\n", msg ? msg : "");
}

static void reply_err(int fd, const char *msg)
{
    dprintf(fd, "ERR %s\n", msg ? msg : "");
}

static void activate(devui_ext_t *s, int mode, int ttl_ms, uint32_t now_ms)
{
    s->active = 1;
    s->mode = mode;
    s->until_ms = ttl_ms > 0 ? now_ms + (uint32_t)ttl_ms : 0;
    s->tap_seq = 0;
    unlink(DEVUI_EXT_EVENT_PATH);
    FILE *fp = fopen(DEVUI_EXT_EVENT_PATH, "w");
    if (fp) fclose(fp);
}

static void copy_frame(devui_ext_t *s, const unsigned char *src, int sw, int sh)
{
    for (int y = 0; y < s->h; y++) {
        int sy = y * sh / s->h;
        for (int x = 0; x < s->w; x++) {
            int sx = x * sw / s->w;
            size_t i = ((size_t)sy * sw + sx) * 2;
            s->canvas[y * s->w + x] = (uint16_t)(src[i] | (src[i + 1] << 8));
        }
    }
}

static void image_to_canvas(devui_ext_t *s, const unsigned char *img, int iw, int ih, const char *mode)
{
    canvas_clear(s, 0, 0, 0);
    double sx = (double)s->w / iw;
    double sy = (double)s->h / ih;
    int ox = 0, oy = 0, ow = s->w, oh = s->h;

    if (strcmp(mode, "stretch")) {
        double sc = (!strcmp(mode, "cover")) ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
        ow = (int)(iw * sc + 0.5);
        oh = (int)(ih * sc + 0.5);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        ox = (s->w - ow) / 2;
        oy = (s->h - oh) / 2;
    }

    for (int y = 0; y < s->h; y++) {
        if (y < oy || y >= oy + oh) continue;
        int iy = (y - oy) * ih / oh;
        if (iy < 0 || iy >= ih) continue;
        for (int x = 0; x < s->w; x++) {
            if (x < ox || x >= ox + ow) continue;
            int ix = (x - ox) * iw / ow;
            if (ix < 0 || ix >= iw) continue;
            const unsigned char *p = img + ((size_t)iy * iw + ix) * 4;
            canvas_px(s, x, y, p[0], p[1], p[2], p[3]);
        }
    }
}

static char *html_escape_text(const char *text)
{
    size_t n = strlen(text);
    char *out = malloc(n * 6 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '&')      { memcpy(out + o, "&amp;", 5); o += 5; }
        else if (c == '<') { memcpy(out + o, "&lt;", 4); o += 4; }
        else if (c == '>') { memcpy(out + o, "&gt;", 4); o += 4; }
        else if (c == '"') { memcpy(out + o, "&quot;", 6); o += 6; }
        else if (c == '\n') { memcpy(out + o, "<br>", 4); o += 4; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
    return out;
}

static int text_to_canvas(devui_ext_t *s, const char *text)
{
    char *esc = html_escape_text(text);
    if (!esc) return 0;
    const char *head =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<style>"
        "body{margin:0;background:#090d14;color:#e9eef7;font-family:sans-serif;}"
        ".txt{padding:18px;font-size:18px;line-height:1.35;}"
        ".muted{color:#8d98a8;font-size:12px;margin-bottom:10px;}"
        "</style></head><body><div class='txt'>";
    const char *tail = "</div></body></html>";
    size_t need = strlen(head) + strlen(esc) + strlen(tail) + 1;
    char *html = malloc(need);
    if (!html) {
        free(esc);
        return 0;
    }
    snprintf(html, need, "%s%s%s", head, esc, tail);
    html_view_set_scroll(0);
    html_view_render_to_size(s->canvas, s->w, s->h, html);
    free(html);
    free(esc);
    return 1;
}

static void present_canvas(devui_ext_t *s, drm_disp_t *disp)
{
    if (!s->canvas) return;
    int top = s->content_y;
    int w = disp->width < s->w ? disp->width : s->w;
    int h = (disp->height - top) < s->h ? (disp->height - top) : s->h;
    if (h <= 0) return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int dy = top + y;
            disp->fb[(size_t)(disp->height - 1 - dy) * disp->pitch_px + (disp->width - 1 - x)] =
                s->canvas[y * s->w + x];
        }
    }
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    return s;
}

static void replay_draw_pass(devui_ext_t *s, int text_pass)
{
    char *copy = strdup(s->draw_script ? s->draw_script : "");
    if (!copy) return;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        int x, y, w, h, r, g, b, a = 255, rad, x1, y1, thick, size, nread;

        if (!text_pass) {
            if (sscanf(p, "CLEAR %d %d %d", &r, &g, &b) == 3) {
                canvas_clear(s, r, g, b);
            } else if (sscanf(p, "RECT %d %d %d %d %d %d %d %d", &x, &y, &w, &h, &r, &g, &b, &a) >= 7) {
                canvas_rect(s, x, y, w, h, r, g, b, a);
            } else if (sscanf(p, "ROUNDRECT %d %d %d %d %d %d %d %d %d",
                              &x, &y, &w, &h, &rad, &r, &g, &b, &a) >= 8 ||
                       sscanf(p, "RRECT %d %d %d %d %d %d %d %d %d",
                              &x, &y, &w, &h, &rad, &r, &g, &b, &a) >= 8) {
                canvas_round_rect(s, x, y, w, h, rad, r, g, b, a);
            } else if (sscanf(p, "LINE %d %d %d %d %d %d %d %d",
                              &x, &y, &x1, &y1, &r, &g, &b, &thick) >= 7) {
                canvas_line(s, x, y, x1, y1, r, g, b, thick);
            }
        } else {
            nread = 0;
            if (sscanf(p, "TEXT %d %d %d %d %d %d %n", &x, &y, &size, &r, &g, &b, &nread) == 6 && nread > 0) {
                if (y >= 0 && y < s->h)
                    html_view_draw_text_px(x, s->content_y + y, p + nread, size, 0, r, g, b, 255);
            } else if (sscanf(p, "CENTER %d %d %d %d %d %d %d %d %n",
                              &x, &y, &w, &h, &size, &r, &g, &b, &nread) == 8 && nread > 0) {
                const char *txt = p + nread;
                int tw = html_view_text_width_px(txt, size);
                if (y + h > 0 && y < s->h)
                    html_view_draw_text_px(x + (w - tw) / 2, s->content_y + y + (h - size) / 2,
                                           txt, size, 0, r, g, b, 255);
            }
        }
    }
    free(copy);
}

static void render_draw(devui_ext_t *s, drm_disp_t *disp)
{
    canvas_clear(s, 0, 0, 0);
    replay_draw_pass(s, 0);
    present_canvas(s, disp);
    replay_draw_pass(s, 1);
}

static int process_client(devui_ext_t *s, int cfd, uint32_t now_ms)
{
    char line[768];
    int changed = 0;
    if (read_line(cfd, line, sizeof line, 500) < 0) {
        reply_err(cfd, "bad-command");
        return 0;
    }

    if (!strcmp(line, "PING")) {
        reply_ok(cfd, "pong");
    } else if (!strcmp(line, "CLOSE")) {
        if (s->active) changed = 1;
        devui_ext_deactivate(s);
        reply_ok(cfd, "closed");
    } else if (!strncmp(line, "FRAME ", 6)) {
        char fmt[16];
        int sw = 0, sh = 0, ttl = 0;
        if (sscanf(line, "FRAME %15s %d %d %d", fmt, &sw, &sh, &ttl) != 4 ||
            strcmp(fmt, "rgb565") || sw <= 0 || sh <= 0 ||
            sw > 1024 || sh > 1024 || (long)sw * sh * 2 > 4L * 1024 * 1024) {
            reply_err(cfd, "bad-frame-header");
        } else {
            size_t bytes = (size_t)sw * sh * 2;
            unsigned char *payload = malloc(bytes);
            if (!payload) {
                reply_err(cfd, "oom");
            } else if (!read_exact(cfd, payload, bytes, 1000)) {
                free(payload);
                reply_err(cfd, "short-frame");
            } else {
                copy_frame(s, payload, sw, sh);
                free(payload);
                free(s->draw_script);
                s->draw_script = NULL;
                activate(s, EXT_MODE_CANVAS, ttl, now_ms);
                changed = 1;
                reply_ok(cfd, "frame");
            }
        }
    } else if (!strncmp(line, "IMAGE ", 6)) {
        int ttl = 0;
        char mode[16] = "fit";
        char path[512] = "";
        if (sscanf(line, "IMAGE %d %15s %511[^\n]", &ttl, mode, path) < 3 ||
            (strcmp(mode, "fit") && strcmp(mode, "stretch") && strcmp(mode, "cover"))) {
            reply_err(cfd, "bad-image-header");
        } else {
            int iw = 0, ih = 0, comp = 0;
            unsigned char *img = stbi_load(path, &iw, &ih, &comp, 4);
            if (!img || iw <= 0 || ih <= 0) {
                reply_err(cfd, stbi_failure_reason());
                if (img) stbi_image_free(img);
            } else {
                image_to_canvas(s, img, iw, ih, mode);
                stbi_image_free(img);
                free(s->draw_script);
                s->draw_script = NULL;
                activate(s, EXT_MODE_CANVAS, ttl, now_ms);
                changed = 1;
                reply_ok(cfd, "image");
            }
        }
    } else if (!strncmp(line, "TEXT ", 5)) {
        int ttl = 0;
        long len = -1;
        if (sscanf(line, "TEXT %d %ld", &ttl, &len) < 1) {
            reply_err(cfd, "bad-text-header");
        } else {
            char *body = read_rest(cfd, len);
            if (!body) {
                reply_err(cfd, "bad-text-body");
            } else if (!text_to_canvas(s, body)) {
                free(body);
                reply_err(cfd, "render-text-failed");
            } else {
                free(body);
                free(s->draw_script);
                s->draw_script = NULL;
                activate(s, EXT_MODE_CANVAS, ttl, now_ms);
                changed = 1;
                reply_ok(cfd, "text");
            }
        }
    } else if (!strncmp(line, "DRAW ", 5)) {
        int ttl = 0;
        long len = -1;
        if (sscanf(line, "DRAW %d %ld", &ttl, &len) < 1) {
            reply_err(cfd, "bad-draw-header");
        } else {
            char *body = read_draw_body(cfd, len);
            if (!body) {
                reply_err(cfd, "bad-draw-body");
            } else {
                free(s->draw_script);
                s->draw_script = body;
                activate(s, EXT_MODE_DRAW, ttl, now_ms);
                changed = 1;
                reply_ok(cfd, "draw");
            }
        }
    } else {
        reply_err(cfd, "unknown-command");
    }
    return changed;
}

int devui_ext_init(devui_ext_t *s, int w, int h)
{
    memset(s, 0, sizeof(*s));
    s->srv_fd = -1;
    s->screen_w = w;
    s->screen_h = h;
    s->content_y = DEVUI_EXT_STATUSBAR_H;
    s->w = w;
    s->h = h - s->content_y;
    if (s->h <= 0) return -1;
    s->canvas = calloc((size_t)s->w * s->h, sizeof(uint16_t));
    if (!s->canvas) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        free(s->canvas);
        s->canvas = NULL;
        return -1;
    }
    set_nonblock(fd);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", DEVUI_EXT_SOCK_PATH);
    unlink(DEVUI_EXT_SOCK_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        free(s->canvas);
        s->canvas = NULL;
        return -1;
    }
    chmod(DEVUI_EXT_SOCK_PATH, 0666);
    if (listen(fd, 4) < 0) {
        close(fd);
        unlink(DEVUI_EXT_SOCK_PATH);
        free(s->canvas);
        s->canvas = NULL;
        return -1;
    }
    s->srv_fd = fd;
    return 0;
}

void devui_ext_close(devui_ext_t *s)
{
    if (s->srv_fd >= 0) close(s->srv_fd);
    s->srv_fd = -1;
    unlink(DEVUI_EXT_SOCK_PATH);
    free(s->canvas);
    s->canvas = NULL;
    free(s->draw_script);
    s->draw_script = NULL;
    s->active = 0;
}

int devui_ext_poll(devui_ext_t *s, uint32_t now_ms)
{
    int changed = 0;
    if (s->active && s->until_ms && (int32_t)(now_ms - s->until_ms) >= 0) {
        devui_ext_deactivate(s);
        changed = 1;
    }
    if (s->srv_fd < 0) return changed;

    for (int i = 0; i < 4; i++) {
        int cfd = accept(s->srv_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        changed |= process_client(s, cfd, now_ms);
        close(cfd);
    }
    return changed;
}

int devui_ext_active(const devui_ext_t *s)
{
    return s && s->active;
}

void devui_ext_deactivate(devui_ext_t *s)
{
    if (!s) return;
    s->active = 0;
    s->mode = EXT_MODE_NONE;
    s->until_ms = 0;
}

int devui_ext_content_point(const devui_ext_t *s, int screen_x, int screen_y, int *out_x, int *out_y)
{
    if (!s) return 0;
    if (screen_x < 0 || screen_x >= s->screen_w) return 0;
    if (screen_y < s->content_y || screen_y >= s->content_y + s->h) return 0;
    if (out_x) *out_x = screen_x;
    if (out_y) *out_y = screen_y - s->content_y;
    return 1;
}

void devui_ext_render(devui_ext_t *s, drm_disp_t *disp)
{
    if (!s || !s->active) return;
    if (s->mode == EXT_MODE_DRAW) render_draw(s, disp);
    else                         present_canvas(s, disp);
}

void devui_ext_handle_tap(devui_ext_t *s, int x, int y, uint32_t now_ms)
{
    if (!s || !s->active) return;
    FILE *fp = fopen(DEVUI_EXT_EVENT_PATH, "a");
    if (!fp) return;
    fprintf(fp, "{\"event\":\"tap\",\"seq\":%u,\"x\":%d,\"y\":%d,\"t\":%u}\n",
            ++s->tap_seq, x, y, now_ms);
    fclose(fp);
}
