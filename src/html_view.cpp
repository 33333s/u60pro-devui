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
#include <sys/stat.h>

using namespace litehtml;

/* ---- target framebuffer (shared with drm_disp, 180° rotated panel) ---- */
static uint16_t *g_fb;
static int g_w, g_h, g_pitch_px, g_rotate = 1;

static inline void put_px(int x, int y, int r, int g, int b, int a)
{
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

static inline uint16_t get_px565(int x, int y)
{
    if (x < 0 || y < 0 || x >= g_w || y >= g_h || !g_fb) return 0;
    int dx = g_rotate ? (g_w - 1 - x) : x;
    int dy = g_rotate ? (g_h - 1 - y) : y;
    return g_fb[dy * g_pitch_px + dx];
}

/* Is (x,y) inside a rounded rectangle? Used by both rounded fill and rounded
 * border stroking (litehtml hands us radii but leaves the drawing to us). */
static bool pt_in_round(int x, int y, int fx, int fy, int fw, int fh,
                        int rtl, int rtr, int rbr, int rbl)
{
    if (x < fx || y < fy || x >= fx + fw || y >= fy + fh) return false;
    int r = 0, cx = 0, cy = 0;
    if      (x < fx + rtl       && y < fy + rtl)       { r = rtl; cx = fx + rtl;          cy = fy + rtl; }
    else if (x >= fx + fw - rtr && y < fy + rtr)       { r = rtr; cx = fx + fw - 1 - rtr; cy = fy + rtr; }
    else if (x >= fx + fw - rbr && y >= fy + fh - rbr) { r = rbr; cx = fx + fw - 1 - rbr; cy = fy + fh - 1 - rbr; }
    else if (x < fx + rbl       && y >= fy + fh - rbl) { r = rbl; cx = fx + rbl;          cy = fy + fh - 1 - rbl; }
    if (r > 0) { int dx = x - cx, dy = y - cy; if (dx * dx + dy * dy > r * r) return false; }
    return true;
}

/* ---- FreeType ---- */
static FT_Library g_ft;
static FT_Face    g_face;

struct ft_font { int size; int ascent, descent, height; };

/* UI base dir (for <link> CSS / images) and last-clicked anchor href. */
static std::string g_ui_dir = "/data/ui";
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
        int x1 = std::max((int)fx, (int)cl.left()),  y1 = std::max((int)fy, (int)cl.top());
        int x2 = std::min((int)(fx + fw), (int)cl.right()), y2 = std::min((int)(fy + fh), (int)cl.bottom());
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++)
                put_px(x, y, c.red, c.green, c.blue, c.alpha);
    }
    /* Filled rectangle with rounded corners. litehtml only hands us the radii;
     * it's up to the container to honor them, so a pixel inside one of the four
     * corner squares but outside its quarter-circle arc is skipped. */
    void fill_rounded(int fx, int fy, int fw, int fh, int rtl, int rtr, int rbr, int rbl, web_color c) {
        if (c.alpha == 0 || fw <= 0 || fh <= 0) return;
        int hw = fw / 2, hh = fh / 2;
        if (rtl > hw) rtl = hw; if (rtl > hh) rtl = hh;
        if (rtr > hw) rtr = hw; if (rtr > hh) rtr = hh;
        if (rbr > hw) rbr = hw; if (rbr > hh) rbr = hh;
        if (rbl > hw) rbl = hw; if (rbl > hh) rbl = hh;
        position cl = eff_clip();
        int x1 = std::max(fx, (int)cl.left()),  y1 = std::max(fy, (int)cl.top());
        int x2 = std::min(fx + fw, (int)cl.right()), y2 = std::min(fy + fh, (int)cl.bottom());
        for (int y = y1; y < y2; y++) {
            for (int x = x1; x < x2; x++) {
                int r = 0, cx = 0, cy = 0;
                if      (x < fx + rtl       && y < fy + rtl)       { r = rtl; cx = fx + rtl;          cy = fy + rtl; }
                else if (x >= fx + fw - rtr && y < fy + rtr)       { r = rtr; cx = fx + fw - 1 - rtr; cy = fy + rtr; }
                else if (x >= fx + fw - rbr && y >= fy + fh - rbr) { r = rbr; cx = fx + fw - 1 - rbr; cy = fy + fh - 1 - rbr; }
                else if (x < fx + rbl       && y >= fy + fh - rbl) { r = rbl; cx = fx + rbl;          cy = fy + fh - 1 - rbl; }
                if (r > 0) { int dx = x - cx, dy = y - cy; if (dx * dx + dy * dy > r * r) continue; }
                put_px(x, y, c.red, c.green, c.blue, c.alpha);
            }
        }
    }
    /* Stroke a rounded-rect outline of uniform thickness t: a pixel inside the
     * outer rounded rect but outside the inner one (inset by t). */
    void stroke_rounded(int fx, int fy, int fw, int fh, int rtl, int rtr, int rbr, int rbl, int t, web_color c) {
        if (c.alpha == 0 || t <= 0 || fw <= 0 || fh <= 0) return;
        int hw = fw / 2, hh = fh / 2;
        if (rtl > hw) rtl = hw; if (rtl > hh) rtl = hh;
        if (rtr > hw) rtr = hw; if (rtr > hh) rtr = hh;
        if (rbr > hw) rbr = hw; if (rbr > hh) rbr = hh;
        if (rbl > hw) rbl = hw; if (rbl > hh) rbl = hh;
        int itl = rtl - t > 0 ? rtl - t : 0, itr = rtr - t > 0 ? rtr - t : 0;
        int ibr = rbr - t > 0 ? rbr - t : 0, ibl = rbl - t > 0 ? rbl - t : 0;
        position cl = eff_clip();
        int x1 = std::max(fx, (int)cl.left()),  y1 = std::max(fy, (int)cl.top());
        int x2 = std::min(fx + fw, (int)cl.right()), y2 = std::min(fy + fh, (int)cl.bottom());
        for (int y = y1; y < y2; y++)
            for (int x = x1; x < x2; x++)
                if (pt_in_round(x, y, fx, fy, fw, fh, rtl, rtr, rbr, rbl) &&
                    !pt_in_round(x, y, fx + t, fy + t, fw - 2 * t, fh - 2 * t, itl, itr, ibr, ibl))
                    put_px(x, y, c.red, c.green, c.blue, c.alpha);
    }

public:
    uint_ptr create_font(const font_description &d, const document *, font_metrics *fm) override {
        auto *f = new ft_font();
        f->size = (int)d.size;
        FT_Set_Pixel_Sizes(g_face, 0, f->size);
        f->ascent  = g_face->size->metrics.ascender >> 6;
        f->descent = -(g_face->size->metrics.descender >> 6);
        f->height  = g_face->size->metrics.height >> 6;
        if (fm) {
            fm->font_size = d.size;
            fm->ascent = f->ascent; fm->descent = f->descent;
            fm->height = f->height ? f->height : f->size;
            fm->x_height = f->size / 2; fm->ch_width = f->size / 2;
            fm->draw_spaces = true;
        }
        return (uint_ptr)f;
    }
    void delete_font(uint_ptr h) override { delete (ft_font *)h; }

    pixel_t text_width(const char *text, uint_ptr h) override {
        auto *f = (ft_font *)h;
        FT_Set_Pixel_Sizes(g_face, 0, f->size);
        pixel_t w = 0;
        for (const char *s = text; *s; ) {
            unsigned cp = utf8_next(s);
            if (FT_Load_Char(g_face, cp, FT_LOAD_DEFAULT)) continue;
            w += g_face->glyph->advance.x >> 6;
        }
        return w;
    }

    void draw_text(uint_ptr, const char *text, uint_ptr h, web_color color, const position &pos) override {
        auto *f = (ft_font *)h;
        FT_Set_Pixel_Sizes(g_face, 0, f->size);
        int pen = (int)pos.x;
        int base = (int)pos.y + f->ascent;
        position cl = eff_clip();
        for (const char *s = text; *s; ) {
            unsigned cp = utf8_next(s);
            if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER)) continue;
            FT_GlyphSlot gl = g_face->glyph;
            FT_Bitmap &bm = gl->bitmap;
            int ox = pen + gl->bitmap_left, oy = base - gl->bitmap_top;
            for (int r = 0; r < (int)bm.rows; r++) {
                int yy = oy + r;
                if (yy < (int)cl.top() || yy >= (int)cl.bottom()) continue;
                for (int cx = 0; cx < (int)bm.width; cx++) {
                    int xx = ox + cx;
                    if (xx < (int)cl.left() || xx >= (int)cl.right()) continue;
                    int a = bm.buffer[r * bm.pitch + cx];
                    if (a) put_px(xx, yy, color.red, color.green, color.blue, a * color.alpha / 255);
                }
            }
            pen += gl->advance.x >> 6;
        }
    }

    pixel_t pt_to_px(float pt) const override { return (pixel_t)(pt * 96.0f / 72.0f + 0.5f); }
    pixel_t get_default_font_size() const override { return 16; }
    const char *get_default_font_name() const override { return "sans-serif"; }

    void draw_solid_fill(uint_ptr, const background_layer &layer, const web_color &color) override {
        const position &b = layer.border_box;
        const border_radiuses &br = layer.border_radius;
        if (br.top_left_x > 0 || br.top_right_x > 0 || br.bottom_right_x > 0 || br.bottom_left_x > 0)
            fill_rounded(b.x, b.y, b.width, b.height,
                         (int)br.top_left_x, (int)br.top_right_x, (int)br.bottom_right_x, (int)br.bottom_left_x, color);
        else
            fill(b.x, b.y, b.width, b.height, color);
    }
    /* Approximate gradients with a representative flat dark color. */
    void draw_linear_gradient(uint_ptr, const background_layer &l, const background_layer::linear_gradient &) override {
        fill(l.border_box.x, l.border_box.y, l.border_box.width, l.border_box.height, web_color(45, 46, 50));
    }
    void draw_radial_gradient(uint_ptr, const background_layer &l, const background_layer::radial_gradient &) override {
        fill(l.border_box.x, l.border_box.y, l.border_box.width, l.border_box.height, web_color(45, 46, 50));
    }
    void draw_conic_gradient(uint_ptr, const background_layer &l, const background_layer::conic_gradient &) override {
        fill(l.border_box.x, l.border_box.y, l.border_box.width, l.border_box.height, web_color(45, 46, 50));
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
            stroke_rounded(p.x, p.y, p.width, p.height, rtl, rtr, rbr, rbl, b.top.width, tc);
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

extern "C" void html_view_init(uint16_t *fb, int w, int h, int pitch_px, int rotate, const char *font_path)
{
    g_fb = fb; g_w = w; g_h = h; g_pitch_px = pitch_px; g_rotate = rotate;
    FT_Init_FreeType(&g_ft);
    if (FT_New_Face(g_ft, font_path, 0, &g_face))
        fprintf(stderr, "html_view: cannot load font %s\n", font_path);
    g_container = new fb_container();
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

/* Fill an arbitrary (possibly concave) polygon via scanline parity. Used by the
 * host to draw shapes litehtml/CSS can't — e.g. the battery charging bolt. */
extern "C" void html_view_fill_poly(const int *xs, const int *ys, int n, int r, int g, int b, int a)
{
    if (n < 3) return;
    int ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; i++) { if (ys[i] < ymin) ymin = ys[i]; if (ys[i] > ymax) ymax = ys[i]; }
    for (int y = ymin; y <= ymax; y++) {
        int xi[16], cnt = 0;
        for (int i = 0; i < n && cnt < 16; i++) {
            int j = (i + 1) % n;
            int y0 = ys[i], y1 = ys[j], x0 = xs[i], x1 = xs[j];
            if (y0 == y1) continue;
            if ((y >= y0 && y < y1) || (y >= y1 && y < y0))
                xi[cnt++] = x0 + (int)((long)(y - y0) * (x1 - x0) / (y1 - y0));
        }
        for (int i = 0; i < cnt; i++)            /* sort intersections ascending */
            for (int k = i + 1; k < cnt; k++)
                if (xi[k] < xi[i]) { int t = xi[i]; xi[i] = xi[k]; xi[k] = t; }
        for (int i = 0; i + 1 < cnt; i += 2)
            for (int x = xi[i]; x <= xi[i + 1]; x++)
                put_px(x, y, r, g, b, a);
    }
}

/* Filled rounded rectangle directly (host overlays like the lock icon). */
extern "C" void html_view_fill_round_rect(int x, int y, int w, int h, int rad, int r, int g, int b, int a)
{
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (pt_in_round(xx, yy, x, y, w, h, rad, rad, rad, rad))
                put_px(xx, yy, r, g, b, a);
}

extern "C" int html_view_text_width_px(const char *text, int size)
{
    if (!g_face || !text) return 0;
    FT_Set_Pixel_Sizes(g_face, 0, size);
    int w = 0;
    for (const char *s = text; *s; ) {
        unsigned cp = utf8_next(s);
        if (FT_Load_Char(g_face, cp, FT_LOAD_DEFAULT)) continue;
        w += g_face->glyph->advance.x >> 6;
    }
    return w;
}

extern "C" void html_view_text_bounds_px(const char *text, int size,
                                         int *x0, int *y0, int *x1, int *y1)
{
    if (x0) *x0 = 0;
    if (y0) *y0 = 0;
    if (x1) *x1 = 0;
    if (y1) *y1 = 0;
    if (!g_face || !text) return;
    FT_Set_Pixel_Sizes(g_face, 0, size);
    int ascent = g_face->size->metrics.ascender >> 6;
    int pen = 0, minx = 0, miny = 0, maxx = 0, maxy = 0, seen = 0;
    for (const char *s = text; *s; ) {
        unsigned cp = utf8_next(s);
        if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER)) continue;
        FT_GlyphSlot gl = g_face->glyph;
        int gx0 = pen + gl->bitmap_left;
        int gy0 = ascent - gl->bitmap_top;
        int gx1 = gx0 + (int)gl->bitmap.width;
        int gy1 = gy0 + (int)gl->bitmap.rows;
        if (!seen) {
            minx = gx0; miny = gy0; maxx = gx1; maxy = gy1; seen = 1;
        } else {
            if (gx0 < minx) minx = gx0;
            if (gy0 < miny) miny = gy0;
            if (gx1 > maxx) maxx = gx1;
            if (gy1 > maxy) maxy = gy1;
        }
        pen += gl->advance.x >> 6;
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
    FT_Set_Pixel_Sizes(g_face, 0, size);
    int ascent = g_face->size->metrics.ascender >> 6;
    int pen = x, base = y + ascent;
    for (const char *s = text; *s; ) {
        unsigned cp = utf8_next(s);
        if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER)) continue;
        FT_GlyphSlot gl = g_face->glyph;
        FT_Bitmap &bm = gl->bitmap;
        for (int pass = 0; pass < (bold ? 2 : 1); pass++) {
            int ox = pen + gl->bitmap_left + pass, oy = base - gl->bitmap_top;
            for (int row = 0; row < (int)bm.rows; row++)
                for (int col = 0; col < (int)bm.width; col++) {
                    int aa = bm.buffer[row * bm.pitch + col];
                    if (aa) put_px(ox + col, oy + row, r, g, b, aa * a / 255);
                }
        }
        pen += gl->advance.x >> 6;
    }
}

extern "C" void html_view_draw_text_contrast_px(int x, int y, const char *text, int size, int bold,
                                                int dr, int dg, int db, int lr, int lg, int lb, int a)
{
    if (!g_face || !text) return;
    FT_Set_Pixel_Sizes(g_face, 0, size);
    int ascent = g_face->size->metrics.ascender >> 6;
    int pen = x, base = y + ascent;
    for (const char *s = text; *s; ) {
        unsigned cp = utf8_next(s);
        if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER)) continue;
        FT_GlyphSlot gl = g_face->glyph;
        FT_Bitmap &bm = gl->bitmap;
        for (int pass = 0; pass < (bold ? 2 : 1); pass++) {
            int ox = pen + gl->bitmap_left + pass, oy = base - gl->bitmap_top;
            for (int row = 0; row < (int)bm.rows; row++)
                for (int col = 0; col < (int)bm.width; col++) {
                    int aa = bm.buffer[row * bm.pitch + col];
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
        pen += gl->advance.x >> 6;
    }
}

/* Hit-test a tap; returns the clicked anchor href ("" if none). */
extern "C" const char *html_view_click(float x, float y)
{
    g_clicked.clear();
    if (g_doc) {
        position::vector rb;
        float yy = y + g_scroll_y;   /* content is shifted up by scroll */
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
    *x = (int)p.x; *y = (int)p.y - g_scroll_y; *w = (int)p.width; *h = (int)p.height;
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
            for (int yy = py; yy < y + h; yy++) put_px(x + col, yy, r, g, b, fill_a);
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
