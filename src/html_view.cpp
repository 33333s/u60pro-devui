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

/* ---- FreeType ---- */
static FT_Library g_ft;
static FT_Face    g_face;

struct ft_font { int size; int ascent, descent, height; };

/* UI base dir (for <link> CSS / images) and last-clicked anchor href. */
static std::string g_ui_dir = "/data/ui";
static std::string g_clicked;

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
        FILE *f = fopen(path.c_str(), "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 0) { text.resize(n); if (fread(&text[0], 1, n, f) != (size_t)n) text.clear(); }
        fclose(f);
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
    g_doc = document::createFromString(html, g_container);
    if (!g_doc) return -1;
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
    g_doc = document::createFromString(html, g_container);
    if (!g_doc) return -1;
    g_doc->render((pixel_t)g_w);
    position clip(0, 0, (pixel_t)g_w, (pixel_t)g_h);
    g_doc->draw((uint_ptr)0, 0, 0, &clip);
    return 0;
}

/* Render into a plain logical W*H RGB565 buffer (no rotation) for animations. */
extern "C" int html_view_render_to(uint16_t *buf, const char *html)
{
    uint16_t *sfb = g_fb; int sp = g_pitch_px, sr = g_rotate;
    g_fb = buf; g_pitch_px = g_w; g_rotate = 0;
    int hh = html_view_render_html(html);
    g_fb = sfb; g_pitch_px = sp; g_rotate = sr;
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
