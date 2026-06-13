/*
 * htmlmain.c - HTML UI shell. The program is fixed; the UI lives in /data/ui
 * as plain HTML/CSS. Each *.html (except menu.html) is a swipeable page;
 * {{TOKENS}} are filled from the zwrt-datad snapshot. Touch swipes pages and
 * taps fire anchor actions (href="act:..."). Power key: short = backlight,
 * long = the menu.html overlay.
 *
 * SPDX-License-Identifier: MIT
 */
#include "drm_disp.h"
#include "data.h"
#include "touch_input.h"
#include "key_input.h"
#include "backlight.h"

#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern void        html_view_init(uint16_t *fb, int w, int h, int pitch_px, int rotate, const char *font_path);
extern void        html_view_set_uidir(const char *dir);
extern int         html_view_render_html(const char *html);
extern int         html_view_render_to(uint16_t *buf, const char *html);
extern const char *html_view_click(float x, float y);
extern int         html_view_rect(const char *sel, int *x, int *y, int *w, int *h);
extern void        html_view_polyline(int x, int y, int w, int h, const int *vals, int n,
                                      int vmin, int vmax, int r, int g, int b, int thick, int fill_a);
extern void        html_view_set_scroll(int y);
extern void        html_view_fill_rect(int x, int y, int w, int h, int r, int g, int b, int a);
extern int         html_view_render_tall(uint16_t *buf, const char *html, int bufh);
extern int         html_view_render_overlay(const char *html);

#ifndef UI_DIR
#define UI_DIR "/data/ui"
#endif
#define UI_FONT "/usr/ui/fonts/ZTEZhengYuan.ttf"

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

static uint32_t millis(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---- pages / theme ---- */
static char g_pages[24][288];
static int  g_npages, g_cur;
static int  g_theme;      /* 0 = dark, 1 = light */
static int  g_show_key;   /* reveal WiFi password (default hidden) */
static int  g_show_cellid; /* reveal NR Cell ID (default hidden) */
static int  g_show_imei;   /* reveal IMEI (default hidden) */
static int  g_speed_bits = 1; /* 1 = Mbps (bit rate), 0 = MB/s (byte rate) */
static int  g_charging;   /* set from snapshot; drives charge animation cadence */
static unsigned g_phase;  /* animation tick (battery charge sweep) */
static int  g_scroll;     /* current page vertical scroll offset */
static int  g_page_h;     /* last rendered page content height */
static int  g_autooff_ms; /* auto screen-off timeout, 0 = never */
static int  g_dtap_wake = 1; /* double-tap to wake the screen */

static void scan_pages(void)
{
    g_npages = 0;
    DIR *dp = opendir(UI_DIR);
    if (!dp) return;
    char names[24][64]; int n = 0;
    struct dirent *de;
    while ((de = readdir(dp)) && n < 24) {
        size_t l = strlen(de->d_name);
        if (l > 5 && strcmp(de->d_name + l - 5, ".html") == 0 &&
            strcmp(de->d_name, "menu.html") != 0) {
            strncpy(names[n], de->d_name, 63); names[n][63] = 0; n++;
        }
    }
    closedir(dp);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[64]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t);
            }
    for (int i = 0; i < n; i++)
        snprintf(g_pages[i], sizeof g_pages[i], "%s/%s", UI_DIR, names[i]);
    g_npages = n;
}

/* ---- value formatting ---- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void fmt_speed(char *o, size_t n, long bps) {
    if (bps >= 1024 * 1024) snprintf(o, n, "%.2f MB/s", bps / 1048576.0);
    else                    snprintf(o, n, "%.1f KB/s", bps / 1024.0);
}
static void fmt_bytes(char *o, size_t n, long b) {
    if (b >= 1024L * 1024 * 1024) snprintf(o, n, "%.2f GB", b / 1073741824.0);
    else                          snprintf(o, n, "%.1f MB", b / 1048576.0);
}
static void fmt_uptime(char *o, size_t n, long s) {
    long d = s / 86400; s %= 86400;
    snprintf(o, n, "%ldd %02ld:%02ld:%02ld", d, s / 3600, (s / 60) % 60, s % 60);
}
static void fmt_uptime_s(char *o, size_t n, long s) {   /* compact, for the grid */
    long d = s / 86400, hh = (s % 86400) / 3600, mm = (s % 3600) / 60;
    if (d > 0)       snprintf(o, n, "%ld天%ld时", d, hh);
    else if (hh > 0) snprintf(o, n, "%ld时%ld分", hh, mm);
    else             snprintf(o, n, "%ld分", mm);
}
/* compact speed for the status bar: 3.0M / 80K / 224B */
static void fmt_speed_c(char *o, size_t n, long bps) {
    if (bps >= 1024 * 1024) snprintf(o, n, "%.1fM", bps / 1048576.0);
    else if (bps >= 1024)   snprintf(o, n, "%.0fK", bps / 1024.0);
    else                    snprintf(o, n, "%ldB", bps);
}
static void fmt_one(char *o, size_t n, double v) {
    if (v >= 10) snprintf(o, n, "%.0f", v);
    else         snprintf(o, n, "%.1f", v);
}
/* Status-bar speed pair: "↑<tx> ↓<rx> <unit>", shared unit picked from the
 * larger of the two. bits=1 -> Mbps/Kbps/bps; bits=0 -> MB/s/KB/s/B/s. */
static void fmt_speed_pair(char *buf, size_t cap, long up, long down, int bits) {
    double mul = bits ? 8.0 : 1.0;
    double u = up * mul, d = down * mul, mx = u > d ? u : d;
    const char *unit; double div;
    if (bits) { if (mx >= 1e6) { unit = "Mbps"; div = 1e6; } else if (mx >= 1e3) { unit = "Kbps"; div = 1e3; } else { unit = "bps"; div = 1; } }
    else      { if (mx >= 1e6) { unit = "MB/s"; div = 1e6; } else if (mx >= 1e3) { unit = "KB/s"; div = 1e3; } else { unit = "B/s"; div = 1; } }
    char us[12], ds[12];
    fmt_one(us, sizeof us, u / div); fmt_one(ds, sizeof ds, d / div);
    snprintf(buf, cap, "\xe2\x86\x91%s \xe2\x86\x93%s %s", us, ds, unit);  /* ↑ ↓ */
}

/* ---- {{key}} template substitution ---- */
struct kv { const char *k; const char *v; };

static char *apply_template(const char *tmpl, struct kv *t, int n)
{
    static char out[32768];
    size_t o = 0;
    for (const char *p = tmpl; *p && o < sizeof(out) - 1; ) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (end) {
                int kl = (int)(end - (p + 2));
                const char *v = "";
                for (int i = 0; i < n; i++)
                    if ((int)strlen(t[i].k) == kl && strncmp(t[i].k, p + 2, kl) == 0) { v = t[i].v; break; }
                size_t vl = strlen(v);
                if (o + vl < sizeof(out) - 1) { memcpy(out + o, v, vl); o += vl; }
                p = end + 2;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return out;
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    buf[n] = 0; fclose(fp);
    return buf;
}

/* Append one carrier card (band·bw + PCI, then RSRP/SINR colored by quality).
 * A carrier reporting the floor sentinel (RSRP <= -140) is "configured but not
 * active" — its values are grayed out and tagged 未激活. */
static int car_row(char *buf, int o, int cap, const char *band, const char *bw,
                   const char *arfcn, const char *pci, const char *rsrp, const char *sinr)
{
    double rp = atof(rsrp), sn = atof(sinr);
    int inactive = rp <= -140.0;
    const char *rq = inactive ? "q-off" : rp >= -85 ? "q-good" : rp >= -105 ? "q-mid" : "q-bad";
    const char *sq = inactive ? "q-off" : sn >= 13  ? "q-good" : sn >= 0    ? "q-mid" : "q-bad";
    const char *tag = inactive ? "<span class='coff'>未激活</span>" : "";
    const char *al = (band[0] == 'n') ? "ARFCN" : "EARFCN";   /* NR vs LTE */
    return o + snprintf(buf + o, cap - o,
        "<div class='ccd%s'><span class='cb'>%s</span><span class='cbw'> %sM</span>%s"
        "<span class='cinfo'><span class='carfcn'>%s %s</span><span class='cpci'>PCI %s</span></span>"
        "<div class='cm'><span class='ml'>RSRP</span><span class='%s'>%s</span>"
        "<span class='ml ml2'>SINR</span><span class='%s'>%s</span></div></div>",
        inactive ? " off" : "", band, (bw && bw[0]) ? bw : "-", tag,
        al, (arfcn && arfcn[0]) ? arfcn : "-", (pci && pci[0]) ? pci : "-",
        rq, (rsrp && rsrp[0]) ? rsrp : "-", sq, (sinr && sinr[0]) ? sinr : "-");
}

/* Split a CA group "f0,f1,..." into fields[] (returns count). */
static int ca_split(char *g, char **f, int maxf)
{
    int n = 0; char *save;
    for (char *tk = strtok_r(g, ",", &save); tk && n < maxf; tk = strtok_r(NULL, ",", &save)) f[n++] = tk;
    return n;
}

/* Build the 5-cell staircase signal meter into buf. Strength is shown by the
 * NUMBER of filled cells (derived from RSRP); filled cells are a fixed color
 * (white), empty cells are dim. */
static void build_sigbars(char *buf, size_t cap, int rsrp)
{
    int lvl;
    if      (rsrp == 0)    lvl = 0;   /* unknown / no signal */
    else if (rsrp >= -80)  lvl = 5;
    else if (rsrp >= -90)  lvl = 4;
    else if (rsrp >= -100) lvl = 3;
    else if (rsrp >= -110) lvl = 2;
    else                   lvl = 1;
    static const int hpx[5] = { 5, 8, 11, 14, 17 };
    int o = snprintf(buf, cap, "<span class='sig'>");
    for (int b = 0; b < 5; b++)
        o += snprintf(buf + o, cap - o, "<i class='%s' style='height:%dpx'></i>",
                      b < lvl ? "on" : "", hpx[b]);
    snprintf(buf + o, cap - o, "</span>");
}

/* ---- rolling history for the charts page (sampled once per second) ---- */
#define HIST 48
static int    h_n;
static int    h_cpu[HIST], h_mem[HIST], h_ct[HIST], h_bt[HIST], h_pwr[HIST];
static long   h_rx[HIST], h_tx[HIST];
static time_t h_last;

/* charge (charger input) or discharge (battery) power, in milliwatts. */
static int power_mw(const devui_data_t *d)
{
    double w = d->charger_connect ? (d->chg_uv / 1e6) * (d->chg_ua / 1e6)
                                  : (d->bat_uv / 1e6) * (labs(d->bat_ua) / 1e6);
    return (int)(w * 1000);
}

static void hist_push(const devui_data_t *d)
{
    if (h_n < HIST) h_n++;
    else {
        memmove(h_cpu, h_cpu + 1, (HIST - 1) * sizeof h_cpu[0]);
        memmove(h_mem, h_mem + 1, (HIST - 1) * sizeof h_mem[0]);
        memmove(h_ct,  h_ct + 1,  (HIST - 1) * sizeof h_ct[0]);
        memmove(h_bt,  h_bt + 1,  (HIST - 1) * sizeof h_bt[0]);
        memmove(h_pwr, h_pwr + 1, (HIST - 1) * sizeof h_pwr[0]);
        memmove(h_rx,  h_rx + 1,  (HIST - 1) * sizeof h_rx[0]);
        memmove(h_tx,  h_tx + 1,  (HIST - 1) * sizeof h_tx[0]);
    }
    int i = h_n - 1;
    h_cpu[i] = d->cpu_usage < 0 ? 0 : (int)d->cpu_usage;
    h_mem[i] = d->mem_used_pct < 0 ? 0 : (int)d->mem_used_pct;
    h_ct[i]  = (int)d->cpu_temp;
    h_bt[i]  = d->bat_temp;
    h_pwr[i] = power_mw(d);
    h_rx[i]  = d->rx_speed;
    h_tx[i]  = d->tx_speed;
}

/* Draw the chart placeholders (#chart-cpu/#chart-mem/#chart-net) natively as
 * Bresenham polylines over the history. Called after the page is rendered; a
 * no-op on pages without those elements. */
static void draw_charts(void)
{
    int x, y, w, h;
    if (html_view_rect("#chart-cpu", &x, &y, &w, &h)) {
        html_view_polyline(x, y, w, h, h_cpu, h_n, 0, 100, 0x4f, 0x8b, 0xff, 2, 26); /* 占用 蓝 */
        html_view_polyline(x, y, w, h, h_ct,  h_n, 20, 70, 0xff, 0x8c, 0x42, 2, 0);  /* 温度 橙 */
    }
    if (html_view_rect("#chart-mem", &x, &y, &w, &h))
        html_view_polyline(x, y, w, h, h_mem, h_n, 0, 100, 0x46, 0xc4, 0x6f, 2, 34); /* 内存 绿 */
    if (html_view_rect("#chart-net", &x, &y, &w, &h)) {
        static int rxi[HIST], txi[HIST];
        long mx = 1;
        for (int i = 0; i < h_n; i++) { if (h_rx[i] > mx) mx = h_rx[i]; if (h_tx[i] > mx) mx = h_tx[i]; }
        for (int i = 0; i < h_n; i++) { rxi[i] = (int)h_rx[i]; txi[i] = (int)h_tx[i]; }
        html_view_polyline(x, y, w, h, rxi, h_n, 0, (int)mx, 0x4f, 0x8b, 0xff, 2, 22); /* 下行 蓝 */
        html_view_polyline(x, y, w, h, txi, h_n, 0, (int)mx, 0xff, 0x8c, 0x42, 2, 0);  /* 上行 橙 */
    }
    if (html_view_rect("#chart-batt", &x, &y, &w, &h)) {
        static int tn[HIST], pn[HIST];
        int mx = 1;
        for (int i = 0; i < h_n; i++) if (h_pwr[i] > mx) mx = h_pwr[i];
        for (int i = 0; i < h_n; i++) { tn[i] = (h_bt[i] - 20) * 2; pn[i] = (int)((long)h_pwr[i] * 100 / mx); }
        html_view_polyline(x, y, w, h, pn, h_n, 0, 100, 0x4f, 0x8b, 0xff, 2, 22); /* 功率 蓝 */
        html_view_polyline(x, y, w, h, tn, h_n, 0, 100, 0xff, 0x8c, 0x42, 2, 0);  /* 温度 橙 */
    }
}

/* ---- band lock (锁频): comma-list band sets ---- */
static int  g_modal;           /* 0 none, 1 SA, 2 NSA, 3 LTE (second-level dialog) */
static int  g_segdrag;         /* dragging the segmented control (suppress cell highlight) */
static char g_toast[48];       /* toast message ("" = hidden) */
static uint32_t g_toast_until; /* millis the toast hides at */
static char g_net_pending[16]; /* optimistic net mode until net_select catches up */
static char g_uni_sa[256], g_uni_nsa[256], g_uni_lte[256];   /* available bands (max seen) */
static char g_sel_sa[256], g_sel_nsa[256], g_sel_lte[256];   /* selected (to lock) */

static int band_count(const char *s) { if (!s[0]) return 0; int n = 1; for (; *s; s++) if (*s == ',') n++; return n; }

static int band_in(const char *list, const char *b)
{
    size_t bl = strlen(b);
    for (const char *p = list; *p; ) {
        const char *c = p; while (*c && *c != ',') c++;
        if ((size_t)(c - p) == bl && !strncmp(p, b, bl)) return 1;
        p = *c ? c + 1 : c;
    }
    return 0;
}
static void band_toggle(char *list, size_t cap, const char *b)
{
    if (band_in(list, b)) {
        char out[256]; int o = 0; size_t bl = strlen(b);
        for (const char *p = list; *p; ) {
            const char *c = p; while (*c && *c != ',') c++;
            if (!((size_t)(c - p) == bl && !strncmp(p, b, bl))) {
                if (o) out[o++] = ',';
                int n = (int)(c - p); if (o + n < (int)sizeof out) { memcpy(out + o, p, n); o += n; }
            }
            p = *c ? c + 1 : c;
        }
        out[o] = 0; snprintf(list, cap, "%s", out);
    } else {
        size_t l = strlen(list);
        snprintf(list + l, cap - l, "%s%s", l ? "," : "", b);
    }
}
/* Render bands as pill chips (selected = highlighted) with the given classes. */
static void build_chips_cls(char *buf, size_t cap, const char *uni, const char *sel,
                            const char *act, const char *pfx, const char *on, const char *off)
{
    char u[256]; snprintf(u, sizeof u, "%s", uni);
    int o = 0;
    for (char *tk = strtok(u, ","); tk; tk = strtok(NULL, ",")) {
        o += snprintf(buf + o, cap - o, "<a href='act:%s:%s' class='%s'>%s%s</a>",
                      act, tk, band_in(sel, tk) ? on : off, pfx, tk);
    }
    if (o == 0) snprintf(buf, cap, "<span class='muted'>无可用频段</span>");
}

/* One-line summary of a card's current band selection. */
static void band_summary(char *buf, size_t cap, const char *uni, const char *sel, const char *pfx)
{
    if (!sel[0]) { snprintf(buf, cap, "未选择频段"); return; }
    if (!strcmp(uni, sel)) { snprintf(buf, cap, "全部频段（未锁定）"); return; }
    char s[256]; snprintf(s, sizeof s, "%s", sel);
    int o = 0;
    for (char *tk = strtok(s, ","); tk; tk = strtok(NULL, ","))
        o += snprintf(buf + o, cap - o, "%s%s%s", o ? " " : "已锁 ", pfx, tk);
}

/* segmented-control cells (shared with the touch handler). */
static const struct { const char *v, *lab; } g_netmodes[4] = {
    { "WL_AND_5G", "自动" }, { "Only_5G", "5G SA" },
    { "LTE_AND_5G", "5G NSA" }, { "Only_LTE", "4G" } };
static const struct { int ms; const char *lab; } g_autooffs[6] = {
    { 0, "关" }, { 30000, "30秒" }, { 60000, "1分" },
    { 120000, "2分" }, { 300000, "5分" }, { 600000, "10分" } };

/* Build the second-level band-lock dialog (overlaid on the dimmed page). */
static const char *modal_html(void)
{
    static char out[3200];
    const char *uni, *sel, *pfx, *title, *act;
    if (g_modal == 1)      { uni = g_uni_sa;  sel = g_sel_sa;  pfx = "n"; act = "bsa";  title = "5G SA 锁频"; }
    else if (g_modal == 2) { uni = g_uni_nsa; sel = g_sel_nsa; pfx = "n"; act = "bnsa"; title = "5G NSA 锁频"; }
    else                   { uni = g_uni_lte; sel = g_sel_lte; pfx = "B"; act = "blte"; title = "4G 锁频"; }
    char chips[2200];
    build_chips_cls(chips, sizeof chips, uni, sel, act, pfx, "bchip-on", "bchip");
    snprintf(out, sizeof out,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><link rel='stylesheet' href='style.css'></head>"
        "<body class='mo'><div class='modal %s'>"
        "<div class='mtitle'>%s</div><div class='mbands'>%s</div>"
        "<div class='mbtns'>"
        "<a href='act:mall' class='mbtn2'>全选/全不选</a>"
        "<a href='act:minv' class='mbtn2'>反选</a>"
        "<a href='act:mapply' class='mbtn2 prim'>应用</a></div>"
        "</div></body></html>",
        g_theme ? "light" : "dark", title, chips);
    return out;
}

/* Fill a kv table from the current device state. Buffers are static. */
static int build_kv(struct kv *t)
{
    g_phase++;

    static char s_time[8], s_bat[8], s_rsrp[12], s_rsrq[12], s_sinr[12], s_bw[12];
    static char s_cellid[20], s_pci[12], s_clients[8], s_up[24], s_rxs[20], s_txs[20];
    static char s_rxb[16], s_txb[16], s_cpu[8], s_mem[8], w_rsrp[6], w_rsrq[6], w_sinr[6], w_bw[6];
    static char s_oper[48], s_ssid[64], s_key[64], s_page[8], s_np[8], s_model[64], s_fw[80];
    static char s_qci[8], s_ambr[24], s_sig[280], s_sbar[640], s_dots[320];

    devui_data_t d;
    if (!data_refresh(&d)) memset(&d, 0, sizeof d);
    g_charging = d.charger_connect;
    snprintf(s_page, sizeof s_page, "%d", g_cur + 1);
    snprintf(s_np, sizeof s_np, "%d", g_npages);

    time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
    if (now != h_last) { hist_push(&d); h_last = now; }   /* sample once per second */
    snprintf(s_time, sizeof s_time, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    snprintf(s_bat, sizeof s_bat, "%d", d.bat_percent);
    snprintf(s_rsrp, sizeof s_rsrp, "%d", d.nr_rsrp);
    snprintf(s_rsrq, sizeof s_rsrq, "%d", d.nr_rsrq);
    snprintf(s_sinr, sizeof s_sinr, "%s", d.nr_snr[0] ? d.nr_snr : "-");
    snprintf(s_bw, sizeof s_bw, "%s", d.nr_bw[0] ? d.nr_bw : "-");
    if (g_show_cellid) snprintf(s_cellid, sizeof s_cellid, "%ld", d.nr_cell_id);
    else               strcpy(s_cellid, "********");
    snprintf(s_pci, sizeof s_pci, "%d", d.nr_pci);
    snprintf(s_clients, sizeof s_clients, "%d", d.clients_total);
    fmt_uptime(s_up, sizeof s_up, d.uptime);
    fmt_speed(s_rxs, sizeof s_rxs, d.rx_speed);
    fmt_speed(s_txs, sizeof s_txs, d.tx_speed);
    fmt_bytes(s_rxb, sizeof s_rxb, d.rx_bytes);
    fmt_bytes(s_txb, sizeof s_txb, d.tx_bytes);
    snprintf(s_cpu, sizeof s_cpu, "%ld", d.cpu_temp);
    snprintf(s_mem, sizeof s_mem, "%ld", d.mem_used_pct);
    snprintf(s_oper, sizeof s_oper, "%s", d.operator_name[0] ? d.operator_name : "--");
    snprintf(s_ssid, sizeof s_ssid, "%s", d.wifi_ssid[0] ? d.wifi_ssid : "-");
    if (!d.wifi_key[0]) strcpy(s_key, "-");
    else if (g_show_key) snprintf(s_key, sizeof s_key, "%s", d.wifi_key);
    else { int kn = (int)strlen(d.wifi_key); if (kn > 16) kn = 16; memset(s_key, '*', kn); s_key[kn] = 0; }
    snprintf(s_model, sizeof s_model, "%s", d.model[0] ? d.model : "-");
    snprintf(s_fw, sizeof s_fw, "%s", d.fw[0] ? d.fw : "-");
    snprintf(w_rsrp, sizeof w_rsrp, "%d", clampi((d.nr_rsrp + 120) * 2, 3, 100));
    snprintf(w_rsrq, sizeof w_rsrq, "%d", clampi((d.nr_rsrq + 20) * 100 / 17, 3, 100));
    snprintf(w_sinr, sizeof w_sinr, "%d", clampi((int)((atof(d.nr_snr) + 10) * 100 / 40), 3, 100));
    snprintf(w_bw, sizeof w_bw, "%d", clampi(atoi(d.nr_bw), 3, 100));

    /* qci / ambr (from modem qos): unified Mbps, e.g. 3000/200 Mbps */
    snprintf(s_qci, sizeof s_qci, "%d", d.qci);
    snprintf(s_ambr, sizeof s_ambr, "%.0f/%.0f Mbps", d.ambr_dl, d.ambr_ul);

    build_sigbars(s_sig, sizeof s_sig, d.nr_rsrp ? d.nr_rsrp : d.lte_rsrp);

    /* ---- per-carrier display + generation badge ----
     * nrca group: idx,pci,?,band,arfcn,bw,?,rsrp,rsrq,sinr,rssi
     * lteca group: pci,band,?,earfcn,bw   (includes the serving cell) */
    static char s_nrrows[1500], s_lterows[1700], s_carriers[3600], s_gen[8];
    int is_endc = strstr(d.net_type, "ENDC") || strstr(d.net_type, "EN-DC");
    int is_nsa  = strstr(d.net_type, "NSA") != NULL;
    int is_sa   = strstr(d.net_type, "SA") && !is_nsa;
    int is_lte  = !is_nsa && !is_sa && (strstr(d.net_type, "LTE") || strstr(d.net_type, "4G"));
    int show_nr  = is_sa || is_nsa || is_endc;
    int show_lte = is_nsa || is_endc || is_lte;
    int nr_cc = 0, nr_bw = 0, lte_cc = 0, lte_bw = 0, no = 0, lo = 0;
    char rp[12], pc[12], ac[16];

    /* NR carriers: PCell from main fields + nrca SCells */
    s_nrrows[0] = 0;
    if (show_nr && d.band[0] && d.nr_bw[0]) {
        snprintf(rp, sizeof rp, "%d", d.nr_rsrp); snprintf(pc, sizeof pc, "%d", d.nr_pci);
        snprintf(ac, sizeof ac, "%ld", d.nr_channel);
        no = car_row(s_nrrows, no, sizeof s_nrrows, d.band, d.nr_bw, ac, pc, rp, d.nr_snr);
        nr_cc = 1; nr_bw = atoi(d.nr_bw);
    }
    if (show_nr) {
        char nrca[256]; strncpy(nrca, d.nrca, sizeof nrca - 1); nrca[sizeof nrca - 1] = 0;
        for (char *grp = strtok(nrca, ";"); grp; grp = strtok(NULL, ";")) {
            char g[96]; strncpy(g, grp, sizeof g - 1); g[sizeof g - 1] = 0;
            char *f[12]; int nf = ca_split(g, f, 12);
            if (nf > 5 && atoi(f[5]) > 0) {
                char bn[12]; snprintf(bn, sizeof bn, "n%s", f[3]);
                no = car_row(s_nrrows, no, sizeof s_nrrows, bn, f[5], nf > 4 ? f[4] : "-",
                             nf > 1 ? f[1] : "-", nf > 7 ? f[7] : "-", nf > 9 ? f[9] : "-");
                nr_cc++; nr_bw += atoi(f[5]);
            }
        }
    }

    /* LTE carriers: lteca "pci,band,?,earfcn,bw" (first entry = serving cell) */
    s_lterows[0] = 0;
    if (show_lte) {
        char lteca[256]; strncpy(lteca, d.lteca, sizeof lteca - 1); lteca[sizeof lteca - 1] = 0;
        for (char *grp = strtok(lteca, ";"); grp; grp = strtok(NULL, ";")) {
            char g[96]; strncpy(g, grp, sizeof g - 1); g[sizeof g - 1] = 0;
            char *f[8]; int nf = ca_split(g, f, 8);
            if (nf >= 5 && atoi(f[4]) > 0) {
                char bn[12]; snprintf(bn, sizeof bn, "B%s", f[1]);
                char lr[12] = "-", ls[12] = "-";
                if (lte_cc == 0) {   /* serving-cell signal from main fields */
                    if (d.lte_rsrp) snprintf(lr, sizeof lr, "%d", d.lte_rsrp);
                    if (d.lte_snr[0]) snprintf(ls, sizeof ls, "%s", d.lte_snr);
                }
                lo = car_row(s_lterows, lo, sizeof s_lterows, bn, f[4], f[3], f[0], lr, ls);
                lte_cc++; lte_bw += atoi(f[4]);
            }
        }
    }
    (void)no; (void)lo; (void)pc; (void)ac;

    /* total bandwidth per NSA/ENDC rules; header label */
    int total_bw; const char *cmode;
    if (is_endc)     { total_bw = nr_bw + lte_bw; cmode = "EN-DC"; }
    else if (is_nsa) { total_bw = nr_cc > 0 ? nr_bw : lte_bw; cmode = "5G NSA"; }
    else if (is_sa)  { total_bw = nr_bw; cmode = "5G SA"; }
    else if (is_lte) { total_bw = lte_bw; cmode = (d.mcc == 460) ? "4G" : "4G LTE"; }
    else             { total_bw = 0; cmode = d.net_type[0] ? d.net_type : "无服务"; }
    { char cnt[48];
      if (lte_cc && nr_cc) snprintf(cnt, sizeof cnt, "%d LTE + %d NR 载波", lte_cc, nr_cc);
      else if (nr_cc)      snprintf(cnt, sizeof cnt, "%d NR 载波", nr_cc);
      else if (lte_cc)     snprintf(cnt, sizeof cnt, "%d LTE 载波", lte_cc);
      else                 snprintf(cnt, sizeof cnt, "无载波");
      snprintf(s_carriers, sizeof s_carriers, "<div class='sec'>%s · %s · %d MHz</div>%s%s",
               cmode, cnt, total_bw, s_nrrows, s_lterows); }

    /* generation badge: 5GA / 5G+ / 5G / 4G / LTE / 3G */
    int op = 0;   /* 1 mobile, 2 unicom, 3 telecom, 4 broadnet, 5 other-mainland */
    if (d.mcc == 460) {
        int m = d.mnc;
        if (m == 0 || m == 2 || m == 4 || m == 7 || m == 8) op = 1;
        else if (m == 1 || m == 6 || m == 9) op = 2;
        else if (m == 3 || m == 5 || m == 11) op = 3;
        else if (m == 15) op = 4; else op = 5;
    }
    /* operator: show Chinese names for the four mainland carriers */
    if (d.mcc == 460) {
        const char *cn = op == 1 ? "中国移动" : op == 2 ? "中国联通" :
                         op == 3 ? "中国电信" : op == 4 ? "中国广电" : NULL;
        if (cn) snprintf(s_oper, sizeof s_oper, "%s", cn);
    }

    const char *gen = "--";
    if (is_sa) {
        if ((op == 1 || op == 4) && nr_cc >= 3) gen = "5GA";
        else if ((op == 2 || op == 3) && nr_bw >= 200) gen = "5GA";
        else if (nr_bw > 100) gen = "5G+";
        else gen = "5G";
    } else if (is_nsa || is_endc) gen = "5G";
    else if (is_lte) gen = (d.mcc == 460) ? "4G" : "LTE";
    else if (d.net_type[0]) gen = "3G";
    snprintf(s_gen, sizeof s_gen, "%s", gen);
    const char *genc = "g3";
    if      (!strcmp(gen, "5GA")) genc = "g5ga";
    else if (!strcmp(gen, "5G+")) genc = "g5p";
    else if (!strcmp(gen, "5G"))  genc = "g5";
    else if (!strcmp(gen, "4G"))  genc = "g4";
    else if (!strcmp(gen, "LTE")) genc = "glte";

    /* ---- shared status bar: time | speed | gen-text | sigbars | battery | % ---- */
    {
        char sp[40], glow[80] = "", bcls[20];
        fmt_speed_pair(sp, sizeof sp, d.tx_speed, d.rx_speed, g_speed_bits);
        if (g_charging) {
            int gp = (g_phase % 12) * 9;        /* 0..99 sweep */
            snprintf(glow, sizeof glow, "<span class='glow' style='left:%d%%'></span>", gp);
        }
        snprintf(bcls, sizeof bcls, "%s%s", g_charging ? "chg " : "",
                 d.bat_percent <= 20 ? "low" : "");
        snprintf(s_sbar, sizeof s_sbar,
            "<div class='sbar'><span class='clk'>%s</span><span class='r'>"
            "<span class='spd'>%s</span>"
            "<span class='gt %s'>%s</span>"
            "%s"
            "<span class='bw'><span class='batt %s'><span class='bf' style='width:%d%%'></span>%s</span><span class='tip'></span></span>"
            "<span class='bp'>%d%%</span>"
            "</span></div>",
            s_time, sp, genc, gen, s_sig,
            bcls, clampi(d.bat_percent, 0, 100), glow,
            d.bat_percent);
    }

    /* ---- bottom page dots ---- */
    {
        int o = snprintf(s_dots, sizeof s_dots, "<div class='dots'>");
        for (int p = 0; p < g_npages; p++)
            o += snprintf(s_dots + o, sizeof s_dots - o, "<span class='dot%s'></span>", p == g_cur ? " on" : "");
        snprintf(s_dots + o, sizeof s_dots - o, "</div>");
    }

    /* charts are drawn natively after render (see draw_charts), not via tokens. */

    /* ---- band lock: universe grows to the largest set seen; selection mirrors
     * the live lock unless the user is editing in the modal ---- */
    static char s_netseg[640], s_cursa[300], s_curnsa[300], s_curlte[300], s_toast[120];
    if (band_count(d.sa_bands)  >= band_count(g_uni_sa))  snprintf(g_uni_sa,  sizeof g_uni_sa,  "%s", d.sa_bands);
    if (band_count(d.nsa_bands) >= band_count(g_uni_nsa)) snprintf(g_uni_nsa, sizeof g_uni_nsa, "%s", d.nsa_bands);
    if (band_count(d.lte_bands) >= band_count(g_uni_lte)) snprintf(g_uni_lte, sizeof g_uni_lte, "%s", d.lte_bands);
    if (!g_modal) {
        if (d.sa_bands[0])  snprintf(g_sel_sa,  sizeof g_sel_sa,  "%s", d.sa_bands);
        if (d.nsa_bands[0]) snprintf(g_sel_nsa, sizeof g_sel_nsa, "%s", d.nsa_bands);
        if (d.lte_bands[0]) snprintf(g_sel_lte, sizeof g_sel_lte, "%s", d.lte_bands);
    }
    band_summary(s_cursa,  sizeof s_cursa,  g_uni_sa,  g_sel_sa,  "n");
    band_summary(s_curnsa, sizeof s_curnsa, g_uni_nsa, g_sel_nsa, "n");
    band_summary(s_curlte, sizeof s_curlte, g_uni_lte, g_sel_lte, "B");
    {   /* segmented control: highlight current mode (pending overrides until it applies) */
        if (g_net_pending[0] && !strcmp(g_net_pending, d.net_select)) g_net_pending[0] = 0;
        const char *cur = g_net_pending[0] ? g_net_pending : d.net_select;
        int active = 0;
        for (int k = 0; k < 4; k++) if (!strcmp(g_netmodes[k].v, cur)) active = k;
        int hl = g_segdrag == 1 ? -1 : active;   /* during drag the native box is the highlight */
        int o = snprintf(s_netseg, sizeof s_netseg, "<div id='netseg' class='seg seg4'>");
        for (int k = 0; k < 4; k++)
            o += snprintf(s_netseg + o, sizeof s_netseg - o, "<a href='act:net:%s' class='segc%s'>%s</a>",
                          g_netmodes[k].v, k == hl ? " seg-on" : "", g_netmodes[k].lab);
        snprintf(s_netseg + o, sizeof s_netseg - o, "</div>");
    }
    s_toast[0] = 0;
    if (g_toast[0]) snprintf(s_toast, sizeof s_toast, "<div class='toast'>%s</div>", g_toast);

    /* ---- system extras: usage, temps, version, imei, brightness, auto-off ---- */
    static char s_cusage[8], s_ctemp[8], s_btemp[8], s_swver[80], s_imei[24], s_spu[8], s_bright[8];
    static char s_memdet[24], s_upshort[16], s_autooff[420];
    { int bmax = backlight_max(); if (bmax <= 0) bmax = 255;
      snprintf(s_bright, sizeof s_bright, "%d", clampi(backlight_get() * 100 / bmax, 0, 100)); }
    { long mt = d.mem_total, ma = d.mem_avail;
      snprintf(s_memdet, sizeof s_memdet, "%ld/%ld MB", mt ? (mt - ma) / 1048576 : 0, mt / 1048576); }
    static char s_chgv[8], s_chgi[10], s_batv[8], s_bati[10], s_pwr[10];
    snprintf(s_chgv, sizeof s_chgv, "%.2f", d.chg_uv / 1e6);
    snprintf(s_chgi, sizeof s_chgi, "%ld", d.chg_ua / 1000);
    snprintf(s_batv, sizeof s_batv, "%.2f", d.bat_uv / 1e6);
    snprintf(s_bati, sizeof s_bati, "%ld", labs(d.bat_ua) / 1000);
    { double pw = d.charger_connect ? (d.chg_uv / 1e6) * (d.chg_ua / 1e6)
                                    : (d.bat_uv / 1e6) * (labs(d.bat_ua) / 1e6);
      snprintf(s_pwr, sizeof s_pwr, "%.1f", pw); }
    fmt_uptime_s(s_upshort, sizeof s_upshort, d.uptime);
    {   /* auto-off segmented control (#autoseg), same UI as net mode */
        int active = 0;
        for (int k = 0; k < 6; k++) if (g_autooffs[k].ms == g_autooff_ms) active = k;
        int hl = g_segdrag == 2 ? -1 : active;
        int o = snprintf(s_autooff, sizeof s_autooff, "<div id='autoseg' class='seg seg6'>");
        for (int k = 0; k < 6; k++)
            o += snprintf(s_autooff + o, sizeof s_autooff - o,
                "<a href='act:autooff:%d' class='segc%s'>%s</a>",
                g_autooffs[k].ms, k == hl ? " seg-on" : "", g_autooffs[k].lab);
        snprintf(s_autooff + o, sizeof s_autooff - o, "</div>");
    }
    snprintf(s_cusage, sizeof s_cusage, "%ld", d.cpu_usage < 0 ? 0 : d.cpu_usage);
    snprintf(s_ctemp, sizeof s_ctemp, "%ld", d.cpu_temp);
    snprintf(s_btemp, sizeof s_btemp, "%d", d.bat_temp);
    snprintf(s_swver, sizeof s_swver, "%s", d.sw_version[0] ? d.sw_version : "-");
    if (g_show_imei && d.imei[0]) snprintf(s_imei, sizeof s_imei, "%s", d.imei);
    else { int n = d.imei[0] ? (int)strlen(d.imei) : 15; if (n > 23) n = 23; memset(s_imei, '*', n); s_imei[n] = 0; }
    snprintf(s_spu, sizeof s_spu, "%s", g_speed_bits ? "Mbps" : "MB/s");

    /* ---- connected device list (page 2), capped to keep the page in 480px ---- */
    static char s_clist[1400];
    {
        int o = 0, show = d.client_n < 3 ? d.client_n : 2;
        if (d.client_n == 0)
            o += snprintf(s_clist + o, sizeof s_clist - o, "<div class='cli muted'>暂无已连接设备</div>");
        for (int i = 0; i < show; i++)
            o += snprintf(s_clist + o, sizeof s_clist - o,
                "<div class='cli'><span class='cn'>%s</span><span class='cip'>%s</span></div>",
                d.client[i].name, d.client[i].ip);
        if (d.client_n > show)
            o += snprintf(s_clist + o, sizeof s_clist - o,
                "<div class='cli muted'>…另有 %d 台</div>", d.client_n - show);
    }

    /* ---- dhcp summary (page 2) ---- */
    static char s_pool[64], s_lease[24];
    snprintf(s_pool, sizeof s_pool, "%s · 共 %s",
             d.dhcp_start[0] ? d.dhcp_start : "-", d.dhcp_limit[0] ? d.dhcp_limit : "-");
    { long lt = atol(d.dhcp_leasetime);
      if (lt >= 3600)     snprintf(s_lease, sizeof s_lease, "%ld 小时", lt / 3600);
      else if (lt >= 60)  snprintf(s_lease, sizeof s_lease, "%ld 分钟", lt / 60);
      else if (lt > 0)    snprintf(s_lease, sizeof s_lease, "%ld 秒", lt);
      else                strcpy(s_lease, "-"); }

    int adb_on = !strcmp(d.usb_mode, "debug");
    int connected = strstr(d.wan_status, "connect") != NULL;
    int i = 0;
    t[i++] = (struct kv){ "STATUSBAR", s_sbar };   t[i++] = (struct kv){ "DOTS", s_dots };
    t[i++] = (struct kv){ "SIGBARS", s_sig };
    t[i++] = (struct kv){ "TIME", s_time };       t[i++] = (struct kv){ "BAT", s_bat };
    t[i++] = (struct kv){ "OPER", s_oper };        t[i++] = (struct kv){ "NETTYPE", d.net_type };
    t[i++] = (struct kv){ "BAND", d.band };        t[i++] = (struct kv){ "WAN", connected ? "已连接" : "未连接" };
    t[i++] = (struct kv){ "RSRP", s_rsrp };        t[i++] = (struct kv){ "RSRP_W", w_rsrp };
    t[i++] = (struct kv){ "RSRQ", s_rsrq };        t[i++] = (struct kv){ "RSRQ_W", w_rsrq };
    t[i++] = (struct kv){ "SINR", s_sinr };        t[i++] = (struct kv){ "SINR_W", w_sinr };
    t[i++] = (struct kv){ "BW", s_bw };            t[i++] = (struct kv){ "BW_W", w_bw };
    t[i++] = (struct kv){ "CELLID", s_cellid };    t[i++] = (struct kv){ "PCI", s_pci };
    t[i++] = (struct kv){ "CELLBTN", g_show_cellid ? "隐藏" : "显示" };
    t[i++] = (struct kv){ "CLIENTS", s_clients };  t[i++] = (struct kv){ "UPTIME", s_up };
    t[i++] = (struct kv){ "RXSPEED", s_rxs };      t[i++] = (struct kv){ "TXSPEED", s_txs };
    t[i++] = (struct kv){ "RXBYTES", s_rxb };      t[i++] = (struct kv){ "TXBYTES", s_txb };
    t[i++] = (struct kv){ "CPU", s_cpu };          t[i++] = (struct kv){ "MEM", s_mem };
    t[i++] = (struct kv){ "QCI", s_qci };          t[i++] = (struct kv){ "AMBR", s_ambr };
    t[i++] = (struct kv){ "SSID", s_ssid };        t[i++] = (struct kv){ "KEY", s_key };
    t[i++] = (struct kv){ "ENC", d.wifi_enc[0] ? d.wifi_enc : "-" };
    t[i++] = (struct kv){ "MODEL", s_model };       t[i++] = (struct kv){ "FW", s_fw };
    t[i++] = (struct kv){ "PAGE", s_page };         t[i++] = (struct kv){ "NPAGES", s_np };
    t[i++] = (struct kv){ "CARRIERS", s_carriers }; t[i++] = (struct kv){ "GEN", s_gen };
    t[i++] = (struct kv){ "GENCLASS", genc };
    t[i++] = (struct kv){ "THEME", g_theme ? "light" : "dark" };
    t[i++] = (struct kv){ "BATCLASS", d.bat_percent <= 20 ? "low" : "" };
    t[i++] = (struct kv){ "KEYBTN", g_show_key ? "隐藏密码" : "显示密码" };
    t[i++] = (struct kv){ "ADBCLASS", adb_on ? "on" : "off" };
    t[i++] = (struct kv){ "ADBSTATE", adb_on ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "THEMECLASS", g_theme ? "on" : "off" };
    t[i++] = (struct kv){ "THEMESTATE", g_theme ? "浅色模式" : "深色模式" };
    t[i++] = (struct kv){ "SPUNITCLASS", g_speed_bits ? "on" : "off" };
    t[i++] = (struct kv){ "SPUNITSTATE", g_speed_bits ? "比特率 Mbps" : "字节率 MB/s" };
    t[i++] = (struct kv){ "SPUNIT", s_spu };
    /* system page */
    t[i++] = (struct kv){ "CPUUSAGE", s_cusage };  t[i++] = (struct kv){ "CPUTEMP", s_ctemp };
    t[i++] = (struct kv){ "BATTEMP", s_btemp };    t[i++] = (struct kv){ "SWVER", s_swver };
    t[i++] = (struct kv){ "IMEI", s_imei };
    t[i++] = (struct kv){ "IMEIBTN", g_show_imei ? "隐藏" : "显示" };
    t[i++] = (struct kv){ "BRIGHT", s_bright }; t[i++] = (struct kv){ "AUTOOFF", s_autooff };
    t[i++] = (struct kv){ "MEMDETAIL", s_memdet }; t[i++] = (struct kv){ "UPSHORT", s_upshort };
    t[i++] = (struct kv){ "CHGV", s_chgv };  t[i++] = (struct kv){ "CHGI", s_chgi };
    t[i++] = (struct kv){ "BATV", s_batv };  t[i++] = (struct kv){ "BATI", s_bati };
    t[i++] = (struct kv){ "PWR", s_pwr };    t[i++] = (struct kv){ "PWRLBL", d.charger_connect ? "充电" : "放电" };
    t[i++] = (struct kv){ "DTAPCLASS", g_dtap_wake ? "on" : "off" };
    t[i++] = (struct kv){ "DTAPSTATE", g_dtap_wake ? "开启" : "关闭" };
    /* lock page */
    t[i++] = (struct kv){ "NETSEG", s_netseg };  t[i++] = (struct kv){ "TOAST", s_toast };
    t[i++] = (struct kv){ "CURSA", s_cursa };    t[i++] = (struct kv){ "CURNSA", s_curnsa };
    t[i++] = (struct kv){ "CURLTE", s_curlte };
    /* wifi page */
    t[i++] = (struct kv){ "NFCCLASS", d.nfc_switch ? "on" : "off" };
    t[i++] = (struct kv){ "NFCSTATE", d.nfc_switch ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WIFICLASS", d.wifi_enabled ? "on" : "off" };
    t[i++] = (struct kv){ "WIFISTATE", d.wifi_enabled ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "CLIENTLIST", s_clist }; t[i++] = (struct kv){ "DHCP_IP", d.dhcp_ip[0] ? d.dhcp_ip : "-" };
    t[i++] = (struct kv){ "DHCP_POOL", s_pool };   t[i++] = (struct kv){ "DHCP_LEASE", s_lease };
    return i;
}

/* Build the data-filled HTML for a page (returns a static buffer). */
static const char *page_html(const char *path)
{
    char *tmpl = read_file(path);   /* re-read each time = live reload */
    if (!tmpl) return NULL;
    struct kv t[96];
    int n = build_kv(t);
    char *html = apply_template(tmpl, t, n);
    free(tmpl);
    return html;
}

static void capture_fb(drm_disp_t *d);   /* fwd: fb snapshot for modal overlay */

static void render(drm_disp_t *disp, const char *path)
{
    const char *html = page_html(path);
    if (!html) return;
    html_view_set_scroll(g_scroll);
    g_page_h = html_view_render_html(html);
    draw_charts();   /* native polylines into any #chart-* placeholders */
    int H = disp->height, maxs = g_page_h - H;
    (void)maxs;
    if (g_modal) {                                    /* dim page + second-level dialog */
        html_view_fill_rect(0, 28, disp->width, H - 28, 0, 0, 0, 150);
        capture_fb(disp);                             /* snapshot page+dim for fast modal refresh */
        html_view_render_overlay(modal_html());
    }
    drm_disp_dirty(disp, 0, 0, disp->width - 1, disp->height - 1);
}

/* Offscreen page bitmaps (logical, no rotation) for slide transitions.
 * During a drag: g_bufA = left page, g_bufB = right page (windowed by offset o:
 * window column x shows [left|right][x+o], o in 0..W). */
static uint16_t g_bufA[320 * 480], g_bufB[320 * 480];

static void compose_frame(drm_disp_t *d, int o)
{
    const int W = d->width, H = d->height, pp = d->pitch_px;
    if (o < 0) o = 0; if (o > W) o = W;
    for (int y = 0; y < H; y++) {
        uint16_t *dp = d->fb + (size_t)(H - 1 - y) * pp + (W - 1);
        const uint16_t *lr = g_bufA + (size_t)y * W, *rr = g_bufB + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            int idx = x + o;
            *dp-- = (idx < W) ? lr[idx] : rr[idx - W];
        }
    }
    drm_disp_dirty(d, 0, 0, W - 1, H - 1);
}

/* Render the page pair for a drag direction into A(left)/B(right). dir>0 = next.
 * The current page keeps its scroll; the target page comes in at the top. */
static void prep_pair(int target, int dir)
{
    const char *h;
    if (dir > 0) {   /* next: left=current, right=target */
        html_view_set_scroll(g_scroll); h = page_html(g_pages[g_cur]);  if (h) html_view_render_to(g_bufA, h);
        html_view_set_scroll(0);        h = page_html(g_pages[target]); if (h) html_view_render_to(g_bufB, h);
    } else {         /* prev: left=target, right=current */
        html_view_set_scroll(0);        h = page_html(g_pages[target]); if (h) html_view_render_to(g_bufA, h);
        html_view_set_scroll(g_scroll); h = page_html(g_pages[g_cur]);  if (h) html_view_render_to(g_bufB, h);
    }
    html_view_set_scroll(g_scroll);
}

/* Settle the offset from o0 to o1 over a few frames. */
static void anim_o(drm_disp_t *d, int o0, int o1)
{
    const int FR = 5;
    for (int f = 1; f <= FR; f++) compose_frame(d, o0 + (o1 - o0) * f / FR);
}

/* Smooth vertical scrolling: the full page is pre-rendered once into g_scrollbuf
 * (logical, unrotated); each drag frame just blits the visible window + scrollbar
 * — no re-parse/re-layout, so it keeps up like the horizontal swipe. */
#define SCROLLMAX 1280
static uint16_t g_scrollbuf[320 * SCROLLMAX];
static int g_scroll_h;

static void scroll_blit(drm_disp_t *d, int scroll)
{
    const int W = d->width, Hh = d->height, pp = d->pitch_px;
    for (int y = 0; y < Hh; y++) {
        int sy = y + scroll;
        const uint16_t *src = (sy >= 0 && sy < g_scroll_h) ? &g_scrollbuf[(size_t)sy * W] : NULL;
        uint16_t *dp = d->fb + (size_t)(Hh - 1 - y) * pp + (W - 1);
        for (int x = 0; x < W; x++) *dp-- = src ? src[x] : 0;
    }
    drm_disp_dirty(d, 0, 0, W - 1, Hh - 1);
}

/* fb snapshot for fast overlay refresh (modal toggles, segmented-control drag):
 * avoids re-laying-out the whole page on every interaction. */
static uint16_t g_overbg[320 * 480];
static void capture_fb(drm_disp_t *d) { memcpy(g_overbg, d->fb, (size_t)d->pitch_px * d->height * sizeof(uint16_t)); }
static void restore_fb(drm_disp_t *d) { memcpy(d->fb, g_overbg, (size_t)d->pitch_px * d->height * sizeof(uint16_t)); }

/* Redraw the modal over the cached dimmed page (fast: no page relayout). */
static void render_modal_overlay(drm_disp_t *d)
{
    restore_fb(d);
    html_view_render_overlay(modal_html());
    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}

/* Draw the sliding segmented-control highlight box at the finger, over cached fb.
 * n = number of cells. */
static void seg_box(drm_disp_t *d, int sx, int sy, int sw, int sh, int n, int fx)
{
    restore_fb(d);
    int cw = sw / n, bx = fx - cw / 2;
    if (bx < sx) bx = sx; if (bx > sx + sw - cw) bx = sx + sw - cw;
    html_view_fill_rect(bx, sy, cw, sh, 0x2f, 0x6f, 0xe0, 150);
    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}

/* Set backlight from a tap/drag x within the brightness bar [bx, bx+bw). */
static void set_bright_x(int x, int bx, int bw)
{
    if (bw <= 0) return;
    int f = (x - bx) * 100 / bw; if (f < 3) f = 3; if (f > 100) f = 100;
    int m = backlight_max(); if (m <= 0) m = 255;
    backlight_set(f * m / 100);
}

int main(void)
{
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    scan_pages();
    if (g_npages == 0) { fprintf(stderr, "no pages in %s\n", UI_DIR); return 1; }

    drm_disp_t disp;
    if (drm_disp_init(&disp) != 0) { fprintf(stderr, "drm init failed\n"); return 1; }
    html_view_init(disp.fb, disp.width, disp.height, disp.pitch_px, 1, UI_FONT);
    html_view_set_uidir(UI_DIR);

    touch_input_t touch; touch_input_init(&touch, disp.width, disp.height);
    key_input_t key;     key_input_init(&key);
    backlight_init();

    int menu = 0, prev_press = 0, down_x = 0, down_y = 0;
    int dragging = 0, drag_dir = 0, drag_target = 0;
    int scroll_dir = 0, scroll_start = 0;
    int sliding = 0, bar_x = 0, bar_w = 0;   /* brightness slider drag */
    int segging = 0, seg_x = 0, seg_y = 0, seg_w = 0, seg_h = 0;   /* segmented control drag */
    int seg_which = 0, seg_n = 4;   /* 1 = net mode (4 cells), 2 = auto-off (6) */
    const int W = disp.width, H = disp.height;
    char menu_path[300];
    snprintf(menu_path, sizeof menu_path, "%s/menu.html", UI_DIR);

    #define CUR_PATH (menu ? menu_path : g_pages[g_cur])

    render(&disp, CUR_PATH);
    uint32_t last_data = millis(), last_act = millis(), last_wake_tap = 0;

    while (g_run) {
        uint32_t now = millis();
        int need_render = 0, animating = 0;

        /* power key */
        int ev = key_input_poll(&key, now);
        if (ev == KEY_EV_SHORT || ev == KEY_EV_LONG) last_act = now;
        if (ev == KEY_EV_SHORT) {
            backlight_toggle();
            if (backlight_is_on()) need_render = 1;
            else {   /* blank to black so nothing is visible under external light */
                memset(disp.fb, 0, (size_t)disp.pitch_px * disp.height * sizeof(uint16_t));
                drm_disp_dirty(&disp, 0, 0, disp.width - 1, disp.height - 1);
            }
        } else if (ev == KEY_EV_LONG) { backlight_on(); menu = !menu; need_render = 1; }

        /* touch: follow-finger swipe (pages) / drag (scroll) + tap actions */
        int x, y, pressed;
        touch_input_read(&touch, &x, &y, &pressed);
        if (pressed) last_act = now;

        if (!backlight_is_on()) {
            /* double-tap wakes the screen (if enabled); gesture is consumed */
            if (pressed && !prev_press) {
                if (g_dtap_wake && now - last_wake_tap < 400) { backlight_on(); need_render = 1; }
                last_wake_tap = now;
            }
        } else if (g_modal) {
            /* second-level band-lock dialog: tap only (level-1 is disabled) */
            if (!pressed && prev_press) {
                const char *act = html_view_click((float)x, (float)y);
                char *sel = g_modal == 1 ? g_sel_sa : g_modal == 2 ? g_sel_nsa : g_sel_lte;
                const char *uni = g_modal == 1 ? g_uni_sa : g_modal == 2 ? g_uni_nsa : g_uni_lte;
                if (!strncmp(act, "act:", 4)) {
                    const char *a = act + 4;
                    if (!strncmp(a, "bsa:", 4) || !strncmp(a, "bnsa:", 5) || !strncmp(a, "blte:", 5)) {
                        band_toggle(sel, 256, strchr(a, ':') + 1); render_modal_overlay(&disp); animating = 1;
                    } else if (!strcmp(a, "mall")) {
                        if (!strcmp(sel, uni)) sel[0] = 0; else snprintf(sel, 256, "%s", uni);
                        render_modal_overlay(&disp); animating = 1;
                    } else if (!strcmp(a, "minv")) {
                        char u[256]; snprintf(u, sizeof u, "%s", uni); char out[256]; int o = 0;
                        for (char *tk = strtok(u, ","); tk; tk = strtok(NULL, ","))
                            if (!band_in(sel, tk)) o += snprintf(out + o, sizeof out - o, "%s%s", o ? "," : "", tk);
                        out[o] = 0; snprintf(sel, 256, "%s", out); render_modal_overlay(&disp); animating = 1;
                    } else if (!strcmp(a, "mapply")) {
                        if (sel[0]) { char cmd[360];
                            if (g_modal == 3)
                                snprintf(cmd, sizeof cmd, "ubus call zte_nwinfo_api nwinfo_set_lte_ext_band '{\"lte_band\":\"%s\"}' >/dev/null 2>&1 &", sel);
                            else
                                snprintf(cmd, sizeof cmd, "ubus call zte_nwinfo_api nwinfo_set_nrbandlock '{\"nr5g_type\":\"%s\",\"nr5g_band\":\"%s\"}' >/dev/null 2>&1 &", g_modal == 1 ? "0" : "1", sel);
                            system(cmd);
                        }
                        snprintf(g_toast, sizeof g_toast, "锁频成功"); g_toast_until = now + 1600;
                        g_modal = 0; need_render = 1;
                    }
                } else { g_modal = 0; need_render = 1; }   /* tap outside closes */
            }
        } else {
            int maxs = g_page_h > H ? g_page_h - H : 0;
            if (pressed && !prev_press) {
                down_x = x; down_y = y; dragging = 1; drag_dir = 0; scroll_dir = 0; scroll_start = g_scroll; sliding = 0; segging = 0;
                int bx, by, bw, bh;   /* grab the brightness slider / segmented control if pressed on it */
                if (html_view_rect("#bright-bar", &bx, &by, &bw, &bh) &&
                    x >= bx && x < bx + bw && y >= by - 5 && y < by + bh + 5) {
                    sliding = 1; bar_x = bx; bar_w = bw;
                    set_bright_x(x, bx, bw); render(&disp, CUR_PATH); animating = 1;
                } else {   /* segmented controls: net mode (#netseg) or auto-off (#autoseg) */
                    int which = 0, n = 4;
                    if (html_view_rect("#netseg", &bx, &by, &bw, &bh) &&
                        x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 1; n = 4; }
                    else if (html_view_rect("#autoseg", &bx, &by, &bw, &bh) &&
                             x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 2; n = 6; }
                    if (which) {
                        segging = 1; seg_which = which; seg_n = n;
                        seg_x = bx; seg_y = by; seg_w = bw; seg_h = bh;
                        g_segdrag = which;
                        render(&disp, CUR_PATH);          /* plain seg (no cell highlight) */
                        capture_fb(&disp);
                        seg_box(&disp, seg_x, seg_y, seg_w, seg_h, seg_n, x);
                        animating = 1;
                    }
                }
            }
            else if (pressed && dragging && !menu) {
                if (sliding) { set_bright_x(x, bar_x, bar_w); render(&disp, CUR_PATH); animating = 1; }
                else if (segging) { seg_box(&disp, seg_x, seg_y, seg_w, seg_h, seg_n, x); animating = 1; }
                else {
                int dx = x - down_x, dy = y - down_y;
                int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                if (drag_dir == 0 && scroll_dir == 0) {
                    if (g_npages > 1 && adx > 14 && adx > ady) {
                        drag_dir = dx < 0 ? 1 : -1;
                        drag_target = (g_cur + (drag_dir > 0 ? 1 : g_npages - 1)) % g_npages;
                        prep_pair(drag_target, drag_dir);
                    } else if (ady > 14 && ady >= adx && maxs > 0) {
                        scroll_dir = 1; scroll_start = g_scroll;
                        int bufh = g_page_h > SCROLLMAX ? SCROLLMAX : g_page_h;  /* prerender once */
                        const char *hp = page_html(CUR_PATH);
                        g_scroll_h = hp ? html_view_render_tall(g_scrollbuf, hp, bufh) : g_page_h;
                    } else if (ady > 24) dragging = 0;
                }
                if (drag_dir != 0) {
                    compose_frame(&disp, drag_dir > 0 ? -dx : (W - dx));
                    animating = 1;
                } else if (scroll_dir != 0) {
                    int ns = scroll_start - dy;
                    if (ns < 0) ns = 0; if (ns > maxs) ns = maxs;
                    if (ns != g_scroll) { g_scroll = ns; scroll_blit(&disp, g_scroll); }
                    animating = 1;
                }
                }
            }
            else if (!pressed && prev_press) {
                int dx = x - down_x;
                if (sliding) { /* brightness drag finished */ }
                else if (segging) {   /* snap to nearest cell + apply */
                    int c = clampi((x - seg_x) * seg_n / (seg_w > 0 ? seg_w : 1), 0, seg_n - 1);
                    if (seg_which == 1) {
                        char cmd[160];
                        snprintf(cmd, sizeof cmd,
                            "ubus call zte_nwinfo_api nwinfo_set_netselect '{\"net_select\":\"%s\"}' >/dev/null 2>&1 &", g_netmodes[c].v);
                        system(cmd);
                        snprintf(g_net_pending, sizeof g_net_pending, "%s", g_netmodes[c].v);
                    } else {
                        g_autooff_ms = g_autooffs[c].ms; last_act = now;
                    }
                    g_segdrag = 0; need_render = 1;
                }
                else if (dragging && drag_dir != 0) {            /* finish or snap back */
                    int adx = dx < 0 ? -dx : dx;
                    int commit = adx > W * 30 / 100;
                    int o_now = drag_dir > 0 ? -dx : (W - dx);
                    if (o_now < 0) o_now = 0; if (o_now > W) o_now = W;
                    anim_o(&disp, o_now, drag_dir > 0 ? (commit ? W : 0) : (commit ? 0 : W));
                    if (commit) { g_cur = drag_target; g_scroll = 0; }
                    need_render = 1;
                } else if (dragging && scroll_dir != 0) {   /* scroll settled */
                    need_render = 1;
                } else if (dragging) {                      /* tap -> action */
                    const char *act = html_view_click((float)x, (float)y);
                    if (!strncmp(act, "act:", 4)) {
                        const char *a = act + 4;
                        if      (!strcmp(a, "poweroff")) system("poweroff");
                        else if (!strcmp(a, "reboot"))   system("reboot");
                        else if (!strcmp(a, "close"))     { menu = 0; need_render = 1; }
                        else if (!strcmp(a, "menu"))      { backlight_on(); menu = 1; need_render = 1; }
                        else if (!strcmp(a, "theme"))     { g_theme = !g_theme; need_render = 1; }
                        else if (!strcmp(a, "revealkey")) { g_show_key = !g_show_key; need_render = 1; }
                        else if (!strcmp(a, "revealcell")) { g_show_cellid = !g_show_cellid; need_render = 1; }
                        else if (!strcmp(a, "revealimei")) { g_show_imei = !g_show_imei; need_render = 1; }
                        else if (!strcmp(a, "spunit"))    { g_speed_bits = !g_speed_bits; need_render = 1; }
                        else if (!strcmp(a, "nfc")) {
                            devui_data_t dd; char cmd[120];
                            int on = (data_refresh(&dd) && dd.nfc_switch) ? 0 : 1;
                            snprintf(cmd, sizeof cmd,
                                "ubus call zwrt_nfc zwrt_nfc_wifi_set '{\"switch\":%d,\"flag\":2}'", on);
                            system(cmd);
                            need_render = 1;
                        }
                        else if (!strcmp(a, "wifi")) {
                            devui_data_t dd; int dis = (data_refresh(&dd) && dd.wifi_enabled) ? 1 : 0;
                            char cmd[256];
                            snprintf(cmd, sizeof cmd,
                                "(uci set wireless.main_2g.disabled=%d; uci set wireless.main_5g.disabled=%d; "
                                "uci commit wireless; wifi reload) >/dev/null 2>&1 &", dis, dis);
                            system(cmd);
                            need_render = 1;
                        }
                        else if (!strcmp(a, "adb")) {
                            devui_data_t dd;
                            const char *mode = "debug";
                            if (data_refresh(&dd) && !strcmp(dd.usb_mode, "debug")) mode = "user";
                            char cmd[96];
                            snprintf(cmd, sizeof cmd, "ubus call zwrt_bsp.usb set '{\"mode\":\"%s\"}'", mode);
                            system(cmd);
                            need_render = 1;
                        }
                        else if (!strcmp(a, "bright")) {    /* tap position on the bar = level */
                            int bx, by, bw, bh;
                            if (html_view_rect("#bright-bar", &bx, &by, &bw, &bh) && bw > 0) {
                                int frac = (x - bx) * 100 / bw;
                                if (frac < 3) frac = 3; if (frac > 100) frac = 100;
                                int bmax = backlight_max(); if (bmax <= 0) bmax = 255;
                                backlight_set(frac * bmax / 100);
                            }
                            need_render = 1;
                        }
                        else if (!strncmp(a, "autooff:", 8)) {   /* preset select */
                            g_autooff_ms = atoi(a + 8); last_act = now; need_render = 1;
                        }
                        else if (!strcmp(a, "dtap")) { g_dtap_wake = !g_dtap_wake; need_render = 1; }
                        else if (!strncmp(a, "net:", 4)) {   /* segmented control tap fallback */
                            char cmd[160];
                            snprintf(cmd, sizeof cmd,
                                "ubus call zte_nwinfo_api nwinfo_set_netselect '{\"net_select\":\"%s\"}' >/dev/null 2>&1 &", a + 4);
                            system(cmd);
                            snprintf(g_net_pending, sizeof g_net_pending, "%s", a + 4);
                            need_render = 1;
                        }
                        else if (!strncmp(a, "openmodal:", 10)) {   /* open band-lock dialog */
                            const char *r = a + 10;
                            g_modal = !strcmp(r, "sa") ? 1 : !strcmp(r, "nsa") ? 2 : 3;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "resetband")) {
                            system("ubus call zte_nwinfo_api nwinfo_reset_band_cell_setting '{}' >/dev/null 2>&1 &");
                            snprintf(g_toast, sizeof g_toast, "锁频已恢复默认"); g_toast_until = now + 1600;
                            need_render = 1;   /* selection re-syncs from the live lock automatically */
                        }
                    }
                }
                dragging = 0; drag_dir = 0; scroll_dir = 0; sliding = 0; segging = 0; g_segdrag = 0;
            }
        }
        prev_press = pressed;

        /* dismiss the toast after its timeout */
        if (g_toast[0] && now >= g_toast_until) { g_toast[0] = 0; need_render = 1; }

        /* auto screen-off after the configured idle timeout */
        if (g_autooff_ms > 0 && backlight_is_on() && now - last_act >= (uint32_t)g_autooff_ms) {
            backlight_off();
            memset(disp.fb, 0, (size_t)disp.pitch_px * disp.height * sizeof(uint16_t));
            drm_disp_dirty(&disp, 0, 0, disp.width - 1, disp.height - 1);
        }

        /* periodic data refresh (not mid-drag). While charging, refresh faster so
         * the battery charge sweep animates. */
        uint32_t refresh_ms = g_charging ? 220 : 1000;
        if (!dragging && now - last_data >= refresh_ms) { need_render = 1; last_data = now; }

        if (need_render && backlight_is_on()) render(&disp, CUR_PATH);
        if (!animating) usleep(30000);
    }

    drm_disp_close(&disp);
    return 0;
}
