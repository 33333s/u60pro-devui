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
#include "backlight.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>

/* ---- shared widget handles ---- */
/* Home page */
static lv_obj_t *s_operator, *s_nettype, *s_sig_bar, *s_sig_detail;
static lv_obj_t *s_bat_bar, *s_bat_detail, *s_down, *s_up, *s_clients, *s_sys;
/* Network page */
static lv_obj_t *s_n_band, *s_n_nrsig, *s_n_cell, *s_n_plmn, *s_n_lte, *s_n_wan;
/* nav + power */
static lv_obj_t   *s_tv, *s_tile_home, *s_tile_net, *s_dot0, *s_dot1;
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

    mkheader(t, "SIGNAL", 84);
    s_sig_bar = lv_bar_create(t);
    lv_obj_set_size(s_sig_bar, 296, 14);
    lv_obj_align(s_sig_bar, LV_ALIGN_TOP_MID, 0, 104);
    lv_bar_set_range(s_sig_bar, 0, 100);
    lv_obj_set_style_bg_color(s_sig_bar, lv_color_hex(0x4ea1ff), LV_PART_INDICATOR);
    s_sig_detail = mklabel(t, 12, 124, &lv_font_montserrat_14, 0xc0c8d0);

    mkheader(t, "BATTERY", 156);
    s_bat_bar = lv_bar_create(t);
    lv_obj_set_size(s_bat_bar, 296, 14);
    lv_obj_align(s_bat_bar, LV_ALIGN_TOP_MID, 0, 176);
    s_bat_detail = mklabel(t, 12, 196, &lv_font_montserrat_16, 0xc0c8d0);

    mkheader(t, "DATA", 232);
    s_down = mklabel(t, 12, 254, &lv_font_montserrat_20, 0x4caf50);
    s_up   = mklabel(t, 12, 284, &lv_font_montserrat_20, 0xffa040);

    s_clients = mklabel(t, 12, 330, &lv_font_montserrat_16, 0xc0c8d0);
    s_sys     = mklabel(t, 12, 360, &lv_font_montserrat_14, 0x9aa4ae);
}

static void build_net(lv_obj_t *t)
{
    lv_obj_t *title = lv_label_create(t);
    lv_label_set_text(title, "NETWORK");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x4ea1ff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

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

/* ---- refresh ---- */
static void refresh_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    static devui_data_t d;
    char b1[32];

    if (!data_refresh(&d)) {
        lv_label_set_text(s_operator, "u60-datad offline");
        lv_label_set_text(s_nettype, "start the backend");
        return;
    }

    /* Home */
    lv_label_set_text(s_operator, d.operator_name[0] ? d.operator_name : "—");
    lv_label_set_text_fmt(s_nettype, "%s  %s", d.net_type, d.band);
    lv_bar_set_value(s_sig_bar, d.bars * 20, LV_ANIM_OFF);
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
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
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
    lv_label_set_text(title, "POWER");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9aa4ae), 0);

    menu_button(s_power_menu, LV_SYMBOL_POWER " Power Off", 0xb23b3b, act_poweroff);
    menu_button(s_power_menu, LV_SYMBOL_REFRESH " Reboot",  0xb2742b, act_reboot);
    menu_button(s_power_menu, LV_SYMBOL_CLOSE " Cancel",    0x394049, act_cancel);
    power_menu_set(0);
}

static void key_poll_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    int ev = key_input_poll(&s_key, lv_tick_get());
    if (ev == KEY_EV_SHORT) {
        backlight_toggle();
    } else if (ev == KEY_EV_LONG) {
        backlight_on();
        power_menu_set(!power_menu_visible());
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
    int home = (lv_tileview_get_tile_active(s_tv) == s_tile_home);
    lv_obj_set_style_bg_color(s_dot0, lv_color_hex(home ? 0x4ea1ff : 0x3a4048), 0);
    lv_obj_set_style_bg_color(s_dot1, lv_color_hex(home ? 0x3a4048 : 0x4ea1ff), 0);
}
static void tv_changed_cb(lv_event_t *e) { LV_UNUSED(e); update_dots(); }

void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0c0f13), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(0xe0e0e0), 0);

    s_tv = lv_tileview_create(scr);
    lv_obj_set_style_bg_color(s_tv, lv_color_hex(0x0c0f13), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    s_tile_home = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_HOR);
    s_tile_net  = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_HOR);
    build_home(s_tile_home);
    build_net(s_tile_net);
    lv_obj_add_event_cb(s_tv, tv_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* page indicator dots (top layer, bottom center) */
    s_dot0 = make_dot();
    s_dot1 = make_dot();
    lv_obj_align(s_dot0, LV_ALIGN_BOTTOM_MID, -8, -6);
    lv_obj_align(s_dot1, LV_ALIGN_BOTTOM_MID,  8, -6);
    update_dots();

    lv_timer_create(refresh_cb, 1000, NULL);
    refresh_cb(NULL);

    build_power_menu();
    backlight_init();
    key_input_init(&s_key);
    lv_timer_create(key_poll_cb, 50, NULL);
}
