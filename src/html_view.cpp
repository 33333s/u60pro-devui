/*
 * html_view.cpp - render an HTML/CSS page to the RGB565 framebuffer using
 * litehtml (layout/CSS) + FreeType (text). devui becomes a thin HTML shell:
 * the UI is authored in ui/index.html, this draws it.
 *
 * No JavaScript; CSS grid is unsupported by litehtml (falls back to block).
 *
 * SPDX-License-Identifier: MIT
 */
#include <litehtml.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <sys/stat.h>
#include <unordered_map>

using namespace litehtml;

/* ---- target framebuffer (shared with drm_disp, 180° rotated panel) ---- */
static uint16_t *g_fb;
static int g_w, g_h, g_pitch_px, g_rotate = 1;
static int g_clip_top = 0;

static inline void put_px(int x, int y, int r, int g, int b, int a)
{
    if (y < g_clip_top) return;
    if (x < 0 || y < 0 || x >= g_w || y >= g_h || a <= 0) return;
    int dx = g_rotate ? (g_w - 1 - x) : x;
    int dy = g_rotate ? (g_h - 1 - y) : y;
    uint16_t *p = &g_fb[dy * g_pitch_px + dx];
    if (a < 255) {
        uint16_t o = *p;
        int orr = ((o >> 11) & 0x1F) << 3, og = ((o >> 5) & 0x3F) << 2, ob = (o & 0x1F) << 3;
        r = (r * a + orr * (255 - a)) / 255;
        g = (g * a + og * (255 - a)) / 255;
        b = (b * a + ob * (255 - a)) / 255;
    }
    *p = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* put_px with 4x4 ordered dithering: a smooth gradient quantized straight to
 * RGB565 (5/6/5 bits) shows bands; spreading the rounding error over a fixed
 * pattern hides them. */
static const uint8_t k_bayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static inline void put_px_dither(int x, int y, int r, int g, int b, int a)
{
    if (y < g_clip_top) return;
    if (x < 0 || y < 0 || x >= g_w || y >= g_h || a <= 0) return;
    int dx = g_rotate ? (g_w - 1 - x) : x;
    int dy = g_rotate ? (g_h - 1 - y) : y;
    uint16_t *p = &g_fb[dy * g_pitch_px + dx];
    if (a < 255) {
        uint16_t o = *p;
        int orr = ((o >> 11) & 0x1F) << 3, og = ((o >> 5) & 0x3F) << 2, ob = (o & 0x1F) << 3;
        r = (r * a + orr * (255 - a)) / 255;
        g = (g * a + og * (255 - a)) / 255;
        b = (b * a + ob * (255 - a)) / 255;
    }
    int t = k_bayer4[y & 3][x & 3];          /* 0..15: threshold within one quantization step */
    r += t >> 1; g += t >> 2; b += t >> 1;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    *p = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* The opaque RGB565 pixel put_px_dither writes at (x, y). */
static inline uint16_t dither565(int x, int y, int r, int g, int b)
{
    int t = k_bayer4[y & 3][x & 3];
    r += t >> 1; g += t >> 2; b += t >> 1;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint16_t get_px565(int x, int y)
{
    if (x < 0 || y < 0 || x >= g_w || y >= g_h || !g_fb) return 0;
    int dx = g_rotate ? (g_w - 1 - x) : x;
    int dy = g_rotate ? (g_h - 1 - y) : y;
    return g_fb[dy * g_pitch_px + dx];
}

/* Is (x,y) inside a rounded rectangle? Used by both rounded fill and rounded
 * border stroking (litehtml hands us radii but leaves the drawing to us). */
/* Coverage (0..1) of pixel (x, y) by a rounded rect [fx, fx+fw) x [fy, fy+fh)
 * with per-corner radii, from the signed distance of the pixel centre to the
 * edge, so curved edges blend over about one pixel instead of stair-stepping.
 * Straight edges on whole pixels still come out as exactly 0 or 1. */
static float round_cov(int x, int y, int fx, int fy, int fw, int fh,
                       int rtl, int rtr, int rbr, int rbl)
{
    if (fw <= 0 || fh <= 0 || x < fx || y < fy || x >= fx + fw || y >= fy + fh) return 0.f;
    float hw = fw * .5f, hh = fh * .5f;
    float px = (float)x + .5f - ((float)fx + hw), py = (float)y + .5f - ((float)fy + hh);
    float r = (float)(py < 0.f ? (px < 0.f ? rtl : rtr) : (px < 0.f ? rbl : rbr));
    if (r > hw) r = hw;
    if (r > hh) r = hh;
    float qx = std::fabs(px) - (hw - r), qy = std::fabs(py) - (hh - r);
    float ox = qx > 0.f ? qx : 0.f, oy = qy > 0.f ? qy : 0.f;
    float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
    float c = .5f - d;
    return c <= 0.f ? 0.f : c >= 1.f ? 1.f : c;
}

static inline int cov_alpha(float cov, int a) { return (int)(cov * (float)a + .5f); }

/* ---- FreeType ---- */
static FT_Library g_ft;
static FT_Face    g_face;

struct ft_font { int size; int ascent, descent, height; };

/* Rendered glyphs by (pixel size, code point). Loading and rasterizing a glyph
 * through FreeType costs far more than drawing it, and a page uses the same few
 * dozen glyphs on every refresh. */
struct glyph {
    int adv = 0, left = 0, top = 0, w = 0, h = 0;
    std::vector<uint8_t> px;   /* w*h coverage */
};
static std::unordered_map<uint32_t, glyph> g_glyphs;
static int g_ascent[256];      /* ascender per pixel size, 0 = not looked up */

static const glyph &glyph_get(unsigned cp, int size)
{
    uint32_t key = ((uint32_t)(size & 0x7FF) << 21) | (cp & 0x1FFFFF);
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return it->second;
    if (g_glyphs.size() >= 4096) g_glyphs.clear();
    glyph gl;
    FT_Set_Pixel_Sizes(g_face, 0, size);
    if (!FT_Load_Char(g_face, cp, FT_LOAD_RENDER)) {
        FT_GlyphSlot s = g_face->glyph;
        gl.adv = (int)(s->advance.x >> 6);
        gl.left = s->bitmap_left; gl.top = s->bitmap_top;
        gl.w = (int)s->bitmap.width; gl.h = (int)s->bitmap.rows;
        gl.px.resize((size_t)gl.w * gl.h);
        for (int r = 0; r < gl.h; r++)
            memcpy(&gl.px[(size_t)r * gl.w], s->bitmap.buffer + r * s->bitmap.pitch, (size_t)gl.w);
    }
    return g_glyphs.emplace(key, std::move(gl)).first->second;
}

static int font_ascent(int size)
{
    if (size > 0 && size < 256 && g_ascent[size]) return g_ascent[size];
    FT_Set_Pixel_Sizes(g_face, 0, size);
    int a = (int)(g_face->size->metrics.ascender >> 6);
    if (size > 0 && size < 256) g_ascent[size] = a;
    return a;
}

/* UI base dir (for <link> CSS / images) and last-clicked anchor href. */
static std::string g_ui_dir = "/data/plugins/zwrt-devui/ui";
static std::string g_clicked;

static long long css_file_mtime_ns(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return -1;
    return (long long)st.st_mtime * 1000000000LL + (long long)st.st_mtim.tv_nsec;
}

struct css_cache_entry {
    std::string path;
    std::string text;
    long long mtime = -1;
    bool used = false;
};

static css_cache_entry g_css_cache[4];
static int g_css_cache_next;

static int read_file_text(const char *path, std::string &out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    long n;
    int ok = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        n = ftell(f);
        if (n >= 0) {
            if (n > 0) {
                out.resize((size_t)n);
                fseek(f, 0, SEEK_SET);
                ok = (fread(&out[0], 1, (size_t)n, f) == (size_t)n);
            } else {
                out.clear();
                ok = 1;
            }
        }
    }
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

static void css_get_cached(std::string &out, const std::string &path)
{
    long long mtime = css_file_mtime_ns(path);
    if (mtime < 0) return;

    for (int i = 0; i < 4; i++) {
        css_cache_entry &e = g_css_cache[i];
        if (!e.used || e.path != path) continue;
        if (e.mtime == mtime && !e.text.empty()) {
            out = e.text;
            return;
        }
        if (read_file_text(path.c_str(), e.text)) {
            e.mtime = mtime;
            out = e.text;
            return;
        }
        out = e.text;
        return;
    }

    css_cache_entry *e = nullptr;
    for (int i = 0; i < 4; i++) {
        if (!g_css_cache[i].used) { e = &g_css_cache[i]; break; }
    }
    if (!e) {
        e = &g_css_cache[g_css_cache_next];
        g_css_cache_next = (g_css_cache_next + 1) % 4;
    }
    std::string next;
    if (!read_file_text(path.c_str(), next)) return;
    e->path = path;
    e->text = std::move(next);
    e->mtime = mtime;
    e->used = true;
    out = e->text;
}

static unsigned utf8_next(const char *&s)
{
    unsigned c = (unsigned char)*s++;
    if (c < 0x80) return c;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : 1;
    c &= (0x3F >> n);
    while (n-- && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3F);
    return c;
}

static int text_advance(const char *text, int size)
{
    int w = 0;
    for (const char *s = text; *s; ) w += glyph_get(utf8_next(s), size).adv;
    return w;
}

/* litehtml lays out in fractional pixels. Every draw path snaps an edge with
 * the same rounding, so a box, its text and a native icon placed over it move
 * together instead of truncating to different rows. */
static inline int R(pixel_t v) { return (int)floorf(v + 0.5f); }

/* ---- container ---- */
class fb_container : public document_container {
    std::vector<position> m_clip;

    position eff_clip() const {
        position r(0, 0, (pixel_t)g_w, (pixel_t)g_h);
        for (auto &c : m_clip) {
            pixel_t x1 = std::max(r.left(), c.left()), y1 = std::max(r.top(), c.top());
            pixel_t x2 = std::min(r.right(), c.right()), y2 = std::min(r.bottom(), c.bottom());
            r = position(x1, y1, std::max<pixel_t>(0, x2 - x1), std::max<pixel_t>(0, y2 - y1));
        }
        return r;
    }
    void fill(pixel_t fx, pixel_t fy, pixel_t fw, pixel_t fh, web_color c) {
        if (c.alpha == 0) return;
        position cl = eff_clip();
        int x1 = std::max(R(fx), R(cl.left())),  y1 = std::max(R(fy), R(cl.top()));
        int x2 = std::min(R(fx + fw), R(cl.right())), y2 = std::min(R(fy + fh), R(cl.bottom()));
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++)
                put_px(x, y, c.red, c.green, c.blue, c.alpha);
    }
    /* Filled rectangle with rounded corners. litehtml only hands us the radii;
     * it's up to the container to honor them. Corners are anti-aliased. */
    void fill_rounded(int fx, int fy, int fw, int fh, int rtl, int rtr, int rbr, int rbl, web_color c) {
        if (c.alpha == 0 || fw <= 0 || fh <= 0) return;
        position cl = eff_clip();
        int x1 = std::max(fx, R(cl.left())),  y1 = std::max(fy, R(cl.top()));
        int x2 = std::min(fx + fw, R(cl.right())), y2 = std::min(fy + fh, R(cl.bottom()));
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++) {
                float cov = round_cov(x, y, fx, fy, fw, fh, rtl, rtr, rbr, rbl);
                if (cov > 0.f) put_px(x, y, c.red, c.green, c.blue, cov_alpha(cov, c.alpha));
            }
    }
    /* Stroke a rounded-rect outline of uniform thickness t: the outer rounded
     * rect minus the inner one (inset by t), both anti-aliased. */
    void stroke_rounded(int fx, int fy, int fw, int fh, int rtl, int rtr, int rbr, int rbl, int t, web_color c) {
        if (c.alpha == 0 || t <= 0 || fw <= 0 || fh <= 0) return;
        int itl = rtl - t > 0 ? rtl - t : 0, itr = rtr - t > 0 ? rtr - t : 0;
        int ibr = rbr - t > 0 ? rbr - t : 0, ibl = rbl - t > 0 ? rbl - t : 0;
        position cl = eff_clip();
        int x1 = std::max(fx, R(cl.left())),  y1 = std::max(fy, R(cl.top()));
        int x2 = std::min(fx + fw, R(cl.right())), y2 = std::min(fy + fh, R(cl.bottom()));
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++) {
                float cov = round_cov(x, y, fx, fy, fw, fh, rtl, rtr, rbr, rbl) *
                            (1.f - round_cov(x, y, fx + t, fy + t, fw - 2 * t, fh - 2 * t, itl, itr, ibr, ibl));
                if (cov > 0.f) put_px(x, y, c.red, c.green, c.blue, cov_alpha(cov, c.alpha));
            }
    }

    void fill_background_box(const background_layer &layer, web_color c) {
        const position &b = layer.border_box;
        const border_radiuses &br = layer.border_radius;
        if (br.top_left_x > 0 || br.top_right_x > 0 || br.bottom_right_x > 0 || br.bottom_left_x > 0)
            fill_rounded(R(b.x), R(b.y), R(b.x + b.width) - R(b.x), R(b.y + b.height) - R(b.y),
                         (int)br.top_left_x, (int)br.top_right_x,
                         (int)br.bottom_right_x, (int)br.bottom_left_x, c);
        else
            fill(b.x, b.y, b.width, b.height, c);
    }

public:
    /* Metrics for optical centering. The font's own ascender and descender are
     * each rounded up, so they add up to more than its line height and push
     * text down, and its capitals sit off the middle of the box. Instead keep
     * the font's line height, place the baseline so the middle of the
     * capitals/digits lands on the middle of the text box, and report
     * x_height as twice that middle so vertical-align: middle lines icons up
     * with digits and CJK rather than with lowercase letters. */
    uint_ptr create_font(const font_description &d, const document *, font_metrics *fm) override {
        auto *f = new ft_font();
        f->size = (int)d.size;
        FT_Set_Pixel_Sizes(g_face, 0, f->size);
        f->height = (int)(g_face->size->metrics.height >> 6);
        if (f->height <= 0) f->height = f->size;
        const glyph &cap = glyph_get('H', f->size);
        double cap_mid = cap.h ? cap.top - (cap.h - 1) / 2.0 : f->size * 0.36;
        /* rows 0..h-1 centre on (h-1)/2; when that falls between rows, sit a
         * hair high rather than low */
        f->ascent  = (int)floor((f->height - 1) / 2.0 + cap_mid);
        f->descent = f->height - f->ascent;
        if (fm) {
            fm->font_size = d.size;
            fm->ascent = f->ascent; fm->descent = f->descent;
            fm->height = f->height;
            fm->x_height = (int)lround(2 * cap_mid); fm->ch_width = f->size / 2;
            fm->draw_spaces = true;
        }
        return (uint_ptr)f;
    }
    void delete_font(uint_ptr h) override { delete (ft_font *)h; }

    pixel_t text_width(const char *text, uint_ptr h) override {
        return (pixel_t)text_advance(text, ((ft_font *)h)->size);
    }

    void draw_text(uint_ptr, const char *text, uint_ptr h, web_color color, const position &pos) override {
        auto *f = (ft_font *)h;
        int pen = R(pos.x);
        int base = R(pos.y) + f->ascent;
        position cl = eff_clip();
        for (const char *s = text; *s; ) {
            const glyph &gl = glyph_get(utf8_next(s), f->size);
            int ox = pen + gl.left, oy = base - gl.top;
            for (int r = 0; r < gl.h; r++) {
                int yy = oy + r;
                if (yy < R(cl.top()) || yy >= R(cl.bottom())) continue;
                for (int cx = 0; cx < gl.w; cx++) {
                    int xx = ox + cx;
                    if (xx < R(cl.left()) || xx >= R(cl.right())) continue;
                    int a = gl.px[(size_t)r * gl.w + cx];
                    if (a) put_px(xx, yy, color.red, color.green, color.blue, a * color.alpha / 255);
                }
            }
            pen += gl.adv;
        }
    }

    pixel_t pt_to_px(float pt) const override { return (pixel_t)(pt * 96.0f / 72.0f + 0.5f); }
    pixel_t get_default_font_size() const override { return 16; }
    const char *get_default_font_name() const override { return "sans-serif"; }

    void draw_solid_fill(uint_ptr, const background_layer &layer, const web_color &color) override {
        fill_background_box(layer, color);
    }
    /* Color of a gradient at t (0..1 along the gradient line) from its stops. */
    static web_color gradient_at(const std::vector<background_layer::color_point> &pts, float t) {
        if (pts.empty()) return web_color(0, 0, 0, 0);
        if (t <= pts.front().offset) return pts.front().color;
        if (t >= pts.back().offset) return pts.back().color;
        for (size_t i = 1; i < pts.size(); i++) {
            if (t > pts[i].offset) continue;
            const web_color &a = pts[i - 1].color, &b = pts[i].color;
            float span = pts[i].offset - pts[i - 1].offset;
            float k = span > 0 ? (t - pts[i - 1].offset) / span : 1.f;
            return web_color((byte)(a.red + (b.red - a.red) * k), (byte)(a.green + (b.green - a.green) * k),
                             (byte)(a.blue + (b.blue - a.blue) * k), (byte)(a.alpha + (b.alpha - a.alpha) * k));
        }
        return pts.back().color;
    }
    /* Opaque gradients repaint the same pixels on every refresh (the page
     * background), so the last few are kept and copied back. */
    struct grad_cache { std::string key; std::vector<uint16_t> px; };
    grad_cache m_grad[4];
    int m_grad_next = 0;

    /* Paint the layer's (rounded) box with color_of(x, y) -> t. `kind` names
     * the gradient geometry for the cache. */
    template <class F>
    void fill_gradient_box(const background_layer &l, const std::vector<background_layer::color_point> &pts,
                           const std::string &kind, F t_of) {
        const position &b = l.border_box;
        const border_radiuses &br = l.border_radius;
        int rtl = (int)br.top_left_x, rtr = (int)br.top_right_x, rbr = (int)br.bottom_right_x, rbl = (int)br.bottom_left_x;
        position cl = eff_clip();
        int bx = R(b.left()), by = R(b.top()), bw = R(b.right()) - bx, bh = R(b.bottom()) - by;
        int x1 = std::max({bx, R(cl.left()), R(l.clip_box.left())});
        int y1 = std::max({by, R(cl.top()), R(l.clip_box.top())});
        int x2 = std::min({bx + bw, R(cl.right()), R(l.clip_box.right())});
        int y2 = std::min({by + bh, R(cl.bottom()), R(l.clip_box.bottom())});
        if (x2 <= x1 || y2 <= y1) return;
        bool opaque = !(rtl || rtr || rbr || rbl);
        for (const auto &p : pts) if (p.color.alpha != 255) opaque = false;
        if (opaque) {
            char geo[96];
            snprintf(geo, sizeof geo, "|%d,%d,%d,%d|%d,%d,%d,%d|", bx, by, bw, bh, x1, y1, x2, y2);
            std::string key = kind + geo;
            for (const auto &p : pts) {
                char st[48];
                snprintf(st, sizeof st, "%g:%02x%02x%02x;", p.offset, p.color.red, p.color.green, p.color.blue);
                key += st;
            }
            grad_cache *c = nullptr;
            for (auto &e : m_grad) if (e.key == key) c = &e;
            if (!c) {
                c = &m_grad[m_grad_next];
                m_grad_next = (m_grad_next + 1) % 4;
                c->key = key;
                c->px.resize((size_t)(x2 - x1) * (y2 - y1));
                uint16_t *o = c->px.data();
                for (int y = y1; y < y2; y++)
                    for (int x = x1; x < x2; x++) {
                        web_color col = gradient_at(pts, t_of((float)x + .5f, (float)y + .5f));
                        *o++ = dither565(x, y, col.red, col.green, col.blue);
                    }
            }
            const uint16_t *src = c->px.data();
            for (int y = y1; y < y2; y++, src += x2 - x1) {
                if (y < g_clip_top) continue;
                if (g_rotate) {
                    uint16_t *dp = &g_fb[(g_h - 1 - y) * g_pitch_px + (g_w - 1 - x1)];
                    for (int i = 0; i < x2 - x1; i++) *dp-- = src[i];
                } else {
                    memcpy(&g_fb[y * g_pitch_px + x1], src, (size_t)(x2 - x1) * sizeof(uint16_t));
                }
            }
            return;
        }
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++) {
                float cov = (rtl || rtr || rbr || rbl) ? round_cov(x, y, bx, by, bw, bh, rtl, rtr, rbr, rbl) : 1.f;
                if (cov <= 0.f) continue;
                web_color c = gradient_at(pts, t_of((float)x + .5f, (float)y + .5f));
                put_px_dither(x, y, c.red, c.green, c.blue, cov_alpha(cov, c.alpha));
            }
    }
    void draw_linear_gradient(uint_ptr, const background_layer &l, const background_layer::linear_gradient &g) override {
        float vx = g.end.x - g.start.x, vy = g.end.y - g.start.y, len2 = vx * vx + vy * vy;
        char kind[96];
        snprintf(kind, sizeof kind, "L%g,%g,%g,%g", g.start.x, g.start.y, g.end.x, g.end.y);
        fill_gradient_box(l, g.color_points, kind, [&](float x, float y) {
            return len2 > 0 ? ((x - g.start.x) * vx + (y - g.start.y) * vy) / len2 : 0.f;
        });
    }
    void draw_radial_gradient(uint_ptr, const background_layer &l, const background_layer::radial_gradient &g) override {
        float rx = g.radius.x > 0 ? g.radius.x : 1.f, ry = g.radius.y > 0 ? g.radius.y : 1.f;
        char kind[96];
        snprintf(kind, sizeof kind, "R%g,%g,%g,%g", g.position.x, g.position.y, rx, ry);
        fill_gradient_box(l, g.color_points, kind, [&](float x, float y) {
            float dx = (x - g.position.x) / rx, dy = (y - g.position.y) / ry;
            return std::sqrt(dx * dx + dy * dy);
        });
    }
    void draw_conic_gradient(uint_ptr, const background_layer &l, const background_layer::conic_gradient &) override {
        fill_background_box(l, web_color(45, 46, 50));
    }

    void draw_borders(uint_ptr, const borders &b, const position &p, bool) override {
        /* Rounded outline for the common case: all four sides equal width, same
         * color, with a corner radius (cards, the battery frame, ...). */
        int rtl = (int)b.radius.top_left_x, rtr = (int)b.radius.top_right_x;
        int rbr = (int)b.radius.bottom_right_x, rbl = (int)b.radius.bottom_left_x;
        const web_color &tc = b.top.color;
        bool same_w = b.top.width == b.left.width && b.top.width == b.right.width && b.top.width == b.bottom.width;
        bool same_c = tc.red == b.left.color.red && tc.green == b.left.color.green && tc.blue == b.left.color.blue && tc.alpha == b.left.color.alpha
                   && tc.red == b.right.color.red && tc.green == b.right.color.green && tc.blue == b.right.color.blue && tc.alpha == b.right.color.alpha
                   && tc.red == b.bottom.color.red && tc.green == b.bottom.color.green && tc.blue == b.bottom.color.blue && tc.alpha == b.bottom.color.alpha;
        bool all_solid = b.top.style != border_style_none && b.left.style != border_style_none
                      && b.right.style != border_style_none && b.bottom.style != border_style_none;
        if ((rtl || rtr || rbr || rbl) && same_w && same_c && all_solid && b.top.width > 0) {
            stroke_rounded(R(p.x), R(p.y), R(p.x + p.width) - R(p.x), R(p.y + p.height) - R(p.y),
                           rtl, rtr, rbr, rbl, (int)b.top.width, tc);
            return;
        }
        if (b.top.width > 0    && b.top.style    != border_style_none) fill(p.x, p.y, p.width, b.top.width, b.top.color);
        if (b.bottom.width > 0 && b.bottom.style != border_style_none) fill(p.x, p.bottom() - b.bottom.width, p.width, b.bottom.width, b.bottom.color);
        if (b.left.width > 0   && b.left.style   != border_style_none) fill(p.x, p.y, b.left.width, p.height, b.left.color);
        if (b.right.width > 0  && b.right.style  != border_style_none) fill(p.right() - b.right.width, p.y, b.right.width, p.height, b.right.color);
    }

    void draw_list_marker(uint_ptr, const list_marker &) override {}

    /* images: not supported (the example uses none) */
    void load_image(const char *, const char *, bool) override {}
    void get_image_size(const char *, const char *, size &sz) override { sz.width = sz.height = 0; }
    void draw_image(uint_ptr, const background_layer &, const std::string &, const std::string &) override {}

    void set_clip(const position &pos, const border_radiuses &) override { m_clip.push_back(pos); }
    void del_clip() override { if (!m_clip.empty()) m_clip.pop_back(); }
    void reset_state() { m_clip.clear(); }

    void get_viewport(position &v) const override { v = position(0, 0, (pixel_t)g_w, (pixel_t)g_h); }
    void get_media_features(media_features &m) const override {
        m.type = media_type_screen;
        m.width = m.device_width = (pixel_t)g_w;
        m.height = m.device_height = (pixel_t)g_h;
        m.color = 8; m.resolution = 96;
    }
    void get_language(string &, string &) const override {}

    /* trivial stubs */
    void set_caption(const char *) override {}
    void set_base_url(const char *) override {}
    void link(const std::shared_ptr<document> &, const element::ptr &) override {}
    void on_anchor_click(const char *url, const element::ptr &) override { g_clicked = url ? url : ""; }
    void on_mouse_event(const element::ptr &, mouse_event) override {}
    void set_cursor(const char *) override {}
    void transform_text(string &, text_transform) override {}
    void import_css(string &text, const string &url, string &) override {
        std::string path = g_ui_dir + "/" + url;
        css_get_cached(text, path);
    }
    element::ptr create_element(const char *, const string_map &, const std::shared_ptr<document> &) override { return nullptr; }
};

/* ---- C interface for the (C) main harness ---- */
static fb_container   *g_container;
static document::ptr   g_doc;
static int             g_scroll_y;   /* vertical scroll offset (logical px) */
static int             g_doc_overlay; /* overlay docs are already in screen coords */

extern "C" void html_view_init(uint16_t *fb, int w, int h, int pitch_px, int rotate, const char *font_path)
{
    g_fb = fb; g_w = w; g_h = h; g_pitch_px = pitch_px; g_rotate = rotate;
    FT_Init_FreeType(&g_ft);
    if (FT_New_Face(g_ft, font_path, 0, &g_face))
        fprintf(stderr, "html_view: cannot load font %s\n", font_path);
    g_container = new fb_container();
}

/* Draw into the framebuffer rotated by 180 degrees (upside-down panel) or not. */
extern "C" void html_view_set_rotate(int rotate)
{
    g_rotate = rotate;
}

/* Parse + lay out + paint an HTML string into the framebuffer. Returns height. */
extern "C" int html_view_render_html(const char *html)
{
    if (!html || !g_container) return -1;
    g_container->reset_state();
    g_doc.reset();
    document::ptr doc = document::createFromString(html, g_container);
    if (!doc) return -1;

    g_doc = doc;
    g_doc_overlay = 0;
    g_doc->render((pixel_t)g_w);
    for (int i = 0; i < g_pitch_px * g_h; i++) g_fb[i] = 0;
    position clip(0, 0, (pixel_t)g_w, (pixel_t)g_h);
    g_doc->draw((uint_ptr)0, 0, -g_scroll_y, &clip);   /* shift up by scroll */
    element::ptr root = g_doc->root();                 /* full content height */
    return root ? (int)root->get_placement().height : g_h;
}

/* Render an overlay on top of the current framebuffer (no clear): the body must
 * be transparent so only its boxes paint. g_doc is set to the overlay so taps
 * hit it. Used for modal dialogs. */
extern "C" int html_view_render_overlay(const char *html)
{
    if (!html || !g_container) return -1;
    g_container->reset_state();
    g_doc.reset();
    document::ptr doc = document::createFromString(html, g_container);
    if (!doc) return -1;

    g_doc = doc;
    g_doc_overlay = 1;
    g_doc->render((pixel_t)g_w);
    position clip(0, 0, (pixel_t)g_w, (pixel_t)g_h);
    g_doc->draw((uint_ptr)0, 0, 0, &clip);
    return 0;
}

/* Render into a plain logical W*H RGB565 buffer (no rotation) for animations. */
extern "C" int html_view_render_to(uint16_t *buf, const char *html)
{
    uint16_t *sfb = g_fb; int sw = g_w, sh = g_h, sp = g_pitch_px, sr = g_rotate, ssc = g_scroll_y;
    g_fb = buf; g_w = sw; g_h = sh; g_pitch_px = sw; g_rotate = 0; g_scroll_y = 0;
    int hh = html_view_render_html(html);
    g_fb = sfb; g_w = sw; g_h = sh; g_pitch_px = sp; g_rotate = sr; g_scroll_y = ssc;
    return hh;
}

extern "C" int html_view_render_to_scroll(uint16_t *buf, const char *html, int scroll)
{
    uint16_t *sfb = g_fb; int sw = g_w, sh = g_h, sp = g_pitch_px, sr = g_rotate, ssc = g_scroll_y;
    g_fb = buf; g_w = sw; g_h = sh; g_pitch_px = sw; g_rotate = 0; g_scroll_y = scroll < 0 ? 0 : scroll;
    int hh = html_view_render_html(html);
    g_fb = sfb; g_w = sw; g_h = sh; g_pitch_px = sp; g_rotate = sr; g_scroll_y = ssc;
    return hh;
}

static uint16_t *g_saved_fb = nullptr;
static int g_saved_w = 0, g_saved_h = 0, g_saved_pitch = 0, g_saved_rotate = 0, g_saved_scroll = 0;

extern "C" void html_view_target_begin(uint16_t *buf, int scroll)
{
    if (g_saved_fb) return;
    g_saved_fb = g_fb; g_saved_w = g_w; g_saved_h = g_h;
    g_saved_pitch = g_pitch_px; g_saved_rotate = g_rotate; g_saved_scroll = g_scroll_y;
    g_fb = buf; g_pitch_px = g_w; g_rotate = 0; g_scroll_y = scroll < 0 ? 0 : scroll;
}

extern "C" void html_view_target_end(void)
{
    if (!g_saved_fb) return;
    g_fb = g_saved_fb; g_w = g_saved_w; g_h = g_saved_h;
    g_pitch_px = g_saved_pitch; g_rotate = g_saved_rotate; g_scroll_y = g_saved_scroll;
    g_saved_fb = nullptr;
}

extern "C" int html_view_render_to_size(uint16_t *buf, int w, int h, const char *html)
{
    uint16_t *sfb = g_fb; int sw = g_w, sh = g_h, sp = g_pitch_px, sr = g_rotate, ssc = g_scroll_y;
    g_fb = buf; g_w = w; g_h = h; g_pitch_px = w; g_rotate = 0; g_scroll_y = 0;
    int hh = html_view_render_html(html);
    g_fb = sfb; g_w = sw; g_h = sh; g_pitch_px = sp; g_rotate = sr; g_scroll_y = ssc;
    return hh;
}

extern "C" void html_view_set_uidir(const char *d) { g_ui_dir = d; }
extern "C" void html_view_set_scroll(int y) { g_scroll_y = y < 0 ? 0 : y; }
extern "C" void html_view_set_clip_top(int y) { g_clip_top = y < 0 ? 0 : y; }

/* Render the full page (no rotation, no clip to 480) into a tall W*bufh logical
 * buffer, for smooth windowed scrolling. Returns content height. */
extern "C" int html_view_render_tall(uint16_t *buf, const char *html, int bufh)
{
    uint16_t *sfb = g_fb; int sh = g_h, sp = g_pitch_px, sr = g_rotate, ssc = g_scroll_y;
    g_fb = buf; g_h = bufh; g_pitch_px = g_w; g_rotate = 0; g_scroll_y = 0;
    int hh = html_view_render_html(html);
    g_fb = sfb; g_h = sh; g_pitch_px = sp; g_rotate = sr; g_scroll_y = ssc;
    return hh;
}

/* Fill a rect directly (used for the scrollbar overlay). */
extern "C" void html_view_fill_rect(int x, int y, int w, int h, int r, int g, int b, int a)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put_px(xx, yy, r, g, b, a);
}

/* Fill an arbitrary (possibly concave) polygon, even-odd rule. Used by the
 * host to draw shapes litehtml/CSS can't, e.g. the battery charging bolt.
 * Vertex coordinates name pixel centres, as with the old scanline fill; each
 * pixel is sampled 4x4 so slanted edges are anti-aliased. */
extern "C" void html_view_fill_poly(const int *xs, const int *ys, int n, int r, int g, int b, int a)
{
    if (n < 3) return;
    int xmin = xs[0], xmax = xs[0], ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; i++) {
        xmin = std::min(xmin, xs[i]); xmax = std::max(xmax, xs[i]);
        ymin = std::min(ymin, ys[i]); ymax = std::max(ymax, ys[i]);
    }
    for (int y = ymin; y <= ymax; y++)
        for (int x = xmin; x <= xmax; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float px = (float)x - .375f + sx * .25f, py = (float)y - .375f + sy * .25f;
                    bool in = false;
                    for (int i = 0, j = n - 1; i < n; j = i++) {
                        float yi = (float)ys[i], yj = (float)ys[j];
                        if ((yi > py) != (yj > py) &&
                            px < (float)xs[j] + (py - yj) * (float)(xs[i] - xs[j]) / (yi - yj))
                            in = !in;
                    }
                    hits += in;
                }
            if (hits) put_px(x, y, r, g, b, (hits * a + 8) / 16);
        }
}

static inline float clampf01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

/* Anti-aliased filled circle (coverage from the signed distance to the edge). */
extern "C" void html_view_fill_circle(float cx, float cy, float rad, int r, int g, int b, int a)
{
    for (int y = (int)std::floor(cy - rad - 1); y <= (int)std::ceil(cy + rad + 1); y++)
        for (int x = (int)std::floor(cx - rad - 1); x <= (int)std::ceil(cx + rad + 1); x++) {
            float dx = (float)x + .5f - cx, dy = (float)y + .5f - cy;
            float cov = clampf01(rad - std::sqrt(dx * dx + dy * dy) + .5f);
            if (cov > 0.f) put_px(x, y, r, g, b, (int)(cov * (float)a + .5f));
        }
}

/* Anti-aliased line segment of thickness th with round ends. */
extern "C" void html_view_line(float x0, float y0, float x1, float y1, float th, int r, int g, int b, int a)
{
    float half = th * .5f, vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    for (int y = (int)std::floor(std::min(y0, y1) - half - 1); y <= (int)std::ceil(std::max(y0, y1) + half + 1); y++)
        for (int x = (int)std::floor(std::min(x0, x1) - half - 1); x <= (int)std::ceil(std::max(x0, x1) + half + 1); x++) {
            float px = (float)x + .5f - x0, py = (float)y + .5f - y0;
            float t = len2 > 0 ? clampf01((px * vx + py * vy) / len2) : 0.f;
            float dx = px - t * vx, dy = py - t * vy;
            float cov = clampf01(half - std::sqrt(dx * dx + dy * dy) + .5f);
            if (cov > 0.f) put_px(x, y, r, g, b, (int)(cov * (float)a + .5f));
        }
}

/* Anti-aliased arc of thickness th around radius rad with round caps. Angles in
 * degrees, 0 = 12 o'clock, clockwise, a0 < a1 (a span of 360 draws a ring). */
extern "C" void html_view_arc(float cx, float cy, float rad, float th, float a0, float a1,
                              int r, int g, int b, int a)
{
    const float pi = 3.14159265f;
    float half = th * .5f, r0 = rad - half, r1 = rad + half;
    int ring = a1 - a0 >= 359.9f;

    if (a1 <= a0) return;
    for (int y = (int)std::floor(cy - r1 - 1); y <= (int)std::ceil(cy + r1 + 1); y++)
        for (int x = (int)std::floor(cx - r1 - 1); x <= (int)std::ceil(cx + r1 + 1); x++) {
            float dx = (float)x + .5f - cx, dy = (float)y + .5f - cy, d2 = dx * dx + dy * dy;
            if (d2 > (r1 + 1) * (r1 + 1) || d2 < (r0 - 1) * (r0 - 1)) continue;
            float d = std::sqrt(d2);
            float cov = clampf01(d - r0 + .5f) * clampf01(r1 - d + .5f);
            if (!ring) {
                float deg = std::atan2(dx, -dy) * 180.f / pi;
                while (deg < a0) deg += 360.f;
                while (deg >= a0 + 360.f) deg -= 360.f;
                if (deg > a1) continue;
            }
            if (cov > 0.f) put_px(x, y, r, g, b, (int)(cov * (float)a + .5f));
        }
    if (!ring) {
        float ra0 = a0 * pi / 180.f, ra1 = a1 * pi / 180.f;
        html_view_fill_circle(cx + rad * std::sin(ra0), cy - rad * std::cos(ra0), half, r, g, b, a);
        html_view_fill_circle(cx + rad * std::sin(ra1), cy - rad * std::cos(ra1), half, r, g, b, a);
    }
}

/* Anti-aliased rounded rectangle: filled when th <= 0, else an outline of
 * thickness th inside the edge (coverage from the signed distance). */
extern "C" void html_view_round_rect_aa(float x, float y, float w, float h, float rad, float th,
                                        int r, int g, int b, int a)
{
    if (w <= 0.f || h <= 0.f) return;
    float hw = w * .5f, hh = h * .5f, cx = x + hw, cy = y + hh;
    if (rad > hw) rad = hw;
    if (rad > hh) rad = hh;
    for (int py = (int)std::floor(y) - 1; py <= (int)std::ceil(y + h); py++)
        for (int px = (int)std::floor(x) - 1; px <= (int)std::ceil(x + w); px++) {
            float qx = std::fabs((float)px + .5f - cx) - (hw - rad), qy = std::fabs((float)py + .5f - cy) - (hh - rad);
            float ox = qx > 0.f ? qx : 0.f, oy = qy > 0.f ? qy : 0.f;
            float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - rad;
            float cov = clampf01(.5f - d);
            if (th > 0.f) cov *= clampf01(.5f + d + th);
            if (cov > 0.f) put_px(px, py, r, g, b, (int)(cov * (float)a + .5f));
        }
}

/* Filled rounded rectangle directly (host overlays like the lock icon). */
extern "C" void html_view_fill_round_rect(int x, int y, int w, int h, int rad, int r, int g, int b, int a)
{
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
        {
            float cov = round_cov(xx, yy, x, y, w, h, rad, rad, rad, rad);
            if (cov > 0.f) put_px(xx, yy, r, g, b, cov_alpha(cov, a));
        }
}

extern "C" int html_view_text_width_px(const char *text, int size)
{
    if (!g_face || !text) return 0;
    return text_advance(text, size);
}

extern "C" void html_view_text_bounds_px(const char *text, int size,
                                         int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = 0;
    if (y0) *y0 = 0;
    if (x1) *x1 = 0;
    if (y1) *y1 = 0;
    if (!g_face || !text) return;
    int ascent = font_ascent(size);
    int pen = 0, minx = 0, miny = 0, maxx = 0, maxy = 0, seen = 0;
    for (const char *s = text; *s; ) {
        const glyph &gl = glyph_get(utf8_next(s), size);
        int gx0 = pen + gl.left;
        int gy0 = ascent - gl.top;
        int gx1 = gx0 + gl.w;
        int gy1 = gy0 + gl.h;
        if (!seen) {
            minx = gx0; miny = gy0; maxx = gx1; maxy = gy1; seen = 1;
        } else {
            if (gx0 < minx) minx = gx0;
            if (gy0 < miny) miny = gy0;
            if (gx1 > maxx) maxx = gx1;
            if (gy1 > maxy) maxy = gy1;
        }
        pen += gl.adv;
    }
    if (!seen) return;
    if (x0) *x0 = minx;
    if (y0) *y0 = miny;
    if (x1) *x1 = maxx;
    if (y1) *y1 = maxy;
}

extern "C" void html_view_draw_text_px(int x, int y, const char *text, int size, int bold,
                                       int r, int g, int b, int a)
{
    if (!g_face || !text) return;
    int pen = x, base = y + font_ascent(size);
    for (const char *s = text; *s; ) {
        const glyph &gl = glyph_get(utf8_next(s), size);
        for (int pass = 0; pass < (bold ? 2 : 1); pass++) {
            int ox = pen + gl.left + pass, oy = base - gl.top;
            for (int row = 0; row < gl.h; row++)
                for (int col = 0; col < gl.w; col++) {
                    int aa = gl.px[(size_t)row * gl.w + col];
                    if (aa) put_px(ox + col, oy + row, r, g, b, aa * a / 255);
                }
        }
        pen += gl.adv;
    }
}

extern "C" void html_view_draw_text_contrast_px(int x, int y, const char *text, int size, int bold,
                                                int dr, int dg, int db, int lr, int lg, int lb, int a)
{
    if (!g_face || !text) return;
    int pen = x, base = y + font_ascent(size);
    for (const char *s = text; *s; ) {
        const glyph &gl = glyph_get(utf8_next(s), size);
        for (int pass = 0; pass < (bold ? 2 : 1); pass++) {
            int ox = pen + gl.left + pass, oy = base - gl.top;
            for (int row = 0; row < gl.h; row++)
                for (int col = 0; col < gl.w; col++) {
                    int aa = gl.px[(size_t)row * gl.w + col];
                    if (!aa) continue;
                    uint16_t bg = get_px565(ox + col, oy + row);
                    int br = ((bg >> 11) & 0x1F) * 255 / 31;
                    int bgc = ((bg >> 5) & 0x3F) * 255 / 63;
                    int bb = (bg & 0x1F) * 255 / 31;
                    int luma = (br * 299 + bgc * 587 + bb * 114) / 1000;
                    int r = luma > 116 ? dr : lr;
                    int g = luma > 116 ? dg : lg;
                    int b = luma > 116 ? db : lb;
                    put_px(ox + col, oy + row, r, g, b, aa * a / 255);
                }
        }
        pen += gl.adv;
    }
}

/* Hit-test a tap; returns the clicked anchor href ("" if none). */
extern "C" const char *html_view_click(float x, float y)
{
    g_clicked.clear();
    if (g_doc) {
        position::vector rb;
        float yy = y + (g_doc_overlay ? 0 : g_scroll_y);
        g_doc->on_lbutton_down(x, yy, x, yy, rb);
        g_doc->on_lbutton_up(x, yy, x, yy, rb);
    }
    return g_clicked.c_str();
}

/* ---- custom chart drawing: native polylines into a litehtml placeholder ----
 * Flow: render the page (which lays out an empty <div id="chart-..">), query the
 * div's box with html_view_rect(), then draw series into it with html_view_polyline().
 * Coords are logical (pre-rotation); put_px maps to the panel. */

/* Bresenham line, thick px wide, clipped to [rx,rx+rw) x [ry,ry+rh). */
static void chart_line(int x0, int y0, int x1, int y1,
                       int rx, int ry, int rw, int rh, int r, int g, int b, int thick)
{
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int ty = 0; ty < thick; ty++)
            for (int tx = 0; tx < thick; tx++) {
                int px = x0 + tx, py = y0 + ty;
                if (px >= rx && px < rx + rw && py >= ry && py < ry + rh)
                    put_px(px, py, r, g, b, 255);
            }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Find an element by CSS selector and return its laid-out box. 1 if found. */
extern "C" int html_view_rect(const char *sel, int *x, int *y, int *w, int *h)
{
    if (!g_doc) return 0;
    element::ptr root = g_doc->root();
    if (!root) return 0;
    element::ptr el = root->select_one(sel);
    if (!el) return 0;
    position p = el->get_placement();
    int sy = g_doc_overlay ? 0 : g_scroll_y;
    *x = R(p.x); *y = R(p.y) - sy; *w = R(p.x + p.width) - *x; *h = R(p.y + p.height) - R(p.y);
    return (*w > 1 && *h > 1);
}

/* Draw a value series as a polyline inside the rect. vals normalized by
 * [vmin,vmax]; top = vmax. fill_a>0 fills the area under the line at that alpha. */
extern "C" void html_view_polyline(int x, int y, int w, int h,
                                   const int *vals, int n, int vmin, int vmax,
                                   int r, int g, int b, int thick, int fill_a)
{
    if (n <= 0 || w <= 1 || h <= 1) return;
    if (vmax <= vmin) vmax = vmin + 1;

    if (fill_a > 0) {
        for (int col = 0; col < w; col++) {
            double t = (n > 1) ? (double)col / (w - 1) * (n - 1) : 0.0;
            int i0 = (int)t; double fr = t - i0;
            int v0 = vals[i0], v1 = (i0 + 1 < n) ? vals[i0 + 1] : v0;
            double v = v0 + (v1 - v0) * fr;
            if (v < vmin) v = vmin; if (v > vmax) v = vmax;
            int py = y + (h - 1) - (int)((v - vmin) * (h - 1) / (vmax - vmin));
            for (int yy = py; yy < y + h; yy++)
                if (((col + yy) & 1) == 0) put_px(x + col, yy, r, g, b, fill_a);
        }
    }

    int px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        int v = vals[i]; if (v < vmin) v = vmin; if (v > vmax) v = vmax;
        int cx = x + (n > 1 ? i * (w - 1) / (n - 1) : 0);
        int cy = y + (h - 1) - (int)((long)(v - vmin) * (h - 1) / (vmax - vmin));
        if (i > 0) chart_line(px, py, cx, cy, x, y, w, h, r, g, b, thick);
        px = cx; py = cy;
    }
}
