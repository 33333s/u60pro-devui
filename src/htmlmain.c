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
static int  g_speed_bits = 1; /* 1 = Mbps (bit rate), 0 = MB/s (byte rate) */
static int  g_charging;   /* set from snapshot; drives charge animation cadence */
static unsigned g_phase;  /* animation tick (battery charge sweep) */

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
    static char out[16384];
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

    /* ---- per-carrier rows + generation badge ----
     * CA group fields: idx,pci,?,band,arfcn,bw,?,rsrp,rsrq,sinr,rssi */
    static char s_nrrows[1500], s_lterows[1700], s_cacc[8], s_cabw[8], s_gen[8], s_lteshow[20];
    int is_nr = strstr(d.net_type, "SA") || strstr(d.net_type, "NSA") || strstr(d.net_type, "NR");
    int sa_only = strstr(d.net_type, "SA") && !strstr(d.net_type, "NSA");
    int nr_cc = 0, nr_bw = 0, no = 0, lo = 0;
    char rp[12], pc[12], ac[16];

    s_nrrows[0] = 0;
    if (is_nr && d.band[0]) {                       /* NR PCell */
        snprintf(rp, sizeof rp, "%d", d.nr_rsrp); snprintf(pc, sizeof pc, "%d", d.nr_pci);
        snprintf(ac, sizeof ac, "%ld", d.nr_channel);
        no = car_row(s_nrrows, no, sizeof s_nrrows, d.band, d.nr_bw, ac, pc, rp, d.nr_snr);
        nr_cc = 1; nr_bw = atoi(d.nr_bw);
    }
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

    /* LTE: only meaningful when an LTE anchor exists (NSA / LTE / 4G), never on
     * pure NR-SA, where the modem leaves stale lte_rsrp around. */
    s_lterows[0] = 0; int lte_cc = 0;
    if (!sa_only && d.lte_rsrp < 0 && d.lte_rsrp > -140) {   /* LTE PCell */
        snprintf(rp, sizeof rp, "%d", d.lte_rsrp);
        lo = car_row(s_lterows, lo, sizeof s_lterows, "LTE", "-", "-", "-", rp, d.lte_snr);
        lte_cc = 1;
    }
    if (!sa_only) {
        char lteca[256]; strncpy(lteca, d.lteca, sizeof lteca - 1); lteca[sizeof lteca - 1] = 0;
        for (char *grp = strtok(lteca, ";"); grp; grp = strtok(NULL, ";")) {
            char g[96]; strncpy(g, grp, sizeof g - 1); g[sizeof g - 1] = 0;
            char *f[12]; int nf = ca_split(g, f, 12);
            if (nf > 5 && atoi(f[5]) > 0) {
                char bn[12]; snprintf(bn, sizeof bn, "B%s", f[3]);
                lo = car_row(s_lterows, lo, sizeof s_lterows, bn, f[5], nf > 4 ? f[4] : "-",
                             nf > 1 ? f[1] : "-", nf > 7 ? f[7] : "-", nf > 9 ? f[9] : "-");
                lte_cc++;
            }
        }
    }
    snprintf(s_lteshow, sizeof s_lteshow, "%s", lte_cc ? "" : "display:none");
    snprintf(s_cacc, sizeof s_cacc, "%d", nr_cc);
    snprintf(s_cabw, sizeof s_cabw, "%d", nr_bw);

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
    if (sa_only) {
        if ((op == 1 || op == 4) && nr_cc >= 3) gen = "5GA";
        else if ((op == 2 || op == 3) && nr_bw >= 200) gen = "5GA";
        else if (nr_bw > 100) gen = "5G+";
        else gen = "5G";
    } else if (is_nr) gen = "5G";
    else if (strstr(d.net_type, "LTE") || strstr(d.net_type, "4G")) gen = (d.mcc == 460) ? "4G" : "LTE";
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
        char sp[40], glow[80] = "";
        fmt_speed_pair(sp, sizeof sp, d.tx_speed, d.rx_speed, g_speed_bits);
        if (g_charging) {
            int gp = (g_phase % 12) * 9;        /* 0..99 sweep */
            snprintf(glow, sizeof glow, "<span class='glow' style='left:%d%%'></span>", gp);
        }
        snprintf(s_sbar, sizeof s_sbar,
            "<div class='sbar'><span class='clk'>%s</span><span class='r'>"
            "<span class='spd'>%s</span>"
            "<span class='gt %s'>%s</span>"
            "%s"
            "<span class='bw'><span class='batt %s'><span class='bf' style='width:%d%%'></span>%s</span><span class='tip'></span></span>"
            "<span class='bp'>%d%%</span>"
            "</span></div>",
            s_time, sp, genc, gen, s_sig,
            d.bat_percent <= 20 ? "low" : "", clampi(d.bat_percent, 0, 100), glow,
            d.bat_percent);
    }

    /* ---- bottom page dots ---- */
    {
        int o = snprintf(s_dots, sizeof s_dots, "<div class='dots'>");
        for (int p = 0; p < g_npages; p++)
            o += snprintf(s_dots + o, sizeof s_dots - o, "<span class='dot%s'></span>", p == g_cur ? " on" : "");
        snprintf(s_dots + o, sizeof s_dots - o, "</div>");
    }

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
    t[i++] = (struct kv){ "CA_CC", s_cacc };        t[i++] = (struct kv){ "CA_BW", s_cabw };
    t[i++] = (struct kv){ "NR_ROWS", s_nrrows };    t[i++] = (struct kv){ "LTE_ROWS", s_lterows };
    t[i++] = (struct kv){ "LTE_SHOW", s_lteshow };  t[i++] = (struct kv){ "GEN", s_gen };
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
    return i;
}

/* Build the data-filled HTML for a page (returns a static buffer). */
static const char *page_html(const char *path)
{
    char *tmpl = read_file(path);   /* re-read each time = live reload */
    if (!tmpl) return NULL;
    struct kv t[64];
    int n = build_kv(t);
    char *html = apply_template(tmpl, t, n);
    free(tmpl);
    return html;
}

static void render(drm_disp_t *disp, const char *path)
{
    const char *html = page_html(path);
    if (!html) return;
    html_view_render_html(html);
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

/* Render the page pair for a drag direction into A(left)/B(right). dir>0 = next. */
static void prep_pair(int target, int dir)
{
    const char *h;
    if (dir > 0) {   /* next: left=current, right=target */
        h = page_html(g_pages[g_cur]);  if (h) html_view_render_to(g_bufA, h);
        h = page_html(g_pages[target]); if (h) html_view_render_to(g_bufB, h);
    } else {         /* prev: left=target, right=current */
        h = page_html(g_pages[target]); if (h) html_view_render_to(g_bufA, h);
        h = page_html(g_pages[g_cur]);  if (h) html_view_render_to(g_bufB, h);
    }
}

/* Settle the offset from o0 to o1 over a few frames. */
static void anim_o(drm_disp_t *d, int o0, int o1)
{
    const int FR = 5;
    for (int f = 1; f <= FR; f++) compose_frame(d, o0 + (o1 - o0) * f / FR);
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
    const int W = disp.width;
    char menu_path[300];
    snprintf(menu_path, sizeof menu_path, "%s/menu.html", UI_DIR);

    #define CUR_PATH (menu ? menu_path : g_pages[g_cur])

    render(&disp, CUR_PATH);
    uint32_t last_data = millis();

    while (g_run) {
        uint32_t now = millis();
        int need_render = 0, animating = 0;

        /* power key */
        int ev = key_input_poll(&key, now);
        if (ev == KEY_EV_SHORT) {
            backlight_toggle();
            if (backlight_is_on()) need_render = 1;
            else {   /* blank to black so nothing is visible under external light */
                memset(disp.fb, 0, (size_t)disp.pitch_px * disp.height * sizeof(uint16_t));
                drm_disp_dirty(&disp, 0, 0, disp.width - 1, disp.height - 1);
            }
        } else if (ev == KEY_EV_LONG) { backlight_on(); menu = !menu; need_render = 1; }

        /* touch: follow-finger page swipe + tap actions */
        int x, y, pressed;
        touch_input_read(&touch, &x, &y, &pressed);
        if (backlight_is_on()) {
            if (pressed && !prev_press) { down_x = x; down_y = y; dragging = 1; drag_dir = 0; }
            else if (pressed && dragging && !menu) {
                int dx = x - down_x, dy = y - down_y;
                int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                if (drag_dir == 0) {
                    if (g_npages > 1 && adx > 14 && adx > ady) {
                        drag_dir = dx < 0 ? 1 : -1;
                        drag_target = (g_cur + (drag_dir > 0 ? 1 : g_npages - 1)) % g_npages;
                        prep_pair(drag_target, drag_dir);
                    } else if (ady > 20) dragging = 0;
                }
                if (drag_dir != 0) {
                    compose_frame(&disp, drag_dir > 0 ? -dx : (W - dx));
                    animating = 1;
                }
            }
            else if (!pressed && prev_press) {
                int dx = x - down_x;
                if (dragging && drag_dir != 0) {            /* finish or snap back */
                    int adx = dx < 0 ? -dx : dx;
                    int commit = adx > W * 30 / 100;
                    int o_now = drag_dir > 0 ? -dx : (W - dx);
                    if (o_now < 0) o_now = 0; if (o_now > W) o_now = W;
                    anim_o(&disp, o_now, drag_dir > 0 ? (commit ? W : 0) : (commit ? 0 : W));
                    if (commit) g_cur = drag_target;
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
                        else if (!strcmp(a, "spunit"))    { g_speed_bits = !g_speed_bits; need_render = 1; }
                        else if (!strcmp(a, "adb")) {
                            devui_data_t dd;
                            const char *mode = "debug";
                            if (data_refresh(&dd) && !strcmp(dd.usb_mode, "debug")) mode = "user";
                            char cmd[96];
                            snprintf(cmd, sizeof cmd, "ubus call zwrt_bsp.usb set '{\"mode\":\"%s\"}'", mode);
                            system(cmd);
                            need_render = 1;
                        }
                    }
                }
                dragging = 0; drag_dir = 0;
            }
        }
        prev_press = pressed;

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
