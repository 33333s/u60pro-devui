/*
 * data.c - read device state from the u60-datad snapshot file.
 *
 * SPDX-License-Identifier: MIT
 */
#include "data.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DEVUI_STATE_FILE
#define DEVUI_STATE_FILE "/tmp/u60-datad/state.json"
#endif

#ifndef DEVUI_SMS_BUF_MAX
#define DEVUI_SMS_BUF_MAX 1048576
#endif

#ifndef DEVUI_STATE_BUF_MAX
#define DEVUI_STATE_BUF_MAX 1048576
#endif

static void getstr(const char *obj, const char *key, char *dst, size_t n)
{
    if (!json_get(obj, key, dst, n)) dst[0] = 0;
}

/* Keep parser buffers large enough for long UTF-8 SMS payloads. */
#ifndef DEVUI_SMS_OBJECT_MAX
#define DEVUI_SMS_OBJECT_MAX (DEVUI_SMS_TEXT_MAX + 32768)
#endif

/* p points at '{' — return the matching '}', skipping braces inside strings. */
static char *obj_end(char *p)
{
    int depth = 0, instr = 0;
    for (char *c = p; *c; c++) {
        if (instr) {
            if (*c == '\\' && c[1]) c++;
            else if (*c == '"') instr = 0;
        } else if (*c == '"') instr = 1;
        else if (*c == '{') depth++;
        else if (*c == '}') { if (--depth == 0) return c; }
    }
    return NULL;
}

static char *find_next_object(const char *p)
{
    int instr = 0, esc = 0;
    for (; *p; p++) {
        char c = *p;
        if (instr) {
            if (esc) {
                esc = 0;
            } else if (c == '\\') {
                esc = 1;
            } else if (c == '"') {
                instr = 0;
            }
        } else {
            if (c == '"') instr = 1;
            else if (c == '{') return (char *)p;
        }
    }
    return NULL;
}

int data_refresh(devui_data_t *d)
{
    memset(d, 0, sizeof *d);
    d->cpu_usage = -1;
    d->valid = 0;

    FILE *fp = fopen(DEVUI_STATE_FILE, "r");
    if (!fp) return 0;
    static char buf[DEVUI_STATE_BUF_MAX];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return 0;
    buf[n] = 0;

    static char sec[DEVUI_STATE_BUF_MAX];

    if (json_get(buf, "net", sec, sizeof sec)) {
        getstr(sec, "type", d->net_type, sizeof d->net_type);
        getstr(sec, "operator", d->operator_name, sizeof d->operator_name);
        getstr(sec, "band", d->band, sizeof d->band);
        getstr(sec, "nr_band", d->nr_band, sizeof d->nr_band);
        getstr(sec, "nr_snr", d->nr_snr, sizeof d->nr_snr);
        getstr(sec, "wan_status", d->wan_status, sizeof d->wan_status);
        getstr(sec, "lte_snr", d->lte_snr, sizeof d->lte_snr);
        getstr(sec, "nr_bw", d->nr_bw, sizeof d->nr_bw);
        getstr(sec, "nrca", d->nrca, sizeof d->nrca);
        getstr(sec, "lteca", d->lteca, sizeof d->lteca);
        getstr(sec, "ltecasig", d->ltecasig, sizeof d->ltecasig);
        getstr(sec, "net_select", d->net_select, sizeof d->net_select);
        getstr(sec, "sa_bands", d->sa_bands, sizeof d->sa_bands);
        getstr(sec, "nsa_bands", d->nsa_bands, sizeof d->nsa_bands);
        getstr(sec, "lte_bands", d->lte_bands, sizeof d->lte_bands);
        d->bars     = (int)json_get_int(sec, "bars", 0);
        d->nr_rsrp  = (int)json_get_int(sec, "nr_rsrp", 0);
        d->nr_rsrq  = (int)json_get_int(sec, "nr_rsrq", 0);
        d->nr_rssi  = (int)json_get_int(sec, "nr_rssi", 0);
        d->lte_rsrp = (int)json_get_int(sec, "lte_rsrp", 0);
        d->lte_rsrq = (int)json_get_int(sec, "lte_rsrq", 0);
        d->lte_rssi = (int)json_get_int(sec, "lte_rssi", 0);
        d->rssi     = (int)json_get_int(sec, "rssi", 0);
        d->mcc      = (int)json_get_int(sec, "mcc", 0);
        d->mnc      = (int)json_get_int(sec, "mnc", 0);
        d->nr_pci   = (int)json_get_int(sec, "nr_pci", 0);
        d->nr_cell_id = json_get_int(sec, "nr_cell_id", 0);
        d->nr_channel = json_get_int(sec, "nr_channel", 0);
    }

    if (json_get(buf, "battery", sec, sizeof sec)) {
        d->bat_percent     = (int)json_get_int(sec, "percent", 0);
        d->bat_temp        = (int)json_get_int(sec, "temp", 0);
        d->charging        = (int)json_get_int(sec, "charging", 0);
        d->charger_connect = (int)json_get_int(sec, "charger_connect", 0);
        d->chg_uv = json_get_int(sec, "chg_uv", 0);
        d->chg_ua = json_get_int(sec, "chg_ua", 0);
        d->bat_uv = json_get_int(sec, "bat_uv", 0);
        d->bat_ua = json_get_int(sec, "bat_ua", 0);
    }

    if (json_get(buf, "clients", sec, sizeof sec)) {
        d->clients_total = (int)json_get_int(sec, "total", 0);
        d->clients_wifi  = (int)json_get_int(sec, "wifi", 0);
        d->clients_lan   = (int)json_get_int(sec, "lan", 0);
        /* list:[{name,ip,mac},...] — walk each {...} object */
        char list[2048];
        d->client_n = 0;
        if (json_get(sec, "list", list, sizeof list)) {
            char *p = list;
            while (p && d->client_n < 16) {
                char *start = find_next_object(p);
                char *end;
                if (!start) break;
                end = obj_end(start);
                if (!end) break;
                char obj[160]; size_t L = (size_t)(end - start) + 1;
                if (L >= sizeof obj) {
                    p = end + 1;
                    continue;
                }
                memcpy(obj, start, L); obj[L] = 0;
                getstr(obj, "name", d->client[d->client_n].name, sizeof d->client[0].name);
                getstr(obj, "ip",   d->client[d->client_n].ip,   sizeof d->client[0].ip);
                getstr(obj, "mac",  d->client[d->client_n].mac,  sizeof d->client[0].mac);
                d->client_n++;
                p = end + 1;
            }
        }
    }

    d->sms_unread = 0;
    d->sms_n = 0;
    {
        static char smsbuf[DEVUI_SMS_BUF_MAX];
        static char list[DEVUI_SMS_BUF_MAX];
        if (json_get(buf, "sms", smsbuf, sizeof smsbuf)) {
            d->sms_unread = (int)json_get_int(smsbuf, "unread", 0);
            if (json_get(smsbuf, "list", list, sizeof list)) {
                for (char *p = list; p && d->sms_n < DEVUI_SMS_MAX; ) {
                    p = find_next_object(p);
                    if (!p) break;
                    char *end = obj_end(p);
                    if (!end) break;
                    static char obj[DEVUI_SMS_OBJECT_MAX];
                    size_t L = (size_t)(end - p) + 1;
                    if (L >= sizeof obj) {
                        p = end + 1;
                        continue;
                    }
                    memcpy(obj, p, L); obj[L] = 0;
                    d->sms[d->sms_n].id = json_get_int(obj, "id", 0);
                    getstr(obj, "num",  d->sms[d->sms_n].num,  sizeof d->sms[0].num);
                    getstr(obj, "date", d->sms[d->sms_n].date, sizeof d->sms[0].date);
                    getstr(obj, "text", d->sms[d->sms_n].text, sizeof d->sms[0].text);
                    d->sms[d->sms_n].unread = (int)json_get_int(obj, "unread", 0);
                    d->sms_n++;
                    p = end + 1;
                }
            }
        }
    }

    if (json_get(buf, "wlan", sec, sizeof sec)) {
        getstr(sec, "ssid", d->wifi_ssid, sizeof d->wifi_ssid);
        getstr(sec, "key",  d->wifi_key,  sizeof d->wifi_key);
        getstr(sec, "enc",  d->wifi_enc,  sizeof d->wifi_enc);
        d->wifi_enabled = (int)json_get_int(sec, "enabled", 1);
    }

    if (json_get(buf, "nfc", sec, sizeof sec))
        d->nfc_switch = (int)json_get_int(sec, "switch", 0);

    if (json_get(buf, "dhcp", sec, sizeof sec)) {
        getstr(sec, "ip",        d->dhcp_ip,        sizeof d->dhcp_ip);
        getstr(sec, "start",     d->dhcp_start,     sizeof d->dhcp_start);
        getstr(sec, "limit",     d->dhcp_limit,     sizeof d->dhcp_limit);
        getstr(sec, "leasetime", d->dhcp_leasetime, sizeof d->dhcp_leasetime);
    }

    if (json_get(buf, "traffic", sec, sizeof sec)) {
        d->rx_speed = json_get_int(sec, "rx_speed", 0);
        d->tx_speed = json_get_int(sec, "tx_speed", 0);
        d->rx_bytes = json_get_int(sec, "rx_bytes", 0);
        d->tx_bytes = json_get_int(sec, "tx_bytes", 0);
    }

    if (json_get(buf, "qos", sec, sizeof sec)) {
        char tmp[32];
        d->qci = (int)json_get_int(sec, "qci", 0);
        d->ambr_dl = json_get(sec, "ambr_dl", tmp, sizeof tmp) ? atof(tmp) : 0.0;
        d->ambr_ul = json_get(sec, "ambr_ul", tmp, sizeof tmp) ? atof(tmp) : 0.0;
        getstr(sec, "usb_mode", d->usb_mode, sizeof d->usb_mode);
    }

    if (json_get(buf, "system", sec, sizeof sec)) {
        d->uptime       = json_get_int(sec, "uptime", 0);
        d->cpu_temp     = json_get_int(sec, "cpu_temp", 0);
        d->cpu_usage    = json_get_int(sec, "cpu_usage", -1);
        d->mem_used_pct = json_get_int(sec, "mem_used_pct", 0);
        d->mem_total    = json_get_int(sec, "mem_total", 0);
        d->mem_avail    = json_get_int(sec, "mem_avail", 0);
        getstr(sec, "model", d->model, sizeof d->model);
        getstr(sec, "fw", d->fw, sizeof d->fw);
        getstr(sec, "sw_version", d->sw_version, sizeof d->sw_version);
        getstr(sec, "imei", d->imei, sizeof d->imei);
    }

    d->valid = 1;
    return 1;
}
