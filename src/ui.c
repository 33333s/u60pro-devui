/*
 * ui.c - U60Pro multi-page dashboard (LVGL tileview).
 *
 * Pages swipe horizontally: [Home] [Network]. Live values come from u60-datad
 * via data.c, refreshed at 1 Hz. Power key: short = backlight on/off,
 * long = power menu. No ubus here.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ui.h"
#include "data.h"
#include "key_input.h"
#include "touch_input.h"
#include "backlight.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned long g_frame_count;   /* defined in main.c */

/* ---- shared widget handles ---- */
/* Home page */
static lv_obj_t *s_operator, *s_nettype, *s_sig_seg[5], *s_sig_detail;
static lv_obj_t *s_bat_bar, *s_bat_detail, *s_down, *s_up, *s_clients, *s_sys;
/* Network page */
static lv_obj_t *s_n_band, *s_n_nrsig, *s_n_cell, *s_n_plmn, *s_n_lte, *s_n_wan;
/* WiFi page */
static lv_obj_t *s_w_ssid, *s_w_pass, *s_w_enc, *s_w_qr;
/* Settings page */
static lv_obj_t *s_set_bright, *s_off_btn[3];
static uint32_t  s_autooff_ms = 0;   /* 0 = never */
static int       s_auto_slept = 0;
/* CJK fonts loaded from the device at runtime (NULL if unavailable).
 * FCN/FCN_S resolve to the CJK font, or Montserrat as a fallback. */
static lv_font_t *s_cjk, *s_cjk16;
static const lv_font_t *FCN, *FCN_S;
#ifndef DEVUI_CJK_FONT
#define DEVUI_CJK_FONT "/usr/ui/fonts/ZTEZhengYuan.ttf"
#endif
/* Test page */
static lv_obj_t *s_t_fps, *s_t_touch, *s_t_box;
static lv_timer_t *s_bench_timer;
static int       s_box_x = 0, s_box_dir = 1;
/* nav + power */
#define UI_PAGES 5
#define PAGE_TEST 4
static lv_obj_t   *s_tv, *s_tiles[UI_PAGES], *s_dots[UI_PAGES];
static key_input_t s_key;
static lv_obj_t   *s_power_menu;

/* ---- formatting helpers ---- */
static void fmt_rate(char *out, size_t n, long bps)
{
    if (bps >= 1024 * 1024) snprintf(out, n, "%.1f MB/s", bps / 1048576.0);
    else if (bps >= 1024)   snprintf(out, n, "%.1f KB/s", bps / 1024.0);
    else                    snprintf(out, n, "%ld B/s", bps);
}
static lv_obj_t *mklabel(lv_obj_t *parent, int x, int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    lv_label_set_text(l, "");
    return l;
}
static void mkheader(lv_obj_t *parent, const char *txt, int y)
{
    lv_obj_t *l = mklabel(parent, 12, y, &lv_font_montserrat_14, 0x7a8694);
    lv_label_set_text(l, txt);
}
/* Chinese section header (uses the CJK font). */
static void mkheader_cn(lv_obj_t *parent, const char *txt, int y)
{
    lv_obj_t *l = mklabel(parent, 12, y, FCN_S, 0x7a8694);
    lv_label_set_text(l, txt);
}
/* A page title in the CJK font. */
static void mktitle_cn(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, FCN, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x4ea1ff), 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 10);
}

/* ---- page builders ---- */
static void build_home(lv_obj_t *t)
{
    s_operator = lv_label_create(t);
    lv_obj_set_style_text_font(s_operator, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_operator, lv_color_hex(0x4ea1ff), 0);
    lv_obj_align(s_operator, LV_ALIGN_TOP_MID, 0, 10);
    lv_label_set_text(s_operator, "…");

    s_nettype = lv_label_create(t);
    lv_obj_set_style_text_font(s_nettype, &lv_font_montserrat_16, 0);
    lv_obj_align(s_nettype, LV_ALIGN_TOP_MID, 0, 48);
    lv_label_set_text(s_nettype, "");

    mkheader_cn(t, "信号", 84);
    for (int i = 0; i < 5; i++) {              /* 5 rising segment bars */
        s_sig_seg[i] = lv_obj_create(t);
        lv_obj_remove_style_all(s_sig_seg[i]);
        int h = 10 + i * 5;
        lv_obj_set_size(s_sig_seg[i], 20, h);
        lv_obj_set_style_radius(s_sig_seg[i], 2, 0);
        lv_obj_set_style_bg_opa(s_sig_seg[i], LV_OPA_COVER, 0);
        lv_obj_align(s_sig_seg[i], LV_ALIGN_TOP_LEFT, 14 + i * 26, 128 - h);
    }
    s_sig_detail = mklabel(t, 12, 134, &lv_font_montserrat_14, 0xc0c8d0);

    mkheader_cn(t, "电池", 156);
    s_bat_bar = lv_bar_create(t);
    lv_obj_set_size(s_bat_bar, 296, 14);
    lv_obj_align(s_bat_bar, LV_ALIGN_TOP_MID, 0, 176);
    s_bat_detail = mklabel(t, 12, 196, &lv_font_montserrat_16, 0xc0c8d0);

    mkheader_cn(t, "流量", 232);
    s_down = mklabel(t, 12, 254, &lv_font_montserrat_20, 0x4caf50);
    s_up   = mklabel(t, 12, 284, &lv_font_montserrat_20, 0xffa040);

    s_clients = mklabel(t, 12, 330, &lv_font_montserrat_16, 0xc0c8d0);
    s_sys     = mklabel(t, 12, 360, &lv_font_montserrat_14, 0x9aa4ae);
}

static void build_net(lv_obj_t *t)
{
    mktitle_cn(t, "网络");

    mkheader(t, "5G NR", 52);
    s_n_band  = mklabel(t, 12, 74,  &lv_font_montserrat_16, 0xd0d8e0);
    s_n_nrsig = mklabel(t, 12, 100, &lv_font_montserrat_14, 0xc0c8d0);
    s_n_cell  = mklabel(t, 12, 124, &lv_font_montserrat_14, 0xc0c8d0);
    s_n_plmn  = mklabel(t, 12, 148, &lv_font_montserrat_14, 0xc0c8d0);

    mkheader(t, "LTE", 188);
    s_n_lte   = mklabel(t, 12, 210, &lv_font_montserrat_14, 0xc0c8d0);

    mkheader(t, "WAN", 250);
    s_n_wan   = mklabel(t, 12, 272, &lv_font_montserrat_16, 0xa0ffa0);
}

static void build_wifi(lv_obj_t *t)
{
    lv_obj_t *title = lv_label_create(t);
    lv_label_set_text(title, "WIFI");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x4ea1ff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_w_ssid = lv_label_create(t);
    lv_obj_set_style_text_font(s_w_ssid, &lv_font_montserrat_20, 0);
    lv_obj_align(s_w_ssid, LV_ALIGN_TOP_MID, 0, 42);

    s_w_pass = lv_label_create(t);
    lv_obj_set_style_text_font(s_w_pass, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_w_pass, lv_color_hex(0xc0c8d0), 0);
    lv_obj_align(s_w_pass, LV_ALIGN_TOP_MID, 0, 74);

    s_w_qr = lv_qrcode_create(t);
    lv_qrcode_set_size(s_w_qr, 184);
    lv_qrcode_set_dark_color(s_w_qr, lv_color_black());
    lv_qrcode_set_light_color(s_w_qr, lv_color_white());
    lv_obj_set_style_border_color(s_w_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_w_qr, 6, 0);    /* white quiet zone */
    lv_obj_align(s_w_qr, LV_ALIGN_TOP_MID, 0, 104);

    s_w_enc = lv_label_create(t);
    lv_obj_set_style_text_font(s_w_enc, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_w_enc, lv_color_hex(0x9aa4ae), 0);
    lv_obj_align(s_w_enc, LV_ALIGN_TOP_MID, 0, 306);
}

/* ---- settings page ---- */
static const uint32_t k_off_ms[3] = { 0, 30000, 120000 };  /* Never / 30s / 2m */

static void bright_cb(lv_event_t *e)
{
    backlight_set(lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
}

static void highlight_off_btns(int sel)
{
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_bg_color(s_off_btn[i],
            lv_color_hex(i == sel ? 0x4ea1ff : 0x394049), 0);
}

static void offsel_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_autooff_ms = k_off_ms[idx];
    s_auto_slept = 0;
    highlight_off_btns(idx);
}

static void build_settings(lv_obj_t *t)
{
    mktitle_cn(t, "设置");

    mkheader_cn(t, "亮度", 60);
    s_set_bright = lv_slider_create(t);
    lv_obj_set_size(s_set_bright, 280, 18);
    lv_obj_align(s_set_bright, LV_ALIGN_TOP_MID, 0, 84);
    lv_slider_set_range(s_set_bright, 10, backlight_max());
    lv_slider_set_value(s_set_bright, backlight_get() > 0 ? backlight_get() : backlight_max(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_set_bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

    mkheader_cn(t, "自动息屏", 140);
    static const char *labels[3] = { "常亮", "30秒", "2分钟" };
    for (int i = 0; i < 3; i++) {
        s_off_btn[i] = lv_button_create(t);
        lv_obj_set_size(s_off_btn[i], 92, 56);
        lv_obj_align(s_off_btn[i], LV_ALIGN_TOP_LEFT, 12 + i * 100, 166);
        lv_obj_add_event_cb(s_off_btn[i], offsel_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(s_off_btn[i]);
        lv_obj_set_style_text_font(l, FCN_S, 0);
        lv_label_set_text(l, labels[i]);
        lv_obj_center(l);
    }
    highlight_off_btns(0);

    lv_obj_t *hint = lv_label_create(t);
    lv_obj_set_style_text_font(hint, FCN_S, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9aa4ae), 0);
    lv_label_set_text(hint, "电源键：短按 亮屏/息屏\n长按 电源菜单");
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 12, 250);
}

/* ---- perf test page ---- */
static void build_test(lv_obj_t *t)
{
    mktitle_cn(t, "性能测试");

    mkheader_cn(t, "渲染刷新率", 44);
    s_t_fps = mklabel(t, 12, 64, &lv_font_montserrat_28, 0x4caf50);
    lv_label_set_text(s_t_fps, "-- FPS");

    mkheader_cn(t, "触控上报率", 112);
    s_t_touch = mklabel(t, 12, 132, &lv_font_montserrat_28, 0xffa040);
    lv_label_set_text(s_t_touch, "-- Hz");

    lv_obj_t *hint = lv_label_create(t);
    lv_obj_set_style_text_font(hint, FCN_S, 0);
    lv_label_set_text(hint, "在屏上拖动手指\n测量触控上报率");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9aa4ae), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 12, 186);

    /* a moving box drives continuous full-screen redraws on this page */
    s_t_box = lv_obj_create(t);
    lv_obj_remove_style_all(s_t_box);
    lv_obj_set_size(s_t_box, 40, 40);
    lv_obj_set_style_radius(s_t_box, 6, 0);
    lv_obj_set_style_bg_opa(s_t_box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_t_box, lv_color_hex(0x4ea1ff), 0);
    lv_obj_set_pos(s_t_box, 0, 300);
}

static void bench_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    s_box_x += s_box_dir * 12;
    if (s_box_x >= 280) { s_box_x = 280; s_box_dir = -1; }
    if (s_box_x <= 0)   { s_box_x = 0;   s_box_dir = 1; }
    lv_obj_set_pos(s_t_box, s_box_x, 300);
    lv_obj_invalidate(s_tiles[PAGE_TEST]);  /* force a full-screen redraw */
    lv_refr_now(NULL);                       /* render it now -> counts a frame */
}

static void refresh_wifi(const devui_data_t *d)
{
    static char last_qr[256] = "";
    int open = (strstr(d->wifi_enc, "none") != NULL) || d->wifi_key[0] == 0;
    char qr[256];

    lv_label_set_text(s_w_ssid, d->wifi_ssid[0] ? d->wifi_ssid : "—");
    lv_label_set_text_fmt(s_w_pass, "key: %s", d->wifi_key[0] ? d->wifi_key : "(open)");
    lv_label_set_text(s_w_enc, open ? "open  ·  scan to join" : "WPA  ·  scan to join");

    if (open) snprintf(qr, sizeof qr, "WIFI:T:nopass;S:%s;;", d->wifi_ssid);
    else      snprintf(qr, sizeof qr, "WIFI:T:WPA;S:%s;P:%s;;", d->wifi_ssid, d->wifi_key);

    /* Rebuild the QR bitmap only when the credentials actually change. */
    if (strcmp(qr, last_qr) != 0 && d->wifi_ssid[0]) {
        lv_qrcode_update(s_w_qr, qr, strlen(qr));
        strcpy(last_qr, qr);
    }
}

/* ---- refresh ---- */
static void refresh_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    static devui_data_t d;
    char b1[32];

    /* perf counters (per-second deltas) — always update, even if data is down */
    static unsigned long last_f, last_r;
    unsigned long f = g_frame_count, r = touch_input_report_count();
    lv_label_set_text_fmt(s_t_fps, "%lu FPS", f - last_f);
    lv_label_set_text_fmt(s_t_touch, "%lu Hz", r - last_r);
    last_f = f; last_r = r;

    if (!data_refresh(&d)) {
        lv_label_set_text(s_operator, "zwrt-datad offline");
        lv_label_set_text(s_nettype, "start the backend");
        return;
    }

    /* Home */
    lv_label_set_text(s_operator, d.operator_name[0] ? d.operator_name : "—");
    lv_label_set_text_fmt(s_nettype, "%s  %s", d.net_type, d.band);
    uint32_t sig_col = d.bars <= 1 ? 0xff5040 : d.bars <= 2 ? 0xffa040 : 0x4caf50;
    for (int i = 0; i < 5; i++)
        lv_obj_set_style_bg_color(s_sig_seg[i],
            lv_color_hex(i < d.bars ? sig_col : 0x2a3038), 0);
    lv_label_set_text_fmt(s_sig_detail, "RSRP %d  SNR %s  RSSI %d",
                          d.nr_rsrp, d.nr_snr[0] ? d.nr_snr : "-", d.nr_rssi);
    lv_bar_set_value(s_bat_bar, d.bat_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bat_bar,
        lv_color_hex(d.bat_percent <= 15 ? 0xff5040 : 0x4caf50), LV_PART_INDICATOR);
    lv_label_set_text_fmt(s_bat_detail, "%d%%  %d\xC2\xB0""C %s",
                          d.bat_percent, d.bat_temp, d.charging ? "CHG" : "");
    fmt_rate(b1, sizeof b1, d.rx_speed);
    lv_label_set_text_fmt(s_down, LV_SYMBOL_DOWN " %s", b1);
    fmt_rate(b1, sizeof b1, d.tx_speed);
    lv_label_set_text_fmt(s_up, LV_SYMBOL_UP " %s", b1);
    lv_label_set_text_fmt(s_clients, LV_SYMBOL_WIFI " %d clients (wifi %d / lan %d)",
                          d.clients_total, d.clients_wifi, d.clients_lan);
    long up = d.uptime;
    lv_label_set_text_fmt(s_sys, "up %ldh%02ldm  cpu %ld\xC2\xB0""C  mem %ld%%",
                          up / 3600, (up / 60) % 60, d.cpu_temp, d.mem_used_pct);

    /* Network */
    lv_label_set_text_fmt(s_n_band, "Band %s  BW %s MHz  CH %ld",
                          d.band[0] ? d.band : "-", d.nr_bw[0] ? d.nr_bw : "-", d.nr_channel);
    lv_label_set_text_fmt(s_n_nrsig, "RSRP %d  RSRQ %d  SNR %s  RSSI %d",
                          d.nr_rsrp, d.nr_rsrq, d.nr_snr[0] ? d.nr_snr : "-", d.nr_rssi);
    lv_label_set_text_fmt(s_n_cell, "PCI %d   Cell ID %ld", d.nr_pci, d.nr_cell_id);
    lv_label_set_text_fmt(s_n_plmn, "PLMN %d-%02d   %s", d.mcc, d.mnc, d.operator_name);
    if (d.lte_rsrp != 0)
        lv_label_set_text_fmt(s_n_lte, "RSRP %d  RSRQ %d  RSSI %d  SNR %s",
                              d.lte_rsrp, d.lte_rsrq, d.lte_rssi, d.lte_snr[0] ? d.lte_snr : "-");
    else
        lv_label_set_text(s_n_lte, "not aggregated");
    lv_label_set_text(s_n_wan, d.wan_status[0] ? d.wan_status : "-");

    /* WiFi */
    refresh_wifi(&d);
}

/* ---- power menu ---- */
static void power_menu_set(int v)
{
    if (v) lv_obj_remove_flag(s_power_menu, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(s_power_menu, LV_OBJ_FLAG_HIDDEN);
}
static int power_menu_visible(void) { return !lv_obj_has_flag(s_power_menu, LV_OBJ_FLAG_HIDDEN); }

static void act_poweroff(lv_event_t *e) { LV_UNUSED(e); system("poweroff"); }
static void act_reboot(lv_event_t *e)   { LV_UNUSED(e); system("reboot"); }
static void act_cancel(lv_event_t *e)   { LV_UNUSED(e); power_menu_set(0); }

static void menu_button(lv_obj_t *parent, const char *txt, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(90));
    lv_obj_set_height(btn, 52);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, FCN, 0);
    lv_obj_center(l);
}

static void build_power_menu(void)
{
    s_power_menu = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_power_menu, 280, 320);
    lv_obj_center(s_power_menu);
    lv_obj_set_style_bg_color(s_power_menu, lv_color_hex(0x161b21), 0);
    lv_obj_set_style_border_color(s_power_menu, lv_color_hex(0x4ea1ff), 0);
    lv_obj_set_style_border_width(s_power_menu, 2, 0);
    lv_obj_set_style_radius(s_power_menu, 12, 0);
    lv_obj_set_flex_flow(s_power_menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_power_menu, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(s_power_menu);
    lv_label_set_text(title, "电源");
    lv_obj_set_style_text_font(title, FCN, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9aa4ae), 0);

    menu_button(s_power_menu, "关机", 0xb23b3b, act_poweroff);
    menu_button(s_power_menu, "重启", 0xb2742b, act_reboot);
    menu_button(s_power_menu, "取消", 0x394049, act_cancel);
    power_menu_set(0);
}

static void key_poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    int ev = key_input_poll(&s_key, lv_tick_get());
    if (ev == KEY_EV_SHORT) {
        backlight_toggle();
        s_auto_slept = 0;
    } else if (ev == KEY_EV_LONG) {
        backlight_on();
        s_auto_slept = 0;
        power_menu_set(!power_menu_visible());
    }

    /* Auto screen-off after inactivity; any touch/key wakes it. */
    if (s_autooff_ms) {
        uint32_t idle = lv_display_get_inactive_time(NULL);
        if (idle > s_autooff_ms) {
            if (backlight_is_on()) { backlight_off(); s_auto_slept = 1; }
        } else if (s_auto_slept) {
            backlight_on();
            s_auto_slept = 0;
        }
    }
}

/* ---- page dots ---- */
static lv_obj_t *make_dot(void)
{
    lv_obj_t *d = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, 4, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}
static void update_dots(void)
{
    lv_obj_t *act = lv_tileview_get_tile_active(s_tv);
    for (int i = 0; i < UI_PAGES; i++)
        lv_obj_set_style_bg_color(s_dots[i],
            lv_color_hex(s_tiles[i] == act ? 0x4ea1ff : 0x3a4048), 0);
}
static void tv_changed_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    update_dots();
    /* run the benchmark only while the test page is visible */
    if (s_bench_timer) {
        if (lv_tileview_get_tile_active(s_tv) == s_tiles[PAGE_TEST])
            lv_timer_resume(s_bench_timer);
        else
            lv_timer_pause(s_bench_timer);
    }
}

void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0c0f13), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xe0e0e0), 0);

    /* CJK fonts (loaded from the device; repo ships no proprietary font). */
    lv_freetype_init(LV_FREETYPE_CACHE_FT_GLYPH_CNT);
    s_cjk   = lv_freetype_font_create(DEVUI_CJK_FONT,
                                      LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 22,
                                      LV_FREETYPE_FONT_STYLE_NORMAL);
    s_cjk16 = lv_freetype_font_create(DEVUI_CJK_FONT,
                                      LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 16,
                                      LV_FREETYPE_FONT_STYLE_NORMAL);
    FCN   = s_cjk   ? s_cjk   : &lv_font_montserrat_20;
    FCN_S = s_cjk16 ? s_cjk16 : &lv_font_montserrat_14;

    s_tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(s_tv, lv_color_hex(0x0c0f13), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < UI_PAGES; i++)
        s_tiles[i] = lv_tileview_add_tile(s_tv, i, 0, LV_DIR_HOR);
    build_home(s_tiles[0]);
    build_net(s_tiles[1]);
    build_wifi(s_tiles[2]);
    build_settings(s_tiles[3]);
    build_test(s_tiles[PAGE_TEST]);
    lv_obj_add_event_cb(s_tv, tv_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* benchmark driver — paused until the test page is shown */
    s_bench_timer = lv_timer_create(bench_cb, 1, NULL);
    lv_timer_pause(s_bench_timer);

    /* page indicator dots (top layer, bottom center) */
    for (int i = 0; i < UI_PAGES; i++) {
        s_dots[i] = make_dot();
        lv_obj_align(s_dots[i], LV_ALIGN_BOTTOM_MID, i * 16 - (UI_PAGES - 1) * 8, -6);
    }
    update_dots();

    lv_timer_create(refresh_cb, 1000, NULL);
    refresh_cb(NULL);

    build_power_menu();
    backlight_init();
    key_input_init(&s_key);
    lv_timer_create(key_poll_cb, 50, NULL);
}
