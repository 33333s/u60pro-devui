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
#include "devui_ext.h"

#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <sys/stat.h>
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
extern void        html_view_fill_poly(const int *xs, const int *ys, int n, int r, int g, int b, int a);
extern void        html_view_fill_round_rect(int x, int y, int w, int h, int rad, int r, int g, int b, int a);
extern int         html_view_text_width_px(const char *text, int size);
extern void        html_view_text_bounds_px(const char *text, int size, int *x0, int *y0, int *x1, int *y1);
extern void        html_view_draw_text_px(int x, int y, const char *text, int size, int bold,
                                          int r, int g, int b, int a);
extern void        html_view_draw_text_contrast_px(int x, int y, const char *text, int size, int bold,
                                                   int dr, int dg, int db, int lr, int lg, int lb, int a);
extern int         html_view_render_tall(uint16_t *buf, const char *html, int bufh);
extern int         html_view_render_overlay(const char *html);
static void        maybe_dump_fb(drm_disp_t *d);

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

#define TEMPLATE_CACHE_CAP 8
#define PAGE_HTML_CACHE_CAP 1048576

struct template_cache_entry {
    char path[320];
    long long mtime_ns;
    char *content;
    int used;
};

static struct template_cache_entry g_tmpl_cache[TEMPLATE_CACHE_CAP];
static int g_tmpl_next;

/* ---- pages / theme ---- */
static char g_pages[24][288];
static int  g_npages, g_cur;
static int  g_theme;      /* 0 = dark, 1 = light */
static int  g_show_key;   /* reveal WiFi password (default hidden) */
static int  g_show_cellid; /* reveal NR Cell ID (default hidden) */
static int  g_show_imei;   /* reveal IMEI (default hidden) */
static int  g_speed_bits = 1; /* 1 = Mbps (bit rate), 0 = MB/s (byte rate) */
static int  g_show_batpct = 1; /* show percent text inside status battery */
static int  g_charging;   /* set from snapshot; drives charge animation cadence */
static int  g_stat_bat, g_stat_sig, g_stat_lowbat;
static char g_stat_time[8], g_stat_speed[40], g_stat_gen[8];
static unsigned g_phase;  /* animation tick (battery charge sweep) */
static int  g_charge_boot; /* power-off charging boot: full-screen charging UI */
static int  g_scroll;     /* current page vertical scroll offset */
static int  g_page_h;     /* last rendered page content height */
static int  g_autooff_ms; /* auto screen-off timeout, 0 = never */
static int  g_refresh_ms = 1000; /* state refresh interval, 0 = paused */
static int  g_saved_bright = -1; /* persisted backlight level, -1 = none yet */
static int g_last_clock_min = -1;       /* HH:MM changes only once per minute */
static char g_render_html_cache[PAGE_HTML_CACHE_CAP];
static size_t g_render_html_cache_len;
static char g_render_html_cache_path[320];
static int g_render_html_cache_scroll;
static int g_render_html_cache_modal;
static long g_render_html_cache_sms_open;
static int g_render_html_cache_lock;
static long long g_render_html_cache_css_mtime = -1;
static int normalize_refresh_ms(int ms);

/* ---- page-2 aux state, cached (-1 = unknown). Like the reference plugin,
 * bands are controlled purely with `ifconfig wlanN up/down` and read back from
 * operstate (wlan0=main 2.4G, wlan2=main 5G); uci/`wifi reload`/`zwrt_wlan
 * reload` are NOT used (they don't work / wedge the radios). PSM = `iw
 * power_save`, persisted via a self-written /etc/hotplug.d/iface/99-disable-powersave
 * script that re-applies the chosen mode on every ifup (so power_save=on sticks
 * across reconnect/reboot). The DHCP pool is computed live from uci. Toggles flip
 * optimistically; a throttle reconciles. */
static int  g_w24 = -1, g_w5 = -1, g_wpsm = -1;
static int  g_dps = -1;   /* power direct-supply mode (zwrt_bsp.charger), -1 unknown */
static int  g_adb_pending = -1; /* optimistic ADB state while USB re-enumerates */
static int  g_usb_net = -1;     /* Type-C network sharing composition active */
static int  g_typec_source = -1; /* 1 = reverse charge peer, 0 = charge U60 */
static int  g_typec_attached = -1;
static int  g_usb_net_pending = -1;
static int  g_typec_pending = -1;
static uint32_t g_usb_net_until, g_typec_until;
static char g_dhcp_pool[48];
/* currently WiFi-associated client MACs (lowercased, comma-joined). DHCP leases
 * linger after a device leaves, so the device list filters against this (live
 * station dump) to match the vendor's "only connected" behavior. */
static char g_assoc_macs[640];
static uint32_t g_wifi_aux_at;

/* Is this MAC currently associated to a radio? (case-insensitive substring) */
static int mac_assoc(const char *mac)
{
    if (!g_assoc_macs[0] || !mac || !mac[0]) return 1;   /* unknown -> don't hide */
    char lc[24]; int j = 0;
    for (const char *p = mac; *p && j < 23; p++) {
        char c = *p; if (c >= 'A' && c <= 'Z') c += 32; lc[j++] = c;
    }
    lc[j] = 0;
    return strstr(g_assoc_macs, lc) != NULL;
}

static int read_long_path(const char *path, long *out)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    long v;
    int ok = fscanf(fp, "%ld", &v) == 1;
    fclose(fp);
    if (!ok) return 0;
    *out = v;
    return 1;
}

static int read_line_path(const char *path, char *out, size_t cap)
{
    FILE *fp = fopen(path, "r");
    size_t n;
    if (!fp || cap == 0) { if (fp) fclose(fp); return 0; }
    if (!fgets(out, (int)cap, fp)) { fclose(fp); return 0; }
    fclose(fp);
    n = strcspn(out, "\r\n");
    out[n] = 0;
    return 1;
}

static int boot_is_nonsilent(void)
{
    char cmdline[2048];
    FILE *fp = fopen("/proc/cmdline", "r");
    if (!fp) return 0;
    size_t n = fread(cmdline, 1, sizeof cmdline - 1, fp);
    fclose(fp);
    cmdline[n] = 0;
    return strstr(cmdline, "silent_boot.mode=nonsilent") != NULL;
}

static int read_mode_main_state(char *out, size_t cap)
{
    FILE *fp = fopen("/etc/config/zwrt_zte_mc_tmp", "r");
    char line[256];
    if (!fp || cap == 0) { if (fp) fclose(fp); return 0; }
    while (fgets(line, sizeof line, fp)) {
        if (!strstr(line, "option mode_main_state")) continue;
        char *q = strchr(line, '\'');
        if (!q) continue;
        char *e = strchr(q + 1, '\'');
        if (!e) continue;
        size_t n = (size_t)(e - (q + 1));
        if (n >= cap) n = cap - 1;
        memcpy(out, q + 1, n);
        out[n] = 0;
        fclose(fp);
        return 1;
    }
    fclose(fp);
    return 0;
}

static int boot_is_charge_mode(void)
{
    char mode[64];
    if (read_mode_main_state(mode, sizeof mode)) {
        if (!strncmp(mode, "mode_power_off_", 15)) return 1;
        if (!strcmp(mode, "mode_power_on") || !strcmp(mode, "mode_power_on_charger")) return 0;
    }
    return !boot_is_nonsilent();
}

static int boot_has_external_power(void)
{
    long v;
    char st[32];
    if (read_long_path("/sys/class/power_supply/usb/online", &v) && v != 0) return 1;
    if (read_long_path("/sys/class/power_supply/charger_zte/present_mbb", &v) && v != 0) return 1;
    if (read_long_path("/sys/class/power_supply/type-c_zte/present_mbb", &v) && v != 0) return 1;
    if (read_line_path("/sys/class/power_supply/battery/status", st, sizeof st)) {
        if (!strcmp(st, "Charging") || !strcmp(st, "Full")) return 1;
    }
    if (read_long_path("/sys/class/power_supply/charger_zte/status_mbb", &v) && v == 1) return 1;
    if (read_long_path("/sys/class/power_supply/battery_zte/status_mbb", &v) && v == 1) return 1;
    if (read_long_path("/sys/class/power_supply/statistics_zte/batt_status", &v) && v == 1) return 1;
    return 0;
}

/* ---- screen lock (4-digit PIN) ---- */
static char g_pin[8];        /* stored PIN ("" = lock disabled) */
static int  g_lock_state;    /* 0 = normal, 1 = unlock pad, 2 = setup pad */
static char g_pin_entry[8];  /* digits typed so far */
static int  g_lock_err;      /* 1 = show the unlock-pad error message */

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
            strcmp(de->d_name, "menu.html") != 0 &&
            strcmp(de->d_name, "lockscreen.html") != 0) {
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
    if (d > 0)       snprintf(o, n, "%ldd %ldh", d, hh);
    else if (hh > 0) snprintf(o, n, "%ldh %ldm", hh, mm);
    else             snprintf(o, n, "%ldm", mm);
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
/* Single speed value with unit, honoring the bit/byte-rate setting (matches the
 * status-bar units): bits=1 -> Mbps/Kbps/bps, bits=0 -> MB/s/KB/s/B/s. */
static void fmt_speed_u(char *o, size_t n, long bps, int bits)
{
    double v = bps * (bits ? 8.0 : 1.0);
    const char *unit; double div;
    if (bits) { if (v >= 1e6) { unit = "Mbps"; div = 1e6; } else if (v >= 1e3) { unit = "Kbps"; div = 1e3; } else { unit = "bps"; div = 1; } }
    else      { if (v >= 1e6) { unit = "MB/s"; div = 1e6; } else if (v >= 1e3) { unit = "KB/s"; div = 1e3; } else { unit = "B/s"; div = 1; } }
    double val = v / div;
    if (val >= 10) snprintf(o, n, "%.0f %s", val, unit);
    else           snprintf(o, n, "%.1f %s", val, unit);
}

/* Status-bar speed pair: "up/down <unit>", shared unit picked from the
 * larger of the two. bits=1 -> Mbps/Kbps/bps; bits=0 -> MB/s/KB/s/B/s. */
static void fmt_speed_pair(char *buf, size_t cap, long up, long down, int bits) {
    double mul = bits ? 8.0 : 1.0;
    double u = up * mul, d = down * mul, mx = u > d ? u : d;
    const char *unit; double div;
    if (bits) { if (mx >= 1e6) { unit = "Mbps"; div = 1e6; } else if (mx >= 1e3) { unit = "Kbps"; div = 1e3; } else { unit = "bps"; div = 1; } }
    else      { if (mx >= 1e6) { unit = "MB/s"; div = 1e6; } else if (mx >= 1e3) { unit = "KB/s"; div = 1e3; } else { unit = "B/s"; div = 1; } }
    char us[12], ds[12];
    fmt_one(us, sizeof us, u / div); fmt_one(ds, sizeof ds, d / div);
    snprintf(buf, cap, "\xe2\x86\x91%s \xe2\x86\x93%s %s", us, ds, unit);
}

/* ---- {{key}} template substitution ---- */
struct kv { const char *k; const char *v; };

static char *apply_template(const char *tmpl, struct kv *t, int n)
{
    static char out[1048576];
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
                else {
                    size_t room = sizeof(out) - 1 - o;
                    if (room) memcpy(out + o, v, room);
                    o = sizeof(out) - 1;
                    out[o] = 0;
                    break;
                }
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
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(fp); return NULL; }
    for (;;) {
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char *nbuf = realloc(buf, ncap);
            if (!nbuf) { free(buf); fclose(fp); return NULL; }
            buf = nbuf; cap = ncap;
        }
        size_t r = fread(buf + n, 1, cap - n - 1, fp);
        n += r;
        if (r == 0) break;
    }
    if (ferror(fp)) { free(buf); fclose(fp); return NULL; }
    buf[n] = 0; fclose(fp);
    return buf;
}

static long long file_mtime_ns(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long long)st.st_mtime * 1000000000LL + (long long)st.st_mtim.tv_nsec;
}

static const char *read_template_cached(const char *path)
{
    long long mtime = file_mtime_ns(path);
    if (mtime < 0) return NULL;

    for (int i = 0; i < TEMPLATE_CACHE_CAP; i++) {
        struct template_cache_entry *e = &g_tmpl_cache[i];
        if (!e->used || !e->path[0]) continue;
        if (strcmp(e->path, path) != 0) continue;

        if (e->mtime_ns == mtime && e->content) return e->content;

        char *next = read_file(path);
        if (!next) return e->content;   /* keep previous cached copy if read fails */
        free(e->content);
        e->content = next;
        e->mtime_ns = mtime;
        return e->content;
    }

    char *tmpl = read_file(path);
    if (!tmpl) return NULL;

    for (int i = 0; i < TEMPLATE_CACHE_CAP; i++) {
        if (!g_tmpl_cache[i].used) {
            snprintf(g_tmpl_cache[i].path, sizeof(g_tmpl_cache[i].path), "%s", path);
            g_tmpl_cache[i].mtime_ns = mtime;
            g_tmpl_cache[i].content = tmpl;
            g_tmpl_cache[i].used = 1;
            return g_tmpl_cache[i].content;
        }
    }

    struct template_cache_entry *e = &g_tmpl_cache[g_tmpl_next];
    free(e->content);
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->mtime_ns = mtime;
    e->content = tmpl;
    e->used = 1;
    g_tmpl_next = (g_tmpl_next + 1) % TEMPLATE_CACHE_CAP;
    return e->content;
}

static int usb_pid_is(const char *needle)
{
    int ok = 0;
    char *pid = read_file("/sys/kernel/config/usb_gadget/g1/idProduct");
    if (pid) {
        ok = strstr(pid, needle) != NULL;
        free(pid);
    }
    return ok;
}

static int usb_net_has_carrier(void)
{
    int ok = 0;
    char *c = read_file("/sys/class/net/rndis0/carrier");
    if (c) { if (c[0] == '1') ok = 1; free(c); }
    c = read_file("/sys/class/net/ecm0/carrier");
    if (c) { if (c[0] == '1') ok = 1; free(c); }
    return ok;
}

static void usb_net_watchdog_stop(void)
{
    system("pid=$(cat /tmp/u60-usbnet-watchdog.pid 2>/dev/null); "
           "[ -n \"$pid\" ] && kill \"$pid\" 2>/dev/null; "
           "rm -f /tmp/u60-usbnet-watchdog.pid /tmp/u60-usbnet-enabled");
}

static void usb_net_apply_source_async(void)
{
    system("(touch /tmp/u60-typec-source /tmp/u60-usbnet-switching; "
           "date '+%F %T usbnet apply source' >>/tmp/usb.log 2>/dev/null; "
           "ubus call zwrt_bsp.powerbank set '{\"state\":0}'; "
           "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}'; "
           "sleep 1; echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null; "
           "/sbin/usb_composition 90B1 n n y n; "
           "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do "
           "att=$(ubus call zwrt_bsp.typec list 2>/dev/null | grep cc_attch_state | grep -o '[0-9]' | head -1); "
           "pid2=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$att\" = \"1\" ] && { [ \"$pid2\" = \"0x90b1\" ] || [ \"$pid2\" = \"0x90B1\" ]; } && [ \"$e\" = \"1\" ]; then break; fi; "
           "sleep 1; done; "
           "if [ \"$att\" = \"1\" ] && { [ \"$pid2\" = \"0x90b1\" ] || [ \"$pid2\" = \"0x90B1\" ]; } && [ \"$e\" = \"1\" ]; then "
           "ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"source\"}'; "
           "sleep 1; ubus call zwrt_bsp.powerbank set '{\"state\":1}'; "
           "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\"}'; "
           "sleep 1; /sbin/usb_composition 90B1 n n y n; "
           "for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do "
           "r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$r\" = \"1\" ] || [ \"$e\" = \"1\" ]; then break; fi; "
           "sleep 1; done; "
           "else "
           "date '+%F %T usbnet source no ecm carrier, try rndis' >>/tmp/usb.log 2>/dev/null; "
           "rm -f /tmp/u60-typec-source; "
           "/sbin/usb_composition 9057 n n y n; "
           "sleep 10; "
           "r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$r\" != \"1\" ] && [ \"$e\" != \"1\" ]; then "
           "date '+%F %T usbnet rndis no carrier, fallback 90B1' >>/tmp/usb.log 2>/dev/null; "
           "/sbin/usb_composition 90B1 n n y n; fi; "
           "fi; "
           "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
}

static void usb_net_apply_sink_async(void)
{
    system("(touch /tmp/u60-usbnet-switching; rm -f /tmp/u60-typec-source; "
           "date '+%F %T usbnet apply sink' >>/tmp/usb.log 2>/dev/null; "
           "ubus call zwrt_bsp.powerbank set '{\"state\":0}'; "
           "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}'; "
           "sleep 1; echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null; "
           "/sbin/usb_composition 9057 n n y n; "
           "sleep 10; "
           "r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null); "
           "e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null); "
           "if [ \"$r\" != \"1\" ] && [ \"$e\" != \"1\" ]; then "
           "date '+%F %T usbnet fallback 90B1' >>/tmp/usb.log 2>/dev/null; "
           "/sbin/usb_composition 90B1 n n y n; fi; "
           "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
}

static void usb_power_only_apply_async(int source)
{
    if (source) {
        system("(touch /tmp/u60-typec-source /tmp/u60-usbnet-switching; "
               "date '+%F %T usbpower apply source' >>/tmp/usb.log 2>/dev/null; "
               "ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"source\"}'; "
               "sleep 1; ubus call zwrt_bsp.powerbank set '{\"state\":1}'; "
               "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\"}'; "
               "sleep 1; SER=$(cat /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber 2>/dev/null); "
               "sh /sbin/usb/compositions/usb_switch 0x19d2 0x1225 mass_storage \"$SER\"; "
               "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
    } else {
        system("(touch /tmp/u60-usbnet-switching; rm -f /tmp/u60-typec-source; "
               "date '+%F %T usbpower apply sink' >>/tmp/usb.log 2>/dev/null; "
               "ubus call zwrt_bsp.powerbank set '{\"state\":0}'; "
               "sleep 1; ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}'; "
               "sleep 1; SER=$(cat /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber 2>/dev/null); "
               "sh /sbin/usb/compositions/usb_switch 0x19d2 0x1225 mass_storage \"$SER\"; "
               "rm -f /tmp/u60-usbnet-switching) >/tmp/u60-usbnet-switch.log 2>&1 &");
    }
}

static void usb_net_watchdog_start(void)
{
    usb_net_watchdog_stop();
    system("cat >/tmp/u60-usbnet-watchdog.sh <<'EOF'\n"
           "#!/bin/sh\n"
           "touch /tmp/u60-usbnet-enabled\n"
           "echo $$ >/tmp/u60-usbnet-watchdog.pid\n"
           "while [ -f /tmp/u60-usbnet-enabled ]; do\n"
           "  if [ -f /tmp/u60-usbnet-switching ]; then sleep 2; continue; fi\n"
           "  pid=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null)\n"
           "  tc=$(ubus call zwrt_bsp.typec list 2>/dev/null)\n"
           "  att=$(echo \"$tc\" | grep cc_attch_state | grep -o '[0-9]' | head -1)\n"
           "  pr=$(echo \"$tc\" | grep power_role | grep -o 'source\\|sink' | head -1)\n"
           "  r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
           "  e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "  case \"$pid\" in 0x9057|0x90b1|0x90B1) netpid=1;; *) netpid=0;; esac\n"
            "  wantsrc=0; [ -f /tmp/u60-typec-source ] && wantsrc=1\n"
            "  rolebad=0\n"
            "  if [ \"$wantsrc\" = 1 ] && [ \"$pr\" != source ]; then rolebad=1; fi\n"
            "  if [ \"$wantsrc\" != 1 ] && [ \"$pr\" != sink ]; then rolebad=1; fi\n"
            "  if [ \"$att\" = 1 ] && { [ \"$rolebad\" = 1 ] || [ \"$netpid\" != 1 ] || { [ \"$r\" != 1 ] && [ \"$e\" != 1 ]; }; }; then\n"
            "    date '+%F %T usbnet watchdog rearm' >>/tmp/usb.log 2>/dev/null\n"
            "    touch /tmp/u60-usbnet-switching\n"
            "    if [ \"$wantsrc\" = 1 ]; then\n"
            "      ubus call zwrt_bsp.powerbank set '{\"state\":0}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null\n"
            "      /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "      for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do\n"
            "        att=$(ubus call zwrt_bsp.typec list 2>/dev/null | grep cc_attch_state | grep -o '[0-9]' | head -1)\n"
            "        pid2=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null)\n"
            "        e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "        if [ \"$att\" = 1 ] && { [ \"$pid2\" = 0x90b1 ] || [ \"$pid2\" = 0x90B1 ]; } && [ \"$e\" = 1 ]; then break; fi\n"
            "        sleep 1\n"
            "      done\n"
            "      if [ \"$att\" = 1 ] && { [ \"$pid2\" = 0x90b1 ] || [ \"$pid2\" = 0x90B1 ]; } && [ \"$e\" = 1 ]; then\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"source\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      ubus call zwrt_bsp.powerbank set '{\"state\":1}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "      for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do\n"
            "        r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
            "        e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "        if [ \"$r\" = 1 ] || [ \"$e\" = 1 ]; then break; fi\n"
            "        sleep 1\n"
            "      done\n"
            "      else\n"
            "        date '+%F %T usbnet watchdog source no ecm carrier, try rndis' >>/tmp/usb.log 2>/dev/null\n"
            "        rm -f /tmp/u60-typec-source\n"
            "        /sbin/usb_composition 9057 n n y n >>/tmp/usb.log 2>&1\n"
            "        sleep 10\n"
            "        r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
            "        e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "        if [ \"$r\" != 1 ] && [ \"$e\" != 1 ]; then\n"
            "          date '+%F %T usbnet watchdog rndis no carrier, fallback 90B1' >>/tmp/usb.log 2>/dev/null\n"
            "          /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "        fi\n"
            "      fi\n"
            "    else\n"
            "      ubus call zwrt_bsp.powerbank set '{\"state\":0}' >/dev/null 2>&1\n"
            "      ubus call zwrt_bsp.typec set '{\"DR_Swap\":\"device\",\"PR_Swap\":\"sink\"}' >/dev/null 2>&1\n"
            "      sleep 1\n"
            "      echo peripheral > /sys/bus/platform/devices/a600000.ssusb/mode 2>/dev/null\n"
            "      /sbin/usb_composition 9057 n n y n >>/tmp/usb.log 2>&1\n"
            "      sleep 10\n"
            "      r=$(cat /sys/class/net/rndis0/carrier 2>/dev/null)\n"
            "      e=$(cat /sys/class/net/ecm0/carrier 2>/dev/null)\n"
            "      if [ \"$r\" != 1 ] && [ \"$e\" != 1 ]; then\n"
            "        date '+%F %T usbnet watchdog fallback 90B1' >>/tmp/usb.log 2>/dev/null\n"
            "        /sbin/usb_composition 90B1 n n y n >>/tmp/usb.log 2>&1\n"
            "      fi\n"
            "    fi\n"
            "    rm -f /tmp/u60-usbnet-switching\n"
            "  fi\n"
            "  sleep 12\n"
            "done\n"
           "EOF\n"
           "chmod +x /tmp/u60-usbnet-watchdog.sh\n"
           "nohup /tmp/u60-usbnet-watchdog.sh >/tmp/u60-usbnet-watchdog.log 2>&1 &");
}

/* Read page-2 aux state into the cache: band on/off from netdev operstate (the
 * live truth on this firmware), PSM from `iw power_save`, and the DHCP pool
 * range computed live from uci (lan ip + start/limit offsets). One shell
 * round-trip, called on a throttle. */
static void wifi_aux_refresh(void)
{
    FILE *fp = popen(
        "echo W0=$(cat /sys/class/net/wlan0/operstate 2>/dev/null);"
        "echo W2=$(cat /sys/class/net/wlan2/operstate 2>/dev/null);"
        "ps=$(iw dev wlan0 get power_save 2>/dev/null | grep -o 'o[nf]*' | tail -1);"
        "[ -z \"$ps\" ] && ps=$(iw dev wlan2 get power_save 2>/dev/null | grep -o 'o[nf]*' | tail -1);"
        "echo PSM=$([ \"$ps\" = on ] && echo 1 || echo 0);"
        "ip=$(uci -q get network.lan.ipaddr); st=$(uci -q get dhcp.lan.start); lim=$(uci -q get dhcp.lan.limit);"
        "if [ -n \"$ip\" ] && [ -n \"$st\" ]; then pre=${ip%.*}; end=$((st+lim-1)); [ $end -gt 254 ] && end=254;"
        "echo \"POOL=$pre.$st - $pre.$end\"; fi;"
        /* "connected now" = WiFi-associated stations plus ARP-complete entries
         * (ARP covers LAN/USB clients too; leases alone linger 24h). */
        "echo MACS=$({ for w in wlan0 wlan1 wlan2 wlan3; do iw dev $w station dump 2>/dev/null | awk '/Station/{print $2}'; done;"
        " awk 'NR>1 && $3!=\"0x0\"{print $4}' /proc/net/arp 2>/dev/null; } | tr 'A-Z\\n' 'a-z,');"
        "echo DPS=$(ubus call zwrt_bsp.charger list 2>/dev/null | grep direct_power_supply_mode | grep -o 'enable\\|disable');"
        "pid=$(cat /sys/kernel/config/usb_gadget/g1/idProduct 2>/dev/null);"
        "echo USBNET=$([ \"$pid\" = 0x9057 ] || [ \"$pid\" = 0x90b1 ] || [ \"$pid\" = 0x90B1 ] && echo 1 || echo 0);"
        "tc=$(ubus call zwrt_bsp.typec list 2>/dev/null);"
        "echo TYPECSRC=$(echo \"$tc\" | grep power_role | grep -o 'source\\|sink' | head -1);"
        "echo TYPECATTACH=$(echo \"$tc\" | grep cc_attch_state | grep -o '[0-9]' | head -1)", "r");
    if (!fp) return;
    char line[768];
    while (fgets(line, sizeof line, fp)) {
        if      (!strncmp(line, "W0=", 3))   g_w24 = strstr(line, "=up") != NULL;
        else if (!strncmp(line, "W2=", 3))   g_w5  = strstr(line, "=up") != NULL;
        else if (!strncmp(line, "PSM=", 4))  g_wpsm = atoi(line + 4);
        else if (!strncmp(line, "POOL=", 5)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            snprintf(g_dhcp_pool, sizeof g_dhcp_pool, "%s", line + 5);
        }
        else if (!strncmp(line, "MACS=", 5)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            snprintf(g_assoc_macs, sizeof g_assoc_macs, "%s", line + 5);
        }
        else if (!strncmp(line, "DPS=", 4)) {
            if (strstr(line, "disable")) g_dps = 0;
            else if (strstr(line, "enable")) g_dps = 1;
        }
        else if (!strncmp(line, "USBNET=", 7)) {
            g_usb_net = atoi(line + 7) ? 1 : 0;
        }
        else if (!strncmp(line, "TYPECSRC=", 9)) {
            if (strstr(line, "source")) g_typec_source = 1;
            else if (strstr(line, "sink")) g_typec_source = 0;
        }
        else if (!strncmp(line, "TYPECATTACH=", 12)) {
            g_typec_attached = atoi(line + 12) ? 1 : 0;
        }
    }
    pclose(fp);
}

/* ---- screen lock (PIN) persistence. The PIN lives in a dotfile under the UI
 * dir (which always exists) so it survives reboots and isn't clobbered by
 * pushing the .html pages. lock_enabled() == "a PIN is set". ---- */
#define LOCK_PIN_FILE UI_DIR "/.lockpin"
static int lock_enabled(void) { return g_pin[0] != 0; }
static void load_pin(void)
{
    g_pin[0] = 0;
    FILE *fp = fopen(LOCK_PIN_FILE, "r");
    if (!fp) return;
    char b[16]; int o = 0;
    if (fgets(b, sizeof b, fp))
        for (const char *p = b; *p && o < 4; p++)
            if (*p >= '0' && *p <= '9') g_pin[o++] = *p;
    g_pin[o] = 0;
    if (o != 4) g_pin[0] = 0;   /* ignore anything malformed */
    fclose(fp);
    g_refresh_ms = normalize_refresh_ms(g_refresh_ms);
}
static void save_pin(const char *pin)
{
    snprintf(g_pin, sizeof g_pin, "%s", pin);
    FILE *fp = fopen(LOCK_PIN_FILE, "w");
    if (fp) { fprintf(fp, "%s\n", pin); fclose(fp); }
}
static void clear_pin(void) { g_pin[0] = 0; remove(LOCK_PIN_FILE); }
/* Enter the lock pad. setup=1 -> set a new PIN; setup=0 -> unlock prompt. */
static void enter_lock(int setup)
{
    g_lock_state = setup ? 2 : 1;
    g_pin_entry[0] = 0; g_lock_err = 0; g_scroll = 0;
}

/* ---- persisted UI settings (theme / speed unit / auto-off / refresh interval).
 * Stored next to the binary in /data/plugins/u60pro-devui so they survive reinstalling the
 * binary and re-pushing UI files (the PIN persists separately in .lockpin). */
#define CONF_FILE "/data/plugins/u60pro-devui/devui.conf"
static void load_conf(void)
{
    FILE *fp = fopen(CONF_FILE, "r");
    if (!fp) return;
    char line[64]; int v;
    while (fgets(line, sizeof line, fp)) {
        if      (sscanf(line, "theme=%d", &v) == 1)      g_theme = !!v;
        else if (sscanf(line, "speed_bits=%d", &v) == 1) g_speed_bits = !!v;
        else if (sscanf(line, "show_batpct=%d", &v) == 1) g_show_batpct = !!v;
        else if (sscanf(line, "autooff=%d", &v) == 1)    g_autooff_ms = v;
        else if (sscanf(line, "refresh_ms=%d", &v) == 1) g_refresh_ms = normalize_refresh_ms(v);
        else if (sscanf(line, "bright=%d", &v) == 1)     g_saved_bright = v;
    }
    fclose(fp);
}
static void save_conf(void)
{
    FILE *fp = fopen(CONF_FILE, "w");
    if (!fp) return;
    fprintf(fp, "theme=%d\nspeed_bits=%d\nshow_batpct=%d\nautooff=%d\nrefresh_ms=%d\nbright=%d\n",
            g_theme, g_speed_bits, g_show_batpct, g_autooff_ms, g_refresh_ms, backlight_get());
    fclose(fp);
}

/* Append one carrier card (band/bw + PCI, then RSRP/SINR colored by quality).
 * A carrier reporting the floor sentinel (RSRP <= -140) is "configured but not
 * active"; its values are grayed out and tagged inactive. */
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

static int carrier_is_inactive(const char *rsrp)
{
    return atof((rsrp && rsrp[0]) ? rsrp : "") <= -140.0;
}

/* Split a CA group "f0,f1,..." into fields[] (returns count). */
static int ca_split(char *g, char **f, int maxf)
{
    int n = 0; char *save;
    for (char *tk = strtok_r(g, ",", &save); tk && n < maxf; tk = strtok_r(NULL, ",", &save)) f[n++] = tk;
    return n;
}

static int ca_groups(char *s, char **g, int maxg)
{
    int n = 0; char *save;
    for (char *tk = strtok_r(s, ";", &save); tk && n < maxg; tk = strtok_r(NULL, ";", &save)) g[n++] = tk;
    return n;
}

/* LTE SCC signal is sometimes exposed separately from legacy 5-field lteca.
 * We accept both full 11-field groups and shorter signal-only groups by
 * locating the first "RSRP-like" value (typically <= -40 dBm), then taking
 * SINR from two fields later when present. */
static int ca_sig_pick(char *g, const char **rsrp, const char **sinr)
{
    char *f[16];
    int nf = ca_split(g, f, 16), rp = -1;
    *rsrp = *sinr = NULL;
    for (int i = 0; i < nf; i++) {
        if (atof(f[i]) <= -40.0) { rp = i; break; }
    }
    if (rp < 0) return 0;
    *rsrp = f[rp];
    if (rp + 2 < nf)      *sinr = f[rp + 2];
    else if (rp + 1 < nf) *sinr = f[rp + 1];
    return 1;
}

static int signal_level(int rsrp)
{
    int lvl;
    if      (rsrp == 0)    lvl = 0;   /* unknown / no signal */
    else if (rsrp >= -80)  lvl = 5;
    else if (rsrp >= -90)  lvl = 4;
    else if (rsrp >= -100) lvl = 3;
    else if (rsrp >= -110) lvl = 2;
    else                   lvl = 1;
    return lvl;
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
        html_view_polyline(x, y, w, h, h_cpu, h_n, 0, 100, 0x4f, 0x8b, 0xff, 2, 26); /* CPU usage, blue */
        html_view_polyline(x, y, w, h, h_ct,  h_n, 20, 70, 0xff, 0x8c, 0x42, 2, 0);  /* temperature, orange */
    }
    if (html_view_rect("#chart-mem", &x, &y, &w, &h))
        html_view_polyline(x, y, w, h, h_mem, h_n, 0, 100, 0x46, 0xc4, 0x6f, 2, 34); /* memory, green */
    if (html_view_rect("#chart-net", &x, &y, &w, &h)) {
        static int rxi[HIST], txi[HIST];
        long mx = 1;
        for (int i = 0; i < h_n; i++) { if (h_rx[i] > mx) mx = h_rx[i]; if (h_tx[i] > mx) mx = h_tx[i]; }
        for (int i = 0; i < h_n; i++) { rxi[i] = (int)h_rx[i]; txi[i] = (int)h_tx[i]; }
        html_view_polyline(x, y, w, h, rxi, h_n, 0, (int)mx, 0x4f, 0x8b, 0xff, 2, 22); /* downlink, blue */
        html_view_polyline(x, y, w, h, txi, h_n, 0, (int)mx, 0xff, 0x8c, 0x42, 2, 0);  /* uplink, orange */
    }
    if (html_view_rect("#chart-batt", &x, &y, &w, &h)) {
        static int tn[HIST], pn[HIST];
        int mx = 1;
        for (int i = 0; i < h_n; i++) if (h_pwr[i] > mx) mx = h_pwr[i];
        for (int i = 0; i < h_n; i++) { tn[i] = (h_bt[i] - 20) * 2; pn[i] = (int)((long)h_pwr[i] * 100 / mx); }
        html_view_polyline(x, y, w, h, pn, h_n, 0, 100, 0x4f, 0x8b, 0xff, 2, 22); /* power, blue */
        html_view_polyline(x, y, w, h, tn, h_n, 0, 100, 0xff, 0x8c, 0x42, 2, 0);  /* temperature, orange */
    }
}

/* Padlock glyph centered in the lower screen, shown on the locked preview
 * (state 1). Drawn natively so it does not depend on font glyph coverage. */
static void draw_lock_icon(void)
{
    const int cx = 160, cy = 406;          /* icon center, lower-middle */
    int dark = !g_theme;
    int lv = dark ? 0xff : 0x00;           /* lock color: white(dark) / black(light) */
    /* shackle: arch ring (semicircle + short legs); legs end inside the body */
    {
        int xs[40], ys[40], n = 0;
        const int ro = 7, ri = 5, arc_cy = cy - 2, leg = cy + 3;
        xs[n] = cx - ro; ys[n] = leg; n++;
        for (int aa = 180; aa >= 0; aa -= 30) { double r = aa * 3.14159265 / 180; xs[n] = cx + (int)(ro * cos(r)); ys[n] = arc_cy - (int)(ro * sin(r)); n++; }
        xs[n] = cx + ro; ys[n] = leg; n++;
        xs[n] = cx + ri; ys[n] = leg; n++;
        for (int aa = 0; aa <= 180; aa += 30) { double r = aa * 3.14159265 / 180; xs[n] = cx + (int)(ri * cos(r)); ys[n] = arc_cy - (int)(ri * sin(r)); n++; }
        xs[n] = cx - ri; ys[n] = leg; n++;
        html_view_fill_poly(xs, ys, n, lv, lv, lv, 255);
    }
    /* body + keyhole (keyhole in page bg tone to look punched through) */
    html_view_fill_round_rect(cx - 10, cy + 1, 20, 15, 4, lv, lv, lv, 255);
    html_view_fill_round_rect(cx - 2, cy + 6, 4, 4, 2,
                              dark ? 0x15 : 0xec, dark ? 0x16 : 0xee, dark ? 0x1a : 0xf1, 255);
}

/* SMS detail dialog + status-bar envelope state. */
static long g_sms_open = -1;   /* opened SMS message id for detail dialog (-1 = none) */
static int  g_sms_unread_now;  /* unread SMS present -> draw the status-bar envelope */
static char g_sms_num[40], g_sms_date[16], g_sms_text[DEVUI_SMS_TEXT_MAX];   /* opened message */

/* Draw a small envelope in the fixed status bar, just right of the clock, when
 * there are unread messages. Native shape; the font has no envelope glyph. */
static void draw_sms_icon(void)
{
    if (!g_sms_unread_now) return;
    const int ew = 16, eh = 11;
    int ex = 8 + html_view_text_width_px(g_stat_time, 16) + 7;
    int ey = (26 - eh) / 2;
    /* body: blue rounded rectangle */
    html_view_fill_round_rect(ex, ey, ew, eh, 2, 0x2f, 0x6f, 0xe0, 255);
    /* flap: white inverted-V from the two top corners down to center */
    int xs[3] = { ex + 1, ex + ew - 1, ex + ew / 2 };
    int ys[3] = { ey + 1, ey + 1,      ey + eh / 2 + 1 };
    html_view_fill_poly(xs, ys, 3, 0xff, 0xff, 0xff, 255);
}

/* ---- band lock (éé¢): comma-list band sets ---- */
static void draw_native_statusbar(void)
{
    const int dark = !g_theme;
    const int bg_r = dark ? 0x1d : 0xd8, bg_g = dark ? 0x27 : 0xe2, bg_b = dark ? 0x33 : 0xf0;
    const int fg_r = dark ? 0xe9 : 0x1b, fg_g = dark ? 0xeb : 0x1d, fg_b = dark ? 0xee : 0x22;
    const int dim_r = dark ? 0x5b : 0xa6, dim_g = dark ? 0x66 : 0xae, dim_b = dark ? 0x74 : 0xb8;

    html_view_fill_rect(0, 0, 320, 26, bg_r, bg_g, bg_b, 255);
    html_view_draw_text_px(8, 4, g_stat_time, 16, 0, fg_r, fg_g, fg_b, 255);

    const int bat_x = 279, bat_y = 5, bat_w = 35, bat_h = 16;
    const int tip_x = bat_x + bat_w, tip_y = bat_y + 5;
    const int sig_y = 3, sig_h = 18;
    const int sig_left = bat_x - 49;
    static const int bh[5] = { 7, 7, 7, 7, 7 };
    static const int bx[5] = { 18, 23, 28, 33, 38 };
    static const int bw[5] = { 4, 4, 4, 4, 4 };
    const int bar_left = sig_left + bx[0];
    const int bar_right = sig_left + bx[4] + bw[4];
    const int group_gap = bat_x - bar_right;
    int sw = html_view_text_width_px(g_stat_speed, 13);
    int sx = bar_left - group_gap - sw; if (sx < 84) sx = 84;
    html_view_draw_text_px(sx, 5, g_stat_speed, 13, 0, fg_r, fg_g, fg_b, 255);

    const int base = sig_y + sig_h + 1;
    for (int i = 0; i < 5; i++) {
        int on = i < g_stat_sig;
        int r = on ? fg_r : dim_r, g = on ? fg_g : dim_g, b = on ? fg_b : dim_b;
        int x = sig_left + bx[i];
        html_view_fill_round_rect(x, base - bh[i], bw[i], bh[i], 2, r, g, b, 255);
    }
    int gen_size = 11, gw = html_view_text_width_px(g_stat_gen, gen_size);
    if (gw > 24) { gen_size = 10; gw = html_view_text_width_px(g_stat_gen, gen_size); }
    if (gw > 24) { gen_size = 9; }
    gw = html_view_text_width_px(g_stat_gen, gen_size);
    int gx = bar_left + ((bar_right - bar_left) - gw) / 2;
    html_view_draw_text_px(gx, sig_y, g_stat_gen, gen_size, 0, fg_r, fg_g, fg_b, 255);

    html_view_fill_round_rect(bat_x, bat_y, bat_w, bat_h, 4, fg_r, fg_g, fg_b, 255);
    if (dark)
        html_view_fill_round_rect(bat_x + 1, bat_y + 1, bat_w - 2, bat_h - 2, 3, bg_r, bg_g, bg_b, 255);
    else
        html_view_fill_round_rect(bat_x + 1, bat_y + 1, bat_w - 2, bat_h - 2, 3, 0xd6, 0xdc, 0xe4, 255);
    html_view_fill_round_rect(tip_x, tip_y, 2, 7, 1, fg_r, fg_g, fg_b, 255);

    int bat_pct = clampi(g_stat_bat, 0, 100);
    int draw_charging = g_charging;
    int fr = fg_r, fgc = fg_g, fb = fg_b;
    if (draw_charging) { fr = 0x5e; fgc = 0xc8; fb = 0x5e; }
    else if (g_stat_lowbat) { fr = 0xe8; fgc = 0x53; fb = 0x3a; }
    int fill_w = (bat_w - 4) * bat_pct / 100;
    if (fill_w > 0)
        html_view_fill_round_rect(bat_x + 2, bat_y + 2, fill_w, bat_h - 4, 2, fr, fgc, fb, 255);

    if (g_show_batpct) {
        char bp[8]; snprintf(bp, sizeof bp, "%d%%", bat_pct);
        int x0, y0, x1, y1;
        int pct_size = 11;
        html_view_text_bounds_px(bp, pct_size, &x0, &y0, &x1, &y1);
        if (x1 - x0 > bat_w - 4) {
            pct_size = 10;
            html_view_text_bounds_px(bp, pct_size, &x0, &y0, &x1, &y1);
        }
        int tx = bat_x + (bat_w - (x1 - x0)) / 2 - x0;
        int ty = bat_y + (bat_h - (y1 - y0)) / 2 - y0 + 1;
        html_view_draw_text_contrast_px(tx, ty, bp, pct_size, 0,
                                        0x12, 0x25, 0x18,
                                        0xf2, 0xf5, 0xf7, 255);
    }
}

static void draw_center_text_px(int cx, int y, const char *text, int size, int bold,
                                int r, int g, int b, int a)
{
    int x0, y0, x1, y1;
    html_view_text_bounds_px(text, size, &x0, &y0, &x1, &y1);
    html_view_draw_text_px(cx - (x1 - x0) / 2 - x0, y - y0, text, size, bold, r, g, b, a);
}

struct charge_boot_state {
    int pct;
    int charger;
    int charging;
    int full;
    long chg_uv;
    long chg_ua;
};

static void charge_boot_refresh(struct charge_boot_state *s)
{
    static devui_data_t d;
    char st[32];
    long v;
    memset(s, 0, sizeof *s);
    s->pct = -1;

    if (data_refresh(&d)) {
        s->pct = d.bat_percent;
        s->charger = d.charger_connect ? 1 : 0;
        s->charging = (d.charger_connect || d.charging) ? 1 : 0;
        s->chg_uv = d.chg_uv;
        s->chg_ua = d.chg_ua;
    }
    if (s->pct < 0 && read_long_path("/sys/class/power_supply/battery/capacity", &v))
        s->pct = (int)v;
    if (read_line_path("/sys/class/power_supply/battery/status", st, sizeof st)) {
        if (!strcmp(st, "Full")) s->full = 1;
        if (!strcmp(st, "Charging")) s->charging = 1;
    }
    if (!s->charger && read_long_path("/sys/class/power_supply/usb/online", &v))
        s->charger = v != 0;
    if (!s->charger && read_long_path("/sys/class/power_supply/usb/present", &v))
        s->charger = v != 0;
    if (!s->charger && (s->charging || s->full)) s->charger = 1;
    if (s->pct < 0) s->pct = 0;
    if (s->pct > 100) s->pct = 100;
}

static void render_charge_boot(drm_disp_t *disp)
{
    struct charge_boot_state s;
    const int W = disp->width, H = disp->height;
    const int bx = 54, by = 130, bw = 208, bh = 104;
    const int ix = bx + 7, iy = by + 7, iw = bw - 14, ih = bh - 14;
    const int tipw = 10, tiph = 32;
    int fill_w, pulse, fr, fg, fb;
    char pct[8], line1[32], line2[64];

    charge_boot_refresh(&s);
    g_phase++;

    html_view_fill_rect(0, 0, W, H, 0x09, 0x0d, 0x14, 255);
    html_view_fill_round_rect(-28, 36, 154, 154, 72, 0x18, 0x24, 0x38, 88);
    html_view_fill_round_rect(184, 282, 156, 156, 78, 0x0f, 0x2a, 0x22, 72);
    draw_center_text_px(W / 2, 50, "关机充电", 20, 0, 0xd9, 0xdf, 0xea, 255);

    html_view_fill_round_rect(bx, by, bw, bh, 26, 0xd9, 0xdf, 0xea, 255);
    html_view_fill_round_rect(bx + 3, by + 3, bw - 6, bh - 6, 23, 0x0f, 0x14, 0x1d, 255);
    html_view_fill_round_rect(bx + bw, by + (bh - tiph) / 2, tipw, tiph, 4, 0xd9, 0xdf, 0xea, 255);

    pulse = (int)(g_phase % 24);
    if (pulse > 12) pulse = 24 - pulse;
    fr = 0x5e; fg = 0xc8; fb = 0x5e;
    if (s.full) { fr = 0x58; fg = 0xb7; fb = 0xff; }
    else if (s.pct <= 20 && !s.charger) { fr = 0xe8; fg = 0x53; fb = 0x3a; }
    if (s.charger && !s.full) {
        fr = clampi(fr + pulse * 3, 0, 255);
        fg = clampi(fg + pulse * 2, 0, 255);
        fb = clampi(fb + pulse * 3 / 2, 0, 255);
    }
    fill_w = iw * s.pct / 100;
    if (fill_w <= 0 && s.charger) fill_w = 8;
    if (fill_w > 0) {
        html_view_fill_round_rect(ix, iy, fill_w, ih, 18, fr, fg, fb, 255);
        if (s.charger && !s.full && fill_w > 20) {
            int sheen = ix + ((int)(g_phase * 11) % (fill_w + 28)) - 28;
            if (sheen < ix) sheen = ix;
            if (sheen < ix + fill_w)
                html_view_fill_rect(sheen, iy + 6, 18, ih - 12, 0xff, 0xff, 0xff, 42);
        }
    }

    if (s.charger) {
        int cx = W / 2, cy = by + bh / 2;
        int xs[7] = { cx - 12, cx + 2, cx - 3, cx + 11, cx - 8, cx - 2, cx - 11 };
        int ys[7] = { cy - 30, cy - 30, cy - 6, cy - 6, cy + 30, cy + 5, cy + 5 };
        int shx[7], shy[7];
        int br = s.full ? 0xf5 : 0xff;
        int bg = s.full ? 0xfb : 0xf4;
        int bb = s.full ? 0xff : 0xa7;
        for (int i = 0; i < 7; i++) { shx[i] = xs[i] + 1; shy[i] = ys[i] + 2; }
        html_view_fill_poly(shx, shy, 7, 0x05, 0x08, 0x0c, 120);
        html_view_fill_poly(xs, ys, 7, br, bg, bb, 245);
    }

    snprintf(pct, sizeof pct, "%d%%", s.pct);
    draw_center_text_px(W / 2, 274, pct, 46, 1, 0xf5, 0xf7, 0xfa, 255);

    if (s.full) snprintf(line1, sizeof line1, "已充满");
    else if (s.charging) snprintf(line1, sizeof line1, "充电中");
    else if (s.charger) snprintf(line1, sizeof line1, "已接入电源");
    else snprintf(line1, sizeof line1, "等待充电");
    draw_center_text_px(W / 2, 330, line1, 20, 0, 0x9f, 0xd6, 0xb5, 255);

    if (s.chg_uv > 0 && s.chg_ua > 0)
        snprintf(line2, sizeof line2, "输入 %.1fV · %ldmA", s.chg_uv / 1000000.0, s.chg_ua / 1000);
    else
        snprintf(line2, sizeof line2, "按电源键可熄屏/亮屏");
    draw_center_text_px(W / 2, 364, line2, 14, 0, 0x8e, 0x98, 0xa6, 255);

    drm_disp_dirty(disp, 0, 0, W - 1, H - 1);
    maybe_dump_fb(disp);
}

static int  g_modal;           /* 0 none, 1 SA, 2 NSA, 3 LTE (second-level dialog) */
static int  g_segdrag;         /* dragging the segmented control (suppress cell highlight) */
static char g_toast[48];       /* toast message ("" = hidden) */
static uint32_t g_toast_until; /* millis the toast hides at */
static int  g_pwr_confirm;     /* power menu: 0 none, 1 poweroff armed, 2 reboot armed */
static uint32_t g_pwr_until;   /* millis the armed state auto-resets at */
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
    if (o == 0) snprintf(buf, cap, "<span class='muted'>暂无可选频段</span>");
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
static const struct { int ms; const char *lab; } g_refresh_rates[4] = {
    { 0, "停止" }, { 1000, "1s" }, { 2000, "2s" }, { 5000, "5s" } };
static int normalize_refresh_ms(int ms)
{
    switch (ms) {
    case 0:
    case 1000:
    case 2000:
    case 5000:
        return ms;
    case 500:
    default:
        return 1000;
    }
}
/* escape &,<,> for safe litehtml parsing of arbitrary SMS text */
static void html_esc(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *s = src; *s && o + 7 < cap; s++) {
        const char *r = NULL;
        if      (*s == '&') r = "&amp;";
        else if (*s == '<') r = "&lt;";
        else if (*s == '>') r = "&gt;";
        if (r) { size_t L = strlen(r); memcpy(dst + o, r, L); o += L; }
        else dst[o++] = *s;
    }
    dst[o] = 0;
}

/* Copy at most maxb bytes of UTF-8 without splitting a multibyte char; stop at
 * a newline. Sets *cut if the source was longer, so the caller can add ellipsis. */
static void utf8_trunc(char *dst, size_t cap, const char *src, int maxb, int *cut)
{
    int o = 0; *cut = 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s) {
        if (*s == '\n' || *s == '\r') { *cut = 1; break; }
        int len = (*s >= 0xF0) ? 4 : (*s >= 0xE0) ? 3 : (*s >= 0xC0) ? 2 : 1;
        if (o + len > maxb || (size_t)(o + len) >= cap) { *cut = 1; break; }
        for (int k = 0; k < len && *s; k++) dst[o++] = (char)*s++;
    }
    dst[o] = 0;
}

/* Second-level SMS detail dialog (number + date + full text), overlaid dimmed. */
static const char *sms_modal_html(void)
{
    static char out[DEVUI_SMS_TEXT_MAX * 6 + 1024];
    static char esc[DEVUI_SMS_TEXT_MAX * 6];
    html_esc(esc, sizeof esc, g_sms_text);
    snprintf(out, sizeof out,
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><link rel='stylesheet' href='style.css'></head>"
        "<body class='mo'><div class='modal %s'>"
        "<div class='mtitle'>%s</div>"
        "<div class='smsmdate'>%s</div>"
        "<div class='smsmbody'>%s</div>"
        "<div class='mbtns'><a href='act:smsclose' class='mbtn2 prim mfull'>关闭</a></div>"
        "</div></body></html>",
        g_theme ? "light" : "dark", g_sms_num, g_sms_date, esc);
    return out;
}

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
    static char s_qci[8], s_ambr[24], s_sbar[640], s_dots[320];
    static char s_nrrows[1500], s_lterows[1700], s_carriers[4000], s_nr_block[2200], s_lte_block[2200], s_sigcards[5000], s_gen[8];

    static devui_data_t d;
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
    fmt_speed_u(s_rxs, sizeof s_rxs, d.rx_speed, g_speed_bits);
    fmt_speed_u(s_txs, sizeof s_txs, d.tx_speed, g_speed_bits);
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
    if (d.qci > 0) snprintf(s_qci, sizeof s_qci, "%d", d.qci);
    else           snprintf(s_qci, sizeof s_qci, "-");
    if (d.ambr_dl > 0.0 || d.ambr_ul > 0.0) {
        char dl[12], ul[12];
        if (d.ambr_dl > 0.0) snprintf(dl, sizeof dl, "%.0f", d.ambr_dl);
        else                 snprintf(dl, sizeof dl, "-");
        if (d.ambr_ul > 0.0) snprintf(ul, sizeof ul, "%.0f", d.ambr_ul);
        else                 snprintf(ul, sizeof ul, "-");
        snprintf(s_ambr, sizeof s_ambr, "%s/%s Mbps", dl, ul);
    } else {
        snprintf(s_ambr, sizeof s_ambr, "-");
    }

    /* ---- per-carrier display + generation badge ----
     * nrca group: idx,pci,?,band,arfcn,bw,?,rsrp,rsrq,sinr,rssi
     * lteca is seen in both legacy 5-field and newer 11-field variants.
     * Some firmwares expose LTE SCC signal details separately via ltecasig. */
    int is_endc = strstr(d.net_type, "ENDC") || strstr(d.net_type, "EN-DC");
    int is_nsa  = strstr(d.net_type, "NSA") != NULL;
    int is_sa   = strstr(d.net_type, "SA") && !is_nsa;
    int is_lte  = !is_nsa && !is_sa && (strstr(d.net_type, "LTE") || strstr(d.net_type, "4G"));
    int show_nr  = is_sa || is_nsa || is_endc;
    int show_lte = is_nsa || is_endc || is_lte;
    int nr_cc = 0, nr_bw = 0, lte_cc = 0, lte_bw = 0;
    int nr_show_cc = 0, nr_show_bw = 0, lte_show_cc = 0, lte_show_bw = 0, no = 0, lo = 0;
    char rp[12], pc[12], ac[16];
    const char *nr_band = d.nr_band[0] ? d.nr_band : d.band;

    /* NR carriers: PCell from main fields + nrca SCells */
    s_nrrows[0] = 0;
    if (show_nr && nr_band[0] && d.nr_bw[0]) {
        snprintf(rp, sizeof rp, "%d", d.nr_rsrp); snprintf(pc, sizeof pc, "%d", d.nr_pci);
        snprintf(ac, sizeof ac, "%ld", d.nr_channel);
        no = car_row(s_nrrows, no, sizeof s_nrrows, nr_band, d.nr_bw, ac, pc, rp, d.nr_snr);
        nr_show_cc = 1; nr_show_bw = atoi(d.nr_bw);
        if (!carrier_is_inactive(rp)) { nr_cc = 1; nr_bw = atoi(d.nr_bw); }
    }
    if (show_nr) {
        char nrca[256]; strncpy(nrca, d.nrca, sizeof nrca - 1); nrca[sizeof nrca - 1] = 0;
        for (char *grp = strtok(nrca, ";"); grp; grp = strtok(NULL, ";")) {
            char g[96]; strncpy(g, grp, sizeof g - 1); g[sizeof g - 1] = 0;
            char *f[12]; int nf = ca_split(g, f, 12);
            if (nf > 5 && atoi(f[5]) > 0) {
                double rpv = (nf > 7 && f[7] && f[7][0]) ? atof(f[7]) : 0.0;
                char bn[12]; snprintf(bn, sizeof bn, "n%s", f[3]);
                no = car_row(s_nrrows, no, sizeof s_nrrows, bn, f[5], nf > 4 ? f[4] : "-",
                             nf > 1 ? f[1] : "-", nf > 7 ? f[7] : "-", nf > 9 ? f[9] : "-");
                nr_show_cc++;
                nr_show_bw += atoi(f[5]);
                if (rpv > -140.0) { nr_cc++; nr_bw += atoi(f[5]); }
            }
        }
    }

    /* LTE carriers: support both legacy 5-field and newer 11-field groups. */
    s_lterows[0] = 0;
    if (show_lte) {
        char lteca[256]; strncpy(lteca, d.lteca, sizeof lteca - 1); lteca[sizeof lteca - 1] = 0;
        char ltecasig[256]; strncpy(ltecasig, d.ltecasig, sizeof ltecasig - 1); ltecasig[sizeof ltecasig - 1] = 0;
        char *lgrps[8], *sgrps[8];
        int lgn = ca_groups(lteca, lgrps, 8);
        int sgn = ca_groups(ltecasig, sgrps, 8);
        int sig_off = (sgn > 0 && lgn == sgn + 1);   /* ltecasig often contains SCC only */
        for (int gi = 0; gi < lgn; gi++) {
            char *grp = lgrps[gi];
            const char *siggrp = NULL;
            char g[96]; strncpy(g, grp, sizeof g - 1); g[sizeof g - 1] = 0;
            char *f[12]; int nf = ca_split(g, f, 12);
            const char *f_band = NULL, *f_bw = NULL, *f_arfcn = NULL, *f_pci = NULL, *f_rp = NULL, *f_sn = NULL;
            const char *sig_rp = NULL, *sig_sn = NULL;
            char sig_rp_buf[16] = "", sig_sn_buf[16] = "";
            if (nf >= 11) {
                f_band = f[3]; f_arfcn = f[4]; f_bw = f[5]; f_pci = f[1]; f_rp = f[7]; f_sn = f[9];
            } else if (nf >= 5) {
                f_band = f[1]; f_arfcn = f[3]; f_bw = f[4]; f_pci = f[0];
            }
            if (sgn == lgn && gi < sgn) siggrp = sgrps[gi];
            else if (sig_off && gi > 0 && gi - 1 < sgn) siggrp = sgrps[gi - 1];
            else if (gi < sgn) siggrp = sgrps[gi];
            if (siggrp && siggrp[0]) {
                char sg[96]; strncpy(sg, siggrp, sizeof sg - 1); sg[sizeof sg - 1] = 0;
                if (ca_sig_pick(sg, &sig_rp, &sig_sn)) {
                    if (sig_rp && sig_rp[0]) snprintf(sig_rp_buf, sizeof sig_rp_buf, "%s", sig_rp);
                    if (sig_sn && sig_sn[0]) snprintf(sig_sn_buf, sizeof sig_sn_buf, "%s", sig_sn);
                }
            }
            if ((!f_rp || !f_rp[0]) && sig_rp_buf[0]) f_rp = sig_rp_buf;
            if ((!f_sn || !f_sn[0]) && sig_sn_buf[0]) f_sn = sig_sn_buf;
            if (f_bw && atoi(f_bw) > 0) {
                char bn[12];
                if (f_band && (f_band[0] == 'B' || f_band[0] == 'b')) snprintf(bn, sizeof bn, "%s", f_band);
                else snprintf(bn, sizeof bn, "B%s", (f_band && f_band[0]) ? f_band : "-");
                char lr[12] = "-", ls[12] = "-";
                if (f_rp && f_rp[0]) snprintf(lr, sizeof lr, "%s", f_rp);
                if (f_sn && f_sn[0]) snprintf(ls, sizeof ls, "%s", f_sn);
                if (lte_cc == 0) {   /* serving-cell signal from main fields */
                    if (d.lte_rsrp) snprintf(lr, sizeof lr, "%d", d.lte_rsrp);
                    if (d.lte_snr[0]) snprintf(ls, sizeof ls, "%s", d.lte_snr);
                }
                lo = car_row(s_lterows, lo, sizeof s_lterows, bn, f_bw, f_arfcn ? f_arfcn : "-",
                             f_pci ? f_pci : "-", lr, ls);
                lte_show_cc++;
                lte_show_bw += atoi(f_bw);
                if (!carrier_is_inactive(lr)) { lte_cc++; lte_bw += atoi(f_bw); }
            }
        }
    }
    (void)no; (void)lo; (void)pc; (void)ac;

    /* Display summary includes inactive/configured carriers; generation badge
     * still uses active carrier stats above. */
    int total_bw, total_show_bw; const char *cmode;
    if (is_endc)     { total_bw = nr_bw + lte_bw; total_show_bw = nr_show_bw + lte_show_bw; cmode = "EN-DC"; }
    else if (is_nsa) { total_bw = nr_bw + lte_bw; total_show_bw = nr_show_bw + lte_show_bw; cmode = "5G NSA"; }
    else if (is_sa)  { total_bw = nr_bw;          total_show_bw = nr_show_bw;               cmode = "5G SA"; }
    else if (is_lte) { total_bw = lte_bw;         total_show_bw = lte_show_bw;              cmode = "4G LTE"; }
    else             { total_bw = 0;              total_show_bw = 0;                         cmode = d.net_type[0] ? d.net_type : "No Service"; }
    { char cnt[48];
      if (lte_show_cc && nr_show_cc) snprintf(cnt, sizeof cnt, "%d LTE + %d NR 载波", lte_show_cc, nr_show_cc);
      else if (nr_show_cc)           snprintf(cnt, sizeof cnt, "%d NR 载波", nr_show_cc);
      else if (lte_show_cc)          snprintf(cnt, sizeof cnt, "%d LTE 载波", lte_show_cc);
      else                 snprintf(cnt, sizeof cnt, "无载波");
      snprintf(s_carriers, sizeof s_carriers, "<div class='sec'>%s · %s · %d MHz</div>%s%s",
               cmode, cnt, total_show_bw, s_nrrows, s_lterows); }
    if ((is_endc || is_nsa) && s_nrrows[0] && s_lterows[0]) {
        snprintf(s_nr_block, sizeof s_nr_block,
                 "<div class='card'>"
                 "<div class='title'>%s <span class='sub'>%s</span>"
                 "<span class='qa'><b>QCI %s</b><br>AMBR %s</span></div>"
                 "<div class='sec'>%s · %d LTE + %d NR 载波 · %d MHz</div>"
                 "<div class='ctitle'>NR</div>%s"
                 "</div>",
                 s_oper, d.net_type, s_qci, s_ambr, cmode, lte_show_cc, nr_show_cc, total_show_bw, s_nrrows);
        snprintf(s_lte_block, sizeof s_lte_block,
                 "<div class='card'><div class='ctitle'>LTE</div>"
                 "<div class='sec'>%d LTE 载波 · %d MHz</div>%s</div>",
                 lte_show_cc, lte_show_bw, s_lterows);
        snprintf(s_sigcards, sizeof s_sigcards, "%s%s", s_nr_block, s_lte_block);
    } else {
        snprintf(s_sigcards, sizeof s_sigcards,
                 "<div class='card'>"
                 "<div class='title'>%s <span class='sub'>%s</span>"
                 "<span class='qa'><b>QCI %s</b><br>AMBR %s</span></div>"
                 "%s"
                 "</div>",
                 s_oper, d.net_type, s_qci, s_ambr, s_carriers);
    }

    /* generation badge: 5GA / 5G+ / 5G / 4G / LTE / 3G */
    int gen_nr_cc = nr_show_cc;
    int gen_nr_bw = nr_show_bw;
    int op = 0;   /* 1 mobile, 2 unicom, 3 telecom, 4 broadnet, 5 other-mainland */
    if (d.mcc == 460) {
        int m = d.mnc;
        if (m == 0 || m == 2 || m == 4 || m == 7 || m == 8) op = 1;
        else if (m == 1 || m == 6 || m == 9) op = 2;
        else if (m == 3 || m == 5 || m == 11) op = 3;
        else if (m == 15) op = 4;
        else op = 5;
    }
    if (d.mcc == 460) {
        const char *cn = op == 1 ? "中国移动" : op == 2 ? "中国联通" :
                         op == 3 ? "中国电信" : op == 4 ? "中国广电" : NULL;
        if (cn) snprintf(s_oper, sizeof s_oper, "%s", cn);
    }
    const char *gen = "--";
    if (is_sa) {
        if ((op == 1 || op == 4) && gen_nr_cc >= 3) gen = "5GA";
        else if ((op == 2 || op == 3) && gen_nr_bw >= 200) gen = "5GA";
        else if (gen_nr_bw > 100) gen = "5G+";
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
    snprintf(g_stat_time, sizeof g_stat_time, "%s", s_time);
    g_stat_sig = signal_level(d.nr_rsrp ? d.nr_rsrp : d.lte_rsrp);
    snprintf(g_stat_gen, sizeof g_stat_gen, "%s", gen);

    /* ---- shared status bar: native-drawn content over a simple 26px slot ---- */
    {
        char sp[40];
        fmt_speed_pair(sp, sizeof sp, d.tx_speed, d.rx_speed, g_speed_bits);
        snprintf(g_stat_speed, sizeof g_stat_speed, "%s", sp);
        g_stat_bat = clampi(d.bat_percent, 0, 100);
        g_stat_lowbat = d.bat_percent <= 20;
        g_sms_unread_now = d.sms_unread > 0;
        snprintf(s_sbar, sizeof s_sbar, "<div class='sbar'></div>");
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
    static char s_memdet[24], s_upshort[16], s_autooff[420], s_refreshseg[360];
    { int bmax = backlight_max(); if (bmax <= 0) bmax = 255;
      snprintf(s_bright, sizeof s_bright, "%d", clampi(backlight_get() * 100 / bmax, 0, 100)); }
    { long mt = d.mem_total, ma = d.mem_avail;
      snprintf(s_memdet, sizeof s_memdet, "%ld/%ld MB", mt ? (mt - ma) / 1048576 : 0, mt / 1048576); }
    static char s_chgv[8], s_chgi[10], s_batv[8], s_bati[10], s_pwr[10];
    snprintf(s_chgv, sizeof s_chgv, "%.2f", d.chg_uv / 1e6);
    snprintf(s_chgi, sizeof s_chgi, "%ld", d.chg_ua / 1000);
    snprintf(s_batv, sizeof s_batv, "%.2f", d.bat_uv / 1e6);
    /* signed: + while charging, - while discharging (matches PWRLBL) */
    snprintf(s_bati, sizeof s_bati, "%s%ld", d.charger_connect ? "+" : "-", labs(d.bat_ua) / 1000);
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
    {   /* refresh segmented control (#refreshseg), same UI as auto-off */
        int active = 0;
        for (int k = 0; k < 4; k++) if (g_refresh_rates[k].ms == g_refresh_ms) active = k;
        int hl = g_segdrag == 3 ? -1 : active;
        int o = snprintf(s_refreshseg, sizeof s_refreshseg, "<div id='refreshseg' class='seg seg4'>");
        for (int k = 0; k < 4; k++)
            o += snprintf(s_refreshseg + o, sizeof s_refreshseg - o,
                "<a href='act:refreshms:%d' class='segc%s'>%s</a>",
                g_refresh_rates[k].ms, k == hl ? " seg-on" : "", g_refresh_rates[k].lab);
        snprintf(s_refreshseg + o, sizeof s_refreshseg - o, "</div>");
    }
    snprintf(s_cusage, sizeof s_cusage, "%ld", d.cpu_usage < 0 ? 0 : d.cpu_usage);
    snprintf(s_ctemp, sizeof s_ctemp, "%ld", d.cpu_temp);
    snprintf(s_btemp, sizeof s_btemp, "%d", d.bat_temp);
    snprintf(s_swver, sizeof s_swver, "%s", d.sw_version[0] ? d.sw_version : "-");
    if (g_show_imei && d.imei[0]) snprintf(s_imei, sizeof s_imei, "%s", d.imei);
    else { int n = d.imei[0] ? (int)strlen(d.imei) : 15; if (n > 23) n = 23; memset(s_imei, '*', n); s_imei[n] = 0; }
    snprintf(s_spu, sizeof s_spu, "%s", g_speed_bits ? "Mbps" : "MB/s");

    /* ---- connected device list (page 2): all of them, filtered to currently
     * WiFi-associated MACs (leases linger after a device leaves) ---- */
    static char s_clist[2600];
    {
        int o = 0, shown = 0;
        for (int i = 0; i < d.client_n; i++) {
            if (!mac_assoc(d.client[i].mac)) continue;
            o += snprintf(s_clist + o, sizeof s_clist - o,
                "<div class='cli'><span class='cn'>%s</span><span class='cip'>%s</span></div>",
                d.client[i].name, d.client[i].ip);
            shown++;
        }
        if (shown == 0)
            snprintf(s_clist, sizeof s_clist, "<div class='cli muted'>暂无已连接设备</div>");
        /* keep the header count consistent with the filtered list */
        if (g_assoc_macs[0]) snprintf(s_clients, sizeof s_clients, "%d", shown);
    }

    /* ---- sms list (read-only page): collapsed cards, newest first. Each card is
     * a tap target (act:sms:ID) that opens the full message in a dialog. ---- */
    static char s_smslist[131072];
    {
        int o = 0;
        for (int si = 0; si < d.sms_n; si++) {
            char prev[80]; int cut;
            utf8_trunc(prev, sizeof prev, d.sms[si].text, 40, &cut);
            char esc[512];
            html_esc(esc, sizeof esc, prev);
            if (o >= (int)sizeof(s_smslist)) break;
            int wr = snprintf(s_smslist + o, sizeof(s_smslist) - o,
                "<a href='act:sms:%ld' class='sms%s'>"
                "<div class='smsh'><span class='smsn'>%s</span><span class='smsd'>%s</span></div>"
                "<div class='smsp'>%s%s</div></a>",
                d.sms[si].id, d.sms[si].unread ? " un" : "", d.sms[si].num, d.sms[si].date,
                esc, cut ? "…" : "");
            if (wr < 0) wr = 0;
            if (wr >= (int)(sizeof(s_smslist) - o)) {
                o = (int)sizeof(s_smslist) - 1;
                s_smslist[o] = 0;
                break;
            }
            o += wr;
        }
        if (d.sms_n == 0)
            snprintf(s_smslist, sizeof s_smslist, "<div class='sms muted'>暂无短信</div>");
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
    int usb_net_on = (g_usb_net == 1) || usb_pid_is("9057") || usb_pid_is("90b1") || usb_pid_is("90B1");
    int usb_reverse = (g_typec_source == 1);
    uint32_t now_ms = millis();
    if (g_adb_pending >= 0) {
        if (g_adb_pending == adb_on) g_adb_pending = -1;
        else                         adb_on = g_adb_pending;
    }
    if (g_usb_net_pending >= 0) {
        if (g_usb_net_pending == usb_net_on || now_ms > g_usb_net_until) g_usb_net_pending = -1;
        else                                                            usb_net_on = g_usb_net_pending;
    }
    if (g_typec_pending >= 0) {
        if (g_typec_pending == usb_reverse || now_ms > g_typec_until) g_typec_pending = -1;
        else                                                         usb_reverse = g_typec_pending;
    }
    const char *adb_state = g_adb_pending >= 0 ? "切换中" : (adb_on ? "已开启" : "已关闭");
    const char *usb_pwr_state = g_typec_pending >= 0 ? "切换中" : (usb_reverse ? "反向供电" : "给U60充电");
    const char *usb_net_state = g_usb_net_pending >= 0 ? "切换中" : (usb_net_on ? "共享中" : "仅充电");

    int connected = strstr(d.wan_status, "connect") != NULL;
    int wifi_master = (g_w24 == 1 || g_w5 == 1);
    int i = 0;
    t[i++] = (struct kv){ "STATUSBAR", s_sbar };   t[i++] = (struct kv){ "DOTS", s_dots };
    t[i++] = (struct kv){ "SIGBARS", "" };
    t[i++] = (struct kv){ "TIME", s_time };        t[i++] = (struct kv){ "BAT", s_bat };
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
    t[i++] = (struct kv){ "MODEL", s_model };      t[i++] = (struct kv){ "FW", s_fw };
    t[i++] = (struct kv){ "PAGE", s_page };        t[i++] = (struct kv){ "NPAGES", s_np };
    t[i++] = (struct kv){ "CARRIERS", s_carriers }; t[i++] = (struct kv){ "GEN", s_gen };
    t[i++] = (struct kv){ "SIGNALCARDS", s_sigcards };
    t[i++] = (struct kv){ "GENCLASS", genc };
    t[i++] = (struct kv){ "THEME", g_theme ? "light" : "dark" };
    t[i++] = (struct kv){ "BATCLASS", d.bat_percent <= 20 ? "low" : "" };
    t[i++] = (struct kv){ "KEYBTN", g_show_key ? "隐藏密码" : "显示密码" };
    t[i++] = (struct kv){ "ADBCLASS", adb_on ? "on" : "off" };
    t[i++] = (struct kv){ "ADBSTATE", adb_state };
    t[i++] = (struct kv){ "USBPWRCLASS", usb_reverse ? "on" : "off" };
    t[i++] = (struct kv){ "USBPWRSTATE", usb_pwr_state };
    t[i++] = (struct kv){ "USBNETCLASS", usb_net_on ? "on" : "off" };
    t[i++] = (struct kv){ "USBNETSTATE", usb_net_state };
    t[i++] = (struct kv){ "BATPCTCLASS", g_show_batpct ? "on" : "off" };
    t[i++] = (struct kv){ "BATPCTSTATE", g_show_batpct ? "已显示" : "已隐藏" };
    t[i++] = (struct kv){ "THEMECLASS", g_theme ? "on" : "off" };
    t[i++] = (struct kv){ "THEMESTATE", g_theme ? "浅色模式" : "深色模式" };
    t[i++] = (struct kv){ "SPUNITCLASS", g_speed_bits ? "on" : "off" };
    t[i++] = (struct kv){ "SPUNITSTATE", g_speed_bits ? "比特率 Mbps" : "字节率 MB/s" };
    t[i++] = (struct kv){ "SPUNIT", s_spu };
    t[i++] = (struct kv){ "CPUUSAGE", s_cusage }; t[i++] = (struct kv){ "CPUTEMP", s_ctemp };
    t[i++] = (struct kv){ "BATTEMP", s_btemp };   t[i++] = (struct kv){ "SWVER", s_swver };
    t[i++] = (struct kv){ "IMEI", s_imei };
    t[i++] = (struct kv){ "IMEIBTN", g_show_imei ? "隐藏" : "显示" };
    t[i++] = (struct kv){ "BRIGHT", s_bright };   t[i++] = (struct kv){ "AUTOOFF", s_autooff };
    t[i++] = (struct kv){ "REFRESHSEG", s_refreshseg };
    t[i++] = (struct kv){ "MEMDETAIL", s_memdet }; t[i++] = (struct kv){ "UPSHORT", s_upshort };
    t[i++] = (struct kv){ "CHGV", s_chgv };       t[i++] = (struct kv){ "CHGI", s_chgi };
    t[i++] = (struct kv){ "BATV", s_batv };       t[i++] = (struct kv){ "BATI", s_bati };
    t[i++] = (struct kv){ "PWR", s_pwr };         t[i++] = (struct kv){ "PWRLBL", d.charger_connect ? "充电" : "放电" };
    t[i++] = (struct kv){ "NETSEG", s_netseg };   t[i++] = (struct kv){ "TOAST", s_toast };
    t[i++] = (struct kv){ "PWROFFLBL",  g_pwr_confirm == 1 ? "再按一次" : "关机" };
    t[i++] = (struct kv){ "PWROFFCLS",  g_pwr_confirm == 1 ? "armed" : "" };
    t[i++] = (struct kv){ "PWRREBLBL",  g_pwr_confirm == 2 ? "再按一次" : "重启" };
    t[i++] = (struct kv){ "PWRREBCLS",  g_pwr_confirm == 2 ? "armed" : "" };
    t[i++] = (struct kv){ "CURSA", s_cursa };     t[i++] = (struct kv){ "CURNSA", s_curnsa };
    t[i++] = (struct kv){ "CURLTE", s_curlte };
    t[i++] = (struct kv){ "NFCCLASS", d.nfc_switch ? "on" : "off" };
    t[i++] = (struct kv){ "NFCSTATE", d.nfc_switch ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WIFICLASS", wifi_master ? "on" : "off" };
    t[i++] = (struct kv){ "WIFISTATE", (g_w24 < 0 && g_w5 < 0) ? "—" : wifi_master ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WIFI24CLASS", g_w24 == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "WIFI24STATE", g_w24 < 0 ? "—" : g_w24 ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "WIFI5CLASS", g_w5 == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "WIFI5STATE", g_w5 < 0 ? "—" : g_w5 ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "DPSCLASS", g_dps == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "DPSSTATE", g_dps < 0 ? "—" : g_dps ? "已开启" : "已关闭" };
    t[i++] = (struct kv){ "PSMCLASS", g_wpsm == 1 ? "on" : "off" };
    t[i++] = (struct kv){ "PSMSTATE", g_wpsm < 0 ? "—" : g_wpsm ? "已开启（省电）" : "已关闭（高性能）" };
    t[i++] = (struct kv){ "CLIENTLIST", s_clist }; t[i++] = (struct kv){ "DHCP_IP", d.dhcp_ip[0] ? d.dhcp_ip : "-" };
    t[i++] = (struct kv){ "SMSLIST", s_smslist };
    t[i++] = (struct kv){ "DHCP_POOL", g_dhcp_pool[0] ? g_dhcp_pool : s_pool };
    t[i++] = (struct kv){ "DHCP_LEASE", s_lease };

    /* ---- lock screen (PIN pad): dots reflect typed digits. ---- */
    static char s_pindots[512], s_lockaux[140];
    {
        int filled = (int)strlen(g_pin_entry);
        int o = snprintf(s_pindots, sizeof s_pindots, "<div class='pin-slots'>");
        for (int k = 0; k < 4; k++)
            o += (k < filled)
               ? snprintf(s_pindots + o, sizeof s_pindots - o, "<span class='ps'><span class='dot'></span></span>")
               : snprintf(s_pindots + o, sizeof s_pindots - o,
                          "<span class='ps'><span class='ring'><span class='ri'></span></span></span>");
        snprintf(s_pindots + o, sizeof s_pindots - o, "</div>");
    }
    if (g_lock_state == 2)
        snprintf(s_lockaux, sizeof s_lockaux, "<a href='act:lockcancel' class='kp kpx'>取消</a>");
    else s_lockaux[0] = 0;
    t[i++] = (struct kv){ "PINDOTS", s_pindots };
    t[i++] = (struct kv){ "LOCKAUX", s_lockaux };
    t[i++] = (struct kv){ "LOCKTITLE", g_lock_state == 2 ? "设置锁屏密码" : "请输入锁屏密码" };
    t[i++] = (struct kv){ "LOCKMSG", g_lock_err ? "<div class='lock-err'>密码错误</div>" : "" };
    t[i++] = (struct kv){ "LOCKCLASS", lock_enabled() ? "on" : "off" };
    t[i++] = (struct kv){ "LOCKSTATE", lock_enabled() ? "已开启" : "已关闭" };
    return i;
}

static void refresh_status_cache(void)
{
    struct kv t[160];
    (void)build_kv(t);
}

/* Build the data-filled HTML for a page (returns a static buffer). */
static const char *page_html(const char *path)
{
    const char *tmpl = read_template_cached(path);   /* cache templates with mtime invalidation */
    if (!tmpl) return NULL;
    struct kv t[160];
    int n = build_kv(t);
    char *html = apply_template(tmpl, t, n);
    return html;
}

static void invalidate_render_html_cache(void)
{
    g_render_html_cache_len = 0;
    g_render_html_cache_path[0] = 0;
    g_render_html_cache_css_mtime = -1;
}

/* ---- power menu: three large round buttons with native-drawn glyphs
 * (litehtml can't render a power symbol / circular arrow / X reliably). The
 * HTML provides the circle placeholders (#pmc-*) + labels + tap targets; we
 * fill the disc and draw the glyph centered over each. ---- */
#define PM_PI 3.14159265358979

static void pm_disc(int cx, int cy, int rad, int r, int g, int b, int a)
{
    html_view_fill_round_rect(cx - rad, cy - rad, rad * 2, rad * 2, rad, r, g, b, a);
}

/* thick line segment (a-b) as a filled quad */
static void pm_line(double x1, double y1, double x2, double y2, double th, int r, int g, int b)
{
    double dx = x2 - x1, dy = y2 - y1, len = sqrt(dx * dx + dy * dy);
    if (len < 0.001) return;
    double nx = -dy / len * th / 2.0, ny = dx / len * th / 2.0;
    int xs[4] = { (int)(x1 + nx + 0.5), (int)(x2 + nx + 0.5), (int)(x2 - nx + 0.5), (int)(x1 - nx + 0.5) };
    int ys[4] = { (int)(y1 + ny + 0.5), (int)(y2 + ny + 0.5), (int)(y2 - ny + 0.5), (int)(y1 - ny + 0.5) };
    html_view_fill_poly(xs, ys, 4, r, g, b, 255);
}

/* annular sector (ring band) from a0..a1 degrees (math angles; screen y down) */
static void pm_arc(int cx, int cy, double ro, double ri, int a0, int a1, int r, int g, int b)
{
    int xs[160], ys[160], N = 0;
    for (int a = a0; a <= a1 && N < 158; a += 8) {
        double t = a * PM_PI / 180.0;
        xs[N] = cx + (int)(ro * cos(t) + 0.5); ys[N] = cy - (int)(ro * sin(t) + 0.5); N++;
    }
    for (int a = a1; a >= a0 && N < 160; a -= 8) {
        double t = a * PM_PI / 180.0;
        xs[N] = cx + (int)(ri * cos(t) + 0.5); ys[N] = cy - (int)(ri * sin(t) + 0.5); N++;
    }
    html_view_fill_poly(xs, ys, N, r, g, b, 255);
}

/* kind: 0=power, 1=reboot, 2=cancel */
static void pm_glyph(const char *sel, int kind, int cr, int cg, int cb)
{
    int x, y, w, h;
    if (!html_view_rect(sel, &x, &y, &w, &h)) return;
    int rad = (w < h ? w : h) / 2;
    int cx = x + w / 2, cy = y + h / 2;
    int armed = (kind == 0 && g_pwr_confirm == 1) || (kind == 1 && g_pwr_confirm == 2);
    if (armed) pm_disc(cx, cy, rad + 4, 0xff, 0xff, 0xff, 255);   /* white halo when armed */
    pm_disc(cx, cy, rad, cr, cg, cb, 255);
    const int W = 0xff;
    double ro = rad * 0.54, ri = ro - rad * 0.15, th = rad * 0.15;
    if (kind == 0) {                         /* power: ring with a gap at top + vertical bar */
        pm_arc(cx, cy, ro, ri, 110, 430, W, W, W);
        pm_line(cx, cy - rad * 0.06, cx, cy - rad * 0.60, th, W, W, W);
    } else if (kind == 1) {                  /* reboot: about 290 degree ring + arrowhead */
        int a0 = 120, a1 = 410;
        pm_arc(cx, cy, ro, ri, a0, a1, W, W, W);
        double te = a1 * PM_PI / 180.0, rm = (ro + ri) / 2.0;
        double pex = cx + rm * cos(te), pey = cy - rm * sin(te);
        double tx = -sin(te), ty = -cos(te), rxx = cos(te), ryy = -sin(te);
        double L = (ro - ri) * 1.9, hw = (ro - ri) * 1.25;
        int ax[3] = { (int)(pex + tx * L + 0.5), (int)(pex + rxx * hw + 0.5), (int)(pex - rxx * hw + 0.5) };
        int ay[3] = { (int)(pey + ty * L + 0.5), (int)(pey + ryy * hw + 0.5), (int)(pey - ryy * hw + 0.5) };
        html_view_fill_poly(ax, ay, 3, W, W, W, 255);
    } else {                                 /* cancel: X */
        double d = rad * 0.40;
        pm_line(cx - d, cy - d, cx + d, cy + d, th, W, W, W);
        pm_line(cx - d, cy + d, cx + d, cy - d, th, W, W, W);
    }
}

static void draw_power_menu(void)
{
    pm_glyph("#pmc-off",    0, 0xd2, 0x48, 0x3c);   /* red */
    pm_glyph("#pmc-reboot", 1, 0xe0, 0x89, 0x2a);   /* orange */
    pm_glyph("#pmc-cancel", 2, 0x4a, 0x51, 0x5c);   /* gray */
}

static void capture_fb(drm_disp_t *d);   /* fwd: fb snapshot for modal overlay */

static void maybe_dump_fb(drm_disp_t *d)
{
    if (access("/tmp/u60-dumpfb", F_OK) != 0) return;
    FILE *f = fopen("/tmp/fb.dump", "wb");
    if (!f) return;
    const int W = d->width, H = d->height, pp = d->pitch_px;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t px = d->fb[(size_t)(H - 1 - y) * pp + (W - 1 - x)];
            fwrite(&px, sizeof px, 1, f);
        }
    }
    fclose(f);
}

static void render(drm_disp_t *disp, const char *path)
{
    const char *html = page_html(path);
    if (!html) return;
    int special_page = strstr(path, "menu.html") || strstr(path, "lockscreen.html");
    int scroll = special_page ? 0 : g_scroll;
    size_t html_len = strlen(html);
    int has_charts = strstr(path, "charts.html") != NULL;
    long long css_mtime = file_mtime_ns(UI_DIR "/style.css");
    int cacheable = html_len > 0 && html_len < sizeof(g_render_html_cache);
    int reuse = cacheable && !has_charts &&
                 g_render_html_cache_scroll == scroll &&
                 g_render_html_cache_modal == g_modal &&
                 g_render_html_cache_sms_open == g_sms_open &&
                 g_render_html_cache_lock == g_lock_state &&
                 g_render_html_cache_css_mtime == css_mtime &&
                 g_render_html_cache_len == html_len &&
                 !strcmp(g_render_html_cache_path, path) &&
                 !strncmp(g_render_html_cache, html, html_len);
    html_view_set_scroll(scroll);
    if (!reuse) {
        g_page_h = html_view_render_html(html);
        if (cacheable) {
            memcpy(g_render_html_cache, html, html_len + 1);
            g_render_html_cache_len = html_len;
            snprintf(g_render_html_cache_path, sizeof g_render_html_cache_path, "%s", path);
            g_render_html_cache_scroll = scroll;
            g_render_html_cache_modal = g_modal;
            g_render_html_cache_sms_open = g_sms_open;
            g_render_html_cache_lock = g_lock_state;
            g_render_html_cache_css_mtime = css_mtime;
        } else {
            invalidate_render_html_cache();
        }
    }
    draw_charts();      /* native polylines into any #chart-* placeholders */
    draw_native_statusbar();
    draw_sms_icon();    /* native envelope in the status bar when unread SMS */
    if (strstr(path, "menu.html")) draw_power_menu();   /* round power buttons */
    if (g_lock_state == 1) draw_lock_icon();   /* locked preview: lock glyph */
    int H = disp->height, maxs = g_page_h - H;
    (void)maxs;
    if (g_modal) {                                    /* dim page + second-level dialog */
        html_view_fill_rect(0, 28, disp->width, H - 28, 0, 0, 0, 150);
        capture_fb(disp);                             /* snapshot page+dim for fast modal refresh */
        html_view_render_overlay(modal_html());
    } else if (g_sms_open >= 0) {                     /* dim page + SMS detail dialog */
        html_view_fill_rect(0, 28, disp->width, H - 28, 0, 0, 0, 150);
        capture_fb(disp);
        html_view_render_overlay(sms_modal_html());
    }
    drm_disp_dirty(disp, 0, 0, disp->width - 1, disp->height - 1);
    maybe_dump_fb(disp);
}

static void render_ext_view(drm_disp_t *disp, devui_ext_t *ext)
{
    devui_ext_render(ext, disp);
    draw_native_statusbar();
    draw_sms_icon();
    drm_disp_dirty(disp, 0, 0, disp->width - 1, disp->height - 1);
    maybe_dump_fb(disp);
}

/* Offscreen page bitmaps (logical, no rotation) for slide transitions.
 * During a drag: g_bufA = left page, g_bufB = right page (windowed by offset o:
 * window column x shows [left|right][x+o], o in 0..W). */
static uint16_t g_bufA[320 * 480], g_bufB[320 * 480];

/* Status bar stays pinned during a swipe (only the content below slides), so
 * we keep the already-rendered native status bar rows untouched and only
 * update the content area beneath it. */
#define SBAR_H DEVUI_EXT_STATUSBAR_H
/* Keep only a tiny hysteresis so page/scroll drags respond immediately,
 * without turning every resting-finger jitter into a swipe. */
#define DRAG_START_PX 2
#define DRAG_CANCEL_PX 8
#define SCROLL_INERTIA_MIN_V 180.0f
#define SCROLL_INERTIA_MAX_V 2200.0f
#define SCROLL_INERTIA_DECEL 3600.0f
#define STATE_RENDER_THROTTLE_MS 1500
#define STATE_RENDER_IDLE_THROTTLE_MS 2500
#define EXT_BACK_EDGE_PX 24
#define EXT_BACK_COMMIT_PX 56
#define EXT_BACK_MAX_DY 44
#define IDLE_SLEEP_ON_US 8000
#define IDLE_SLEEP_OFF_US 30000

static inline uint16_t *copy_rev_span(uint16_t *dp, const uint16_t *src, int n)
{
    for (int i = 0; i < n; i++) *dp-- = src[i];
    return dp;
}

static inline void fill_rev_span(uint16_t *dp, uint16_t px, int n)
{
    while (n-- > 0) *dp-- = px;
}

static float clamp_scroll_v(float v)
{
    if (v >  SCROLL_INERTIA_MAX_V) return  SCROLL_INERTIA_MAX_V;
    if (v < -SCROLL_INERTIA_MAX_V) return -SCROLL_INERTIA_MAX_V;
    return v;
}

static void compose_frame(drm_disp_t *d, int o)
{
    const int W = d->width, H = d->height, pp = d->pitch_px;
    int left;
    if (o < 0) o = 0; if (o > W) o = W;
    left = W - o;
    for (int y = SBAR_H; y < H; y++) {
        uint16_t *dp = d->fb + (size_t)(H - 1 - y) * pp + (W - 1);
        const uint16_t *lr = g_bufA + (size_t)y * W, *rr = g_bufB + (size_t)y * W;
        if (left > 0) dp = copy_rev_span(dp, lr + o, left);
        if (o > 0)    copy_rev_span(dp, rr, o);
    }
    /* Only the content area changes during a horizontal swipe. With the
     * framebuffer rotated 180 degrees that maps to the top physical rows. */
    drm_disp_dirty(d, 0, 0, W - 1, H - SBAR_H - 1);
    maybe_dump_fb(d);
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
 * without re-parsing/re-layout, so it keeps up like the horizontal swipe. */
#define SCROLLMAX 2048
static uint16_t g_scrollbuf[320 * SCROLLMAX];
static int g_scroll_h;

static void scroll_blit(drm_disp_t *d, int scroll)
{
    const int W = d->width, Hh = d->height, pp = d->pitch_px;
    for (int y = SBAR_H; y < Hh; y++) {
        int sy = y + scroll;
        const uint16_t *src = (sy >= 0 && sy < g_scroll_h) ? &g_scrollbuf[(size_t)sy * W] : NULL;
        uint16_t *dp = d->fb + (size_t)(Hh - 1 - y) * pp + (W - 1);
        if (src) copy_rev_span(dp, src, W);
        else     fill_rev_span(dp, 0, W);
    }
    /* The native status bar is pinned; dragging only changes the content below it.
     * With the framebuffer rotated 180 degrees, that content maps to the top
     * framebuffer rows, so the dirty rectangle intentionally excludes the tail. */
    drm_disp_dirty(d, 0, 0, W - 1, Hh - SBAR_H - 1);
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

/* Screen off: fade backlight down (content still shown) then blank the fb. */
static void screen_off(drm_disp_t *d)
{
    backlight_fade_off();
    invalidate_render_html_cache();
    memset(d->fb, 0, (size_t)d->pitch_px * d->height * sizeof(uint16_t));
    drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
}
/* Screen on: render, then "warm up" the command-mode panel with several frame
 * pushes while the backlight is still 0; this drives it past the idle-exit
 * transient invisibly, then fades the backlight up from 0. */
static void screen_on(drm_disp_t *d, const char *path)
{
    if (g_charge_boot) render_charge_boot(d);
    else               render(d, path);       /* backlight still 0 from screen_off */
    for (int k = 0; k < 5; k++) {            /* ~175ms of dark refresh */
        usleep(35000);
        drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
    }
    backlight_fade_on();   /* 0 -> user level */
}

static void screen_on_ext(drm_disp_t *d, devui_ext_t *ext)
{
    render_ext_view(d, ext);                  /* backlight still 0 from screen_off */
    for (int k = 0; k < 5; k++) {
        usleep(35000);
        drm_disp_dirty(d, 0, 0, d->width - 1, d->height - 1);
    }
    backlight_fade_on();
}

int main(void)
{
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    g_charge_boot = boot_is_charge_mode();
    if (!g_charge_boot) {
        scan_pages();
        if (g_npages == 0) { fprintf(stderr, "no pages in %s\n", UI_DIR); return 1; }
    }

    drm_disp_t disp;
    if (drm_disp_init(&disp) != 0) { fprintf(stderr, "drm init failed\n"); return 1; }
    html_view_init(disp.fb, disp.width, disp.height, disp.pitch_px, 1, UI_FONT);
    html_view_set_uidir(UI_DIR);

    touch_input_t touch;
    int touch_ok = touch_input_init(&touch, disp.width, disp.height) == 0;
    key_input_t key;     key_input_init(&key);
    backlight_init();
    (void)data_backend_init();
    (void)data_backend_commit_latest();

    if (!g_charge_boot && !touch_ok && boot_has_external_power()) {
        fprintf(stderr, "boot: forcing charge UI (touch missing, external power present)\n");
        g_charge_boot = 1;
    }

    int menu = 0, prev_press = 0, prev_lock = 0, was_on = 1, down_x = 0, down_y = 0;
    int dragging = 0, drag_dir = 0, drag_target = 0;
    int scroll_dir = 0, scroll_start = 0;
    int scroll_inertia = 0, scroll_track_valid = 0;
    int scroll_track_pos = 0;
    uint32_t drag_down_ms = 0;
    uint32_t scroll_track_ms = 0;
    float scroll_v = 0.0f, scroll_pos = 0.0f;
    int sliding = 0, bar_x = 0, bar_w = 0;   /* brightness slider drag */
    int segging = 0, seg_x = 0, seg_y = 0, seg_w = 0, seg_h = 0;   /* segmented control drag */
    int seg_which = 0, seg_n = 4;   /* 1 = net mode, 2 = auto-off, 3 = refresh */
    const int W = disp.width, H = disp.height;
    devui_ext_t ext;
    memset(&ext, 0, sizeof ext);
    ext.srv_fd = -1;
    int ext_ok = 0;
    if (!g_charge_boot) {
        ext_ok = devui_ext_init(&ext, W, H) == 0;
        if (!ext_ok) fprintf(stderr, "warning: external display socket disabled\n");
    }
    char menu_path[300], lock_path[300];
    snprintf(menu_path, sizeof menu_path, "%s/menu.html", UI_DIR);
    snprintf(lock_path, sizeof lock_path, "%s/lockscreen.html", UI_DIR);

    /* lock pad takes priority over the power menu and the normal pages */
    /* lock states: 1=preview (page 1), 2=setup pad, 3=unlock pad */
    #define CUR_PATH (g_lock_state == 1 ? g_pages[0] : g_lock_state ? lock_path : menu ? menu_path : g_pages[g_cur])

    load_conf();                          /* restore persisted UI settings */
    if (g_saved_bright >= 0) backlight_set(g_saved_bright);   /* restore brightness */
    if (!g_charge_boot) {
        load_pin();
        wifi_aux_refresh();                   /* prime page-2 switch states */
        if (usb_pid_is("9057") || usb_pid_is("90b1") || usb_pid_is("90B1"))
            usb_net_watchdog_start();
        if (lock_enabled()) enter_lock(0);   /* boot straight into the unlock pad */
        render(&disp, CUR_PATH);
    } else {
        render_charge_boot(&disp);
    }
    uint32_t last_data = millis(), last_act = millis();
    int state_pending = 0;

    while (g_run) {
        uint32_t now = millis();
        int need_render = 0, animating = 0;
        int live_changed = data_backend_poll(now);
        if (live_changed) state_pending = 1;

        if (ext_ok && g_lock_state && devui_ext_active(&ext)) {
            devui_ext_deactivate(&ext);
            touch_input_clear_taps(&touch);
            need_render = 1;
        }
        if (ext_ok && !g_lock_state) {
            int ext_changed = devui_ext_poll(&ext, now);
            if (ext_changed) {
                dragging = 0; drag_dir = 0; scroll_dir = 0; sliding = 0; segging = 0;
                scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
                menu = 0; g_modal = 0; g_sms_open = -1;
                touch_input_clear_taps(&touch);
                if (devui_ext_active(&ext)) {
                    refresh_status_cache();
                    if (backlight_is_on()) {
                        render_ext_view(&disp, &ext);
                        animating = 1;
                    }
                } else {
                    need_render = 1;
                }
            }
        }

        /* power key */
        int ev = key_input_poll(&key, now);
        if (ev == KEY_EV_SHORT || ev == KEY_EV_LONG) last_act = now;
        if (ev == KEY_EV_SHORT) {
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
            if (backlight_is_on()) {
                screen_off(&disp);
                if (!g_charge_boot && lock_enabled()) {
                    if (ext_ok) devui_ext_deactivate(&ext);
                    enter_lock(0);   /* power key locks the screen */
                }
            } else {
                if (ext_ok && devui_ext_active(&ext) && !g_lock_state)
                    screen_on_ext(&disp, &ext);
                else
                    screen_on(&disp, g_charge_boot ? NULL : CUR_PATH);
            }
        } else if (ev == KEY_EV_LONG) {
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
            if (ext_ok && devui_ext_active(&ext) && !g_lock_state) {
                if (!backlight_is_on()) screen_on_ext(&disp, &ext);
                else { devui_ext_deactivate(&ext); need_render = 1; }
            } else if (g_charge_boot) {
                if (!backlight_is_on()) screen_on(&disp, NULL);
            } else if (g_lock_state) {               /* no power menu while locked */
                if (!backlight_is_on()) screen_on(&disp, CUR_PATH);
            } else {
                menu = !menu; g_pwr_confirm = 0;
                if (!backlight_is_on()) screen_on(&disp, CUR_PATH);
                else need_render = 1;
            }
        }

        if (g_charge_boot) {
            if (live_changed && data_backend_commit_latest()) {
                state_pending = 0;
                need_render = 1;
            }
            touch_input_clear_taps(&touch);
            prev_press = 0;
            if (now - last_data >= 120) { need_render = 1; last_data = now; }
            if (need_render && backlight_is_on()) render_charge_boot(&disp);
            was_on = backlight_is_on();
            if (!animating) usleep(backlight_is_on() ? IDLE_SLEEP_ON_US : IDLE_SLEEP_OFF_US);
            continue;
        }

        /* touch: follow-finger swipe (pages) / drag (scroll) + tap actions */
        int x, y, pressed;
        touch_input_read(&touch, &x, &y, &pressed);

        int on_now = backlight_is_on();
        if (on_now && !was_on) {          /* first frame after waking: drop the wake touch */
            touch_input_clear_taps(&touch);
            pressed = 0;
        }
        if (pressed && on_now) last_act = now;

        if (!on_now) {
            /* screen off: ignore touch entirely and discard any taps the panel
             * reported while dark, so they can't replay on wake. Power key is the
             * only way to wake (double-tap-to-wake removed). */
            touch_input_clear_taps(&touch);
            pressed = 0;
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
        } else if (g_lock_state == 1) {
            /* locked preview: page 1 with live data, no actions. A swipe in any
             * direction opens the PIN pad. */
            if (pressed && !prev_press) { down_x = x; down_y = y; }
            else if (pressed && prev_press) {
                int dx = x - down_x, dy = y - down_y;
                if (dx * dx + dy * dy > 22 * 22) {
                    g_lock_state = 3; g_pin_entry[0] = 0; g_lock_err = 0; g_scroll = 0;
                    need_render = 1;
                }
            }
        } else if (g_lock_state) {
            /* PIN pad (3=unlock / 2=setup): tap only, no confirm key; a 4th digit
             * auto-submits. Drain the tap queue so a fast burst of taps all land
             * (the per-digit re-render would otherwise let polls miss presses). */
            int tx, ty;
            while (g_lock_state && touch_input_take_tap(&touch, &tx, &ty)) {
                const char *act = html_view_click((float)tx, (float)ty);
                if (strncmp(act, "act:", 4)) continue;
                const char *a = act + 4;
                if (!strncmp(a, "pin:", 4)) {
                    const char *k = a + 4;
                    int len = (int)strlen(g_pin_entry);
                    if (!strcmp(k, "del")) {
                        if (len > 0) g_pin_entry[len - 1] = 0;
                        g_lock_err = 0;
                    } else if (k[0] >= '0' && k[0] <= '9' && !k[1]) {
                        if (len < 4) { g_pin_entry[len] = k[0]; g_pin_entry[len + 1] = 0; len++; g_lock_err = 0; }
                        if (len == 4) {                       /* auto-submit */
                            if (g_lock_state == 2) {          /* setup: store + enable */
                                save_pin(g_pin_entry);
                                g_pin_entry[0] = 0; g_lock_state = 0;
                            } else if (!strcmp(g_pin_entry, g_pin)) {   /* unlock ok */
                                g_pin_entry[0] = 0; g_lock_err = 0; g_lock_state = 0;
                            } else {                          /* wrong: keep pad, show error */
                                g_pin_entry[0] = 0; g_lock_err = 1;
                            }
                        }
                    }
                    need_render = 1;
                } else if (!strcmp(a, "lockcancel")) {        /* abort PIN setup */
                    g_pin_entry[0] = 0; g_lock_err = 0; g_lock_state = 0; need_render = 1;
                }
            }
        } else if (ext_ok && devui_ext_active(&ext)) {
            if (!pressed) {
                int sx, sy, ex, ey;
                while (devui_ext_active(&ext) &&
                       touch_input_take_stroke(&touch, &sx, &sy, &ex, &ey)) {
                    int dx = ex - sx, dy = ey - sy;
                    int ady = dy < 0 ? -dy : dy;
                    if (sx <= EXT_BACK_EDGE_PX && sy >= ext.content_y &&
                        dx > EXT_BACK_COMMIT_PX && ady <= EXT_BACK_MAX_DY) {
                        devui_ext_deactivate(&ext);
                        touch_input_clear_taps(&touch);
                        need_render = 1;
                        break;
                    }
                }
            }

            if (devui_ext_active(&ext) && !pressed) {
                int tx, ty, cx, cy;
                while (touch_input_take_tap(&touch, &tx, &ty))
                    if (devui_ext_content_point(&ext, tx, ty, &cx, &cy))
                        devui_ext_handle_tap(&ext, cx, cy, now);
            }
            dragging = 0; drag_dir = 0; scroll_dir = 0; sliding = 0; segging = 0;
            scroll_inertia = 0; scroll_track_valid = 0; scroll_v = 0.0f;
        } else if (g_sms_open >= 0) {
            /* SMS detail dialog: any tap (close button or outside) dismisses */
            if (!pressed && prev_press) { g_sms_open = -1; need_render = 1; }
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
            if (pressed && scroll_inertia) {
                scroll_inertia = 0;
                scroll_track_valid = 0;
                scroll_v = 0.0f;
            }
            if (scroll_inertia && !pressed && !dragging && !menu && maxs > 0) {
                uint32_t dtm = now > scroll_track_ms ? now - scroll_track_ms : 0;
                if (dtm > 0) {
                    float dt = dtm / 1000.0f;
                    float dv = SCROLL_INERTIA_DECEL * dt;
                    if (scroll_v > 0.0f) {
                        scroll_v -= dv;
                        if (scroll_v < 0.0f) scroll_v = 0.0f;
                    } else if (scroll_v < 0.0f) {
                        scroll_v += dv;
                        if (scroll_v > 0.0f) scroll_v = 0.0f;
                    }
                    scroll_pos += scroll_v * dt;
                    int ns = (int)(scroll_pos + (scroll_pos >= 0.0f ? 0.5f : -0.5f));
                    if (ns < 0) { ns = 0; scroll_pos = 0.0f; scroll_v = 0.0f; }
                    if (ns > maxs) { ns = maxs; scroll_pos = (float)maxs; scroll_v = 0.0f; }
                    if (ns != g_scroll) { g_scroll = ns; scroll_blit(&disp, g_scroll); }
                    if (scroll_v == 0.0f || ns == 0 || ns == maxs) scroll_inertia = 0;
                    scroll_track_ms = now;
                    animating = 1;
                }
            }
            if (pressed && !prev_press) {
                down_x = x; down_y = y; dragging = 1; drag_dir = 0; scroll_dir = 0; scroll_start = g_scroll; sliding = 0; segging = 0;
                drag_down_ms = now;
                scroll_track_valid = 0; scroll_v = 0.0f;
                int bx, by, bw, bh;   /* grab the brightness slider / segmented control if pressed on it */
                if (html_view_rect("#bright-bar", &bx, &by, &bw, &bh) &&
                    x >= bx && x < bx + bw && y >= by - 5 && y < by + bh + 5) {
                    sliding = 1; bar_x = bx; bar_w = bw;
                    set_bright_x(x, bx, bw); render(&disp, CUR_PATH); animating = 1;
                } else {   /* segmented controls: net mode / auto-off / refresh */
                    int which = 0, n = 4;
                    if (html_view_rect("#netseg", &bx, &by, &bw, &bh) &&
                        x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 1; n = 4; }
                    else if (html_view_rect("#autoseg", &bx, &by, &bw, &bh) &&
                             x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 2; n = 6; }
                    else if (html_view_rect("#refreshseg", &bx, &by, &bw, &bh) &&
                             x >= bx && x < bx + bw && y >= by - 4 && y < by + bh + 4) { which = 3; n = 4; }
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
                    if (g_npages > 1 && adx > DRAG_START_PX && adx > ady) {
                        drag_dir = dx < 0 ? 1 : -1;
                        drag_target = (g_cur + (drag_dir > 0 ? 1 : g_npages - 1)) % g_npages;
                        prep_pair(drag_target, drag_dir);
                    } else if (ady > DRAG_START_PX && ady >= adx && maxs > 0) {
                        scroll_dir = 1; scroll_start = g_scroll;
                        int bufh = g_page_h > SCROLLMAX ? SCROLLMAX : g_page_h;  /* prerender once */
                        const char *hp = page_html(CUR_PATH);
                        int rh = hp ? html_view_render_tall(g_scrollbuf, hp, bufh) : g_page_h;
                        g_scroll_h = rh > bufh ? bufh : rh;
                        maxs = g_scroll_h > H ? g_scroll_h - H : 0;
                        scroll_track_pos = g_scroll;
                        scroll_track_ms = now;
                        scroll_track_valid = 0;
                        scroll_v = 0.0f;
                    } else if (ady > DRAG_CANCEL_PX) dragging = 0;
                }
                if (drag_dir != 0) {
                    compose_frame(&disp, drag_dir > 0 ? -dx : (W - dx));
                    animating = 1;
                } else if (scroll_dir != 0) {
                    int ns = scroll_start - dy;
                    if (ns < 0) ns = 0; if (ns > maxs) ns = maxs;
                    if (ns != g_scroll) {
                        if (scroll_track_ms && now > scroll_track_ms) {
                            float inst = (ns - scroll_track_pos) * 1000.0f / (float)(now - scroll_track_ms);
                            scroll_v = scroll_track_valid ? (scroll_v * 0.65f + inst * 0.35f) : inst;
                            scroll_v = clamp_scroll_v(scroll_v);
                            scroll_track_valid = 1;
                        }
                        g_scroll = ns;
                        scroll_pos = (float)g_scroll;
                        scroll_track_pos = g_scroll;
                        scroll_track_ms = now;
                        scroll_blit(&disp, g_scroll);
                    }
                    animating = 1;
                }
                }
            }
            else if (!pressed && prev_press) {
                int dx = x - down_x;
                if (sliding) { save_conf(); /* persist brightness after drag */ }
                else if (segging) {   /* snap to nearest cell + apply */
                    int c = clampi((x - seg_x) * seg_n / (seg_w > 0 ? seg_w : 1), 0, seg_n - 1);
                    if (seg_which == 1) {
                        char cmd[160];
                        snprintf(cmd, sizeof cmd,
                            "ubus call zte_nwinfo_api nwinfo_set_netselect '{\"net_select\":\"%s\"}' >/dev/null 2>&1 &", g_netmodes[c].v);
                        system(cmd);
                        snprintf(g_net_pending, sizeof g_net_pending, "%s", g_netmodes[c].v);
                    } else if (seg_which == 2) {
                        g_autooff_ms = g_autooffs[c].ms; last_act = now; save_conf();
                    } else {
                        g_refresh_ms = normalize_refresh_ms(g_refresh_rates[c].ms);
                        if (g_refresh_ms > 0) {
                            if (data_backend_commit_latest()) need_render = 1;
                            state_pending = 0;
                        }
                        last_data = 0;
                        save_conf();
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
                    /* After a page-swipe preview or snap-back, the on-screen pixels no
                     * longer match the cached HTML of the current page. Force a full
                     * re-render so native-drawn content from the neighbor page (for
                     * example charts) cannot leak onto the restored page. */
                    invalidate_render_html_cache();
                    need_render = 1;
                } else if (dragging && scroll_dir != 0) {   /* scroll settled */
                    uint32_t gdt = (drag_down_ms && now > drag_down_ms) ? (now - drag_down_ms) : 0;
                    if (gdt > 0) {
                        float rel_v = (-(float)(y - down_y)) * 1000.0f / (float)gdt;
                        if (scroll_track_valid) scroll_v = clamp_scroll_v(scroll_v * 0.55f + rel_v * 0.45f);
                        else                    scroll_v = clamp_scroll_v(rel_v);
                    }
                    if (fabsf(scroll_v) >= SCROLL_INERTIA_MIN_V && maxs > 0) {
                        scroll_inertia = 1;
                        scroll_pos = (float)g_scroll;
                        scroll_track_ms = now;
                    } else {
                        scroll_inertia = 0;
                        scroll_v = 0.0f;
                    }
                    need_render = 1;
                } else if (dragging) {                      /* tap -> action */
                    const char *act = html_view_click((float)x, (float)y);
                    if (!strncmp(act, "act:", 4)) {
                        const char *a = act + 4;
                        if (!strcmp(a, "poweroff")) {
                            if (g_pwr_confirm == 1) {        /* confirmed: blank first (instant feedback), then power off */
                                screen_off(&disp); system("poweroff");
                            } else { g_pwr_confirm = 1; g_pwr_until = now + 4000; need_render = 1; }
                        }
                        else if (!strcmp(a, "reboot")) {
                            if (g_pwr_confirm == 2) { screen_off(&disp); system("reboot"); }
                            else { g_pwr_confirm = 2; g_pwr_until = now + 4000; need_render = 1; }
                        }
                        else if (!strcmp(a, "close"))     { menu = 0; g_pwr_confirm = 0; need_render = 1; }
                        else if (!strcmp(a, "menu"))      { backlight_on(); menu = 1; g_pwr_confirm = 0; need_render = 1; }
                        else if (!strcmp(a, "theme"))     { g_theme = !g_theme; save_conf(); need_render = 1; }
                        else if (!strcmp(a, "revealkey")) { g_show_key = !g_show_key; need_render = 1; }
                        else if (!strcmp(a, "revealcell")) { g_show_cellid = !g_show_cellid; need_render = 1; }
                        else if (!strcmp(a, "revealimei")) { g_show_imei = !g_show_imei; need_render = 1; }
                        else if (!strcmp(a, "refreshambr")) {
                            system("(kill -USR1 $(pidof zwrt-datad 2>/dev/null) >/dev/null 2>&1) || true");
                            snprintf(g_toast, sizeof g_toast, "已请求刷新 AMBR");
                            g_toast_until = now + 1600;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "spunit"))    { g_speed_bits = !g_speed_bits; save_conf(); need_render = 1; }
                        else if (!strcmp(a, "batpct"))    { g_show_batpct = !g_show_batpct; save_conf(); need_render = 1; }
                        else if (!strcmp(a, "nfc")) {
                            static devui_data_t dd; char cmd[120];
                            int on = (data_refresh_live(&dd) && dd.nfc_switch) ? 0 : 1;
                            snprintf(cmd, sizeof cmd,
                                "ubus call zwrt_nfc zwrt_nfc_wifi_set '{\"switch\":%d,\"flag\":2}'", on);
                            system(cmd);
                            need_render = 1;
                        }
                        else if (!strcmp(a, "wifi")) {   /* master: both main bands (wlan0+wlan2) */
                            int on = (g_w24 == 1 || g_w5 == 1);
                            const char *ic = on ? "down" : "up";
                            char cmd[160];
                            snprintf(cmd, sizeof cmd,
                                "(ifconfig wlan0 %s; ifconfig wlan2 %s) >/dev/null 2>&1 &", ic, ic);
                            system(cmd);
                            g_w24 = g_w5 = !on;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "wifi24") || !strcmp(a, "wifi5")) {
                            int is24 = !strcmp(a, "wifi24");
                            int on = (is24 ? g_w24 : g_w5) == 1;
                            char cmd[120];
                            snprintf(cmd, sizeof cmd, "ifconfig %s %s >/dev/null 2>&1 &",
                                     is24 ? "wlan0" : "wlan2", on ? "down" : "up");
                            system(cmd);
                            if (is24) g_w24 = !on; else g_w5 = !on;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "dps")) {   /* power direct-supply mode */
                            int on = (g_dps == 1);
                            char cmd[160];
                            snprintf(cmd, sizeof cmd,
                                "ubus call zwrt_bsp.charger set '{\"direct_power_supply_mode\":\"%s\"}' >/dev/null 2>&1 &",
                                on ? "disable" : "enable");
                            system(cmd);
                            g_dps = !on;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "psm")) {
                            int turn_on = g_wpsm == 1 ? 0 : 1;   /* power_save on = battery saving */
                            const char *m = turn_on ? "on" : "off";
                            /* The switch owns /etc/hotplug.d/iface/99-disable-powersave: it writes
                             * the script itself (re-applying the chosen mode on every ifup) and
                             * applies it now, so the feature is self-contained; nothing needs to be
                             * pre-installed. (Also drop the legacy "psm" file from older builds, which
                             * sorts later and would otherwise override this one.) */
                            char cmd[700];
                            snprintf(cmd, sizeof cmd,
                                "(mkdir -p /etc/hotplug.d/iface; rm -f /etc/hotplug.d/iface/psm; "
                                "{ echo '#!/bin/sh'; echo '[ \"$ACTION\" = ifup ] && {'; "
                                "echo '  iw dev wlan0 set power_save %s 2>/dev/null'; "
                                "echo '  iw dev wlan1 set power_save %s 2>/dev/null'; "
                                "echo '  iw dev wlan2 set power_save %s 2>/dev/null'; "
                                "echo '  iw dev wlan3 set power_save %s 2>/dev/null'; echo '}'; } "
                                "> /etc/hotplug.d/iface/99-disable-powersave; "
                                "chmod +x /etc/hotplug.d/iface/99-disable-powersave; "
                                "for w in wlan0 wlan1 wlan2 wlan3; do iw dev $w set power_save %s 2>/dev/null; done) "
                                ">/dev/null 2>&1 &",
                                m, m, m, m, m);
                            system(cmd);
                            g_wpsm = turn_on;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "adb")) {
                            static devui_data_t dd;
                            const char *mode = "debug";
                            if (data_refresh_live(&dd) && !strcmp(dd.usb_mode, "debug") &&
                                !usb_pid_is("90b1") && !usb_pid_is("90B1"))
                                mode = "user";
                            g_adb_pending = !strcmp(mode, "debug");
                            char cmd[160];
                            if (g_adb_pending)
                                snprintf(cmd, sizeof cmd,
                                    "/sbin/usb_composition 9059 n n y y >/dev/null 2>&1 &");
                            else
                                snprintf(cmd, sizeof cmd,
                                    "ubus call zwrt_bsp.usb set '{\"mode\":\"user\"}' >/dev/null 2>&1 &");
                            system(cmd);
                            need_render = 1;
                        }
                        else if (!strcmp(a, "usbpower")) {
                            if (g_typec_pending >= 0 && now < g_typec_until) {
                                need_render = 1;
                                goto action_done;
                            }
                            int cur = g_typec_source == 1;
                            int target = !cur;
                            int net_active = ((g_usb_net_pending >= 0 ? g_usb_net_pending : g_usb_net) == 1 ||
                                              usb_pid_is("9057") ||
                                              usb_pid_is("90b1") || usb_pid_is("90B1"));
                            if (net_active) {
                                system("touch /tmp/u60-usbnet-switching");
                                if (target) usb_net_apply_source_async();
                                else        usb_net_apply_sink_async();
                                usb_net_watchdog_start();
                                g_usb_net = 1;
                            } else {
                                usb_power_only_apply_async(target);
                            }
                            g_typec_source = target;
                            g_typec_pending = target;
                            g_typec_until = now + 20000;
                            need_render = 1;
                        }
                        else if (!strcmp(a, "usbnet")) {
                            char cmd[900];
                            int cur = g_usb_net_pending >= 0 ? g_usb_net_pending :
                                      ((g_usb_net == 1) || usb_pid_is("9057") ||
                                       usb_pid_is("90b1") || usb_pid_is("90B1"));
                            int target = !cur;
                            if (g_usb_net_pending >= 0 && now < g_usb_net_until) {
                                need_render = 1;
                                goto action_done;
                            }
                            if (!target) {
                                snprintf(cmd, sizeof cmd,
                                    "(rm -f /tmp/u60-usbnet-switching; "
                                    "SER=$(cat /sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber 2>/dev/null); "
                                    "sh /sbin/usb/compositions/usb_switch 0x19d2 0x1225 mass_storage \"$SER\") >/dev/null 2>&1 &");
                                g_usb_net = 0;
                                g_usb_net_pending = 0;
                                g_usb_net_until = now + 20000;
                                usb_net_watchdog_stop();
                                system(cmd);
                            } else {
                                int want_source = g_typec_pending >= 0 ? g_typec_pending : (g_typec_source == 1);
                                g_usb_net = 1;
                                g_typec_source = want_source;
                                g_usb_net_pending = 1;
                                g_typec_pending = want_source;
                                g_usb_net_until = now + 20000;
                                g_typec_until = now + 20000;
                                system("touch /tmp/u60-usbnet-switching");
                                usb_net_watchdog_start();
                                if (want_source) usb_net_apply_source_async();
                                else             usb_net_apply_sink_async();
                            }
                            need_render = 1;
                        }
                        else if (!strcmp(a, "bright")) {    /* tap position on the bar = level */
                            int bx, by, bw, bh;
                            if (html_view_rect("#bright-bar", &bx, &by, &bw, &bh) && bw > 0) {
                                int frac = (x - bx) * 100 / bw;
                                if (frac < 3) frac = 3; if (frac > 100) frac = 100;
                                int bmax = backlight_max(); if (bmax <= 0) bmax = 255;
                                backlight_set(frac * bmax / 100);
                                save_conf();   /* persist brightness */
                            }
                            need_render = 1;
                        }
                        else if (!strncmp(a, "autooff:", 8)) {   /* preset select */
                            g_autooff_ms = atoi(a + 8); last_act = now; save_conf(); need_render = 1;
                        }
                        else if (!strncmp(a, "refreshms:", 10)) {   /* preset select */
                            g_refresh_ms = normalize_refresh_ms(atoi(a + 10));
                            if (g_refresh_ms > 0) {
                                if (data_backend_commit_latest()) need_render = 1;
                                state_pending = 0;
                            }
                            last_data = 0;
                            save_conf();
                            need_render = 1;
                        }
                        else if (!strcmp(a, "locktoggle")) {   /* on->off clears PIN; off->on opens setup pad */
                            if (lock_enabled()) clear_pin();
                            else                enter_lock(1);
                            need_render = 1;
                        }
                        else if (!strncmp(a, "net:", 4)) {   /* segmented control tap fallback */
                            char cmd[160];
                            snprintf(cmd, sizeof cmd,
                                "ubus call zte_nwinfo_api nwinfo_set_netselect '{\"net_select\":\"%s\"}' >/dev/null 2>&1 &", a + 4);
                            system(cmd);
                            snprintf(g_net_pending, sizeof g_net_pending, "%s", a + 4);
                            snprintf(g_toast, sizeof g_toast, "网络模式已切换"); g_toast_until = now + 1600;
                        }
                        else if (!strncmp(a, "openmodal:", 10)) {   /* open band-lock dialog */
                            const char *r = a + 10;
                            g_modal = !strcmp(r, "sa") ? 1 : !strcmp(r, "nsa") ? 2 : 3;
                            need_render = 1;
                        }
                        else if (!strncmp(a, "sms:", 4)) {   /* open SMS detail + mark read */
                            const char *id_txt = a + 4;
                            char *endp = NULL;
                            long sid = strtol(id_txt, &endp, 10);
                            static devui_data_t sd;
                            g_sms_open = -1;
                            g_sms_num[0] = g_sms_date[0] = g_sms_text[0] = 0;
                            need_render = 1;
                            if (id_txt != endp && endp && *endp == '\0' && data_refresh(&sd)) {
                                if (sid > 0) {
                                    for (int i = 0; i < sd.sms_n; i++) {
                                        if (sd.sms[i].id != sid) continue;
                                        snprintf(g_sms_num,  sizeof g_sms_num,  "%s", sd.sms[i].num);
                                        snprintf(g_sms_date, sizeof g_sms_date, "%s", sd.sms[i].date);
                                        snprintf(g_sms_text, sizeof g_sms_text, "%s", sd.sms[i].text);
                                        g_sms_open = sid;
                                        if (sd.sms[i].unread && sd.sms[i].id > 0) {
                                            char cmd[128];
                                            snprintf(cmd, sizeof cmd,
                                                "ubus call zwrt_wms zwrt_wms_modify_tag '{\"id\":\"%ld;\",\"tag\":0}' >/dev/null 2>&1 &",
                                                sd.sms[i].id);
                                            system(cmd);
                                        }
                                        need_render = 1;
                                        break;
                                    }
                                }
                            }
                        }
                        else if (!strcmp(a, "resetband")) {
                            system("ubus call zte_nwinfo_api nwinfo_reset_band_cell_setting '{}' >/dev/null 2>&1 &");
                            snprintf(g_toast, sizeof g_toast, "锁频已恢复默认"); g_toast_until = now + 1600;
                            need_render = 1;   /* selection re-syncs from the live lock automatically */
                        }
                    }
                }
action_done:
                dragging = 0; drag_dir = 0; scroll_dir = 0; sliding = 0; segging = 0; g_segdrag = 0;
                scroll_track_valid = 0;
            }
        }
        prev_press = pressed;

        /* when the lock pad first appears, drop taps that were queued before it
         * (e.g. the toggle tap that opened it) so they don't phantom-press keys */
        if (g_lock_state != prev_lock) touch_input_clear_taps(&touch);
        prev_lock = g_lock_state;

        /* dismiss the toast after its timeout */
        if (g_toast[0] && now >= g_toast_until) { g_toast[0] = 0; need_render = 1; }

        /* power-menu armed state auto-reverts if the second tap doesn't come */
        if (g_pwr_confirm && now >= g_pwr_until) { g_pwr_confirm = 0; if (menu) need_render = 1; }

        /* auto screen-off after the configured idle timeout (locks if enabled) */
        if (g_autooff_ms > 0 && backlight_is_on() && now - last_act >= (uint32_t)g_autooff_ms) {
            screen_off(&disp);
            if (lock_enabled()) {
                if (ext_ok) devui_ext_deactivate(&ext);
                enter_lock(0);
            }
        }

        /* periodic clock/state check (not mid-drag). refresh=0 pauses promotion of
         * live backend updates, but the minute clock still ticks once per second. */
        {
            uint32_t poll_ms = g_refresh_ms > 0 ? (uint32_t)g_refresh_ms : 1000;
            if (!dragging && !scroll_inertia && now - last_data >= poll_ms) {
                time_t now_t = time(NULL);
                int state_changed = 0;
                int clock_changed = 0;
                int state_render = 0;
                int cur_min = (int)(now_t / 60);
                if (cur_min != g_last_clock_min) {
                    g_last_clock_min = cur_min;
                    clock_changed = 1;
                    need_render = 1;   /* update HH:MM and any minute-level derived tokens */
                }

                if (g_refresh_ms > 0 && state_pending && data_backend_commit_latest()) {
                    state_changed = 1;
                    state_render = 1;
                    state_pending = 0;
                }
                if (state_render) need_render = 1;
                if (ext_ok && devui_ext_active(&ext) && !g_lock_state &&
                    (clock_changed || (g_refresh_ms > 0 && state_changed)))
                    refresh_status_cache();
                last_data = now;
            }
        }

        /* reconcile page-2 WiFi switch states from uci/iw on a slow throttle */
        if (!dragging && !scroll_inertia && now - g_wifi_aux_at >= 4000) { g_wifi_aux_at = now; wifi_aux_refresh(); }

        if (need_render && backlight_is_on()) {
            if (ext_ok && devui_ext_active(&ext) && !g_lock_state)
                render_ext_view(&disp, &ext);
            else
                render(&disp, CUR_PATH);
        }
        was_on = backlight_is_on();   /* track for next frame's wake-touch discard */
        if (!animating) usleep(backlight_is_on() ? IDLE_SLEEP_ON_US : IDLE_SLEEP_OFF_US);
    }

    if (ext_ok) devui_ext_close(&ext);
    data_backend_close();
    drm_disp_close(&disp);
    return 0;
}
