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

static void getstr(const char *obj, const char *key, char *dst, size_t n)
{
    if (!json_get(obj, key, dst, n)) dst[0] = 0;
}

int data_refresh(devui_data_t *d)
{
    d->valid = 0;

    FILE *fp = fopen(DEVUI_STATE_FILE, "r");
    if (!fp) return 0;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return 0;
    buf[n] = 0;

    char sec[2048];

    if (json_get(buf, "net", sec, sizeof sec)) {
        getstr(sec, "type", d->net_type, sizeof d->net_type);
        getstr(sec, "operator", d->operator_name, sizeof d->operator_name);
        getstr(sec, "band", d->band, sizeof d->band);
        getstr(sec, "nr_snr", d->nr_snr, sizeof d->nr_snr);
        getstr(sec, "wan_status", d->wan_status, sizeof d->wan_status);
        getstr(sec, "lte_snr", d->lte_snr, sizeof d->lte_snr);
        getstr(sec, "nr_bw", d->nr_bw, sizeof d->nr_bw);
        getstr(sec, "nrca", d->nrca, sizeof d->nrca);
        getstr(sec, "lteca", d->lteca, sizeof d->lteca);
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
    }

    if (json_get(buf, "clients", sec, sizeof sec)) {
        d->clients_total = (int)json_get_int(sec, "total", 0);
        d->clients_wifi  = (int)json_get_int(sec, "wifi", 0);
        d->clients_lan   = (int)json_get_int(sec, "lan", 0);
    }

    if (json_get(buf, "wlan", sec, sizeof sec)) {
        getstr(sec, "ssid", d->wifi_ssid, sizeof d->wifi_ssid);
        getstr(sec, "key",  d->wifi_key,  sizeof d->wifi_key);
        getstr(sec, "enc",  d->wifi_enc,  sizeof d->wifi_enc);
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
        d->mem_used_pct = json_get_int(sec, "mem_used_pct", 0);
        getstr(sec, "model", d->model, sizeof d->model);
        getstr(sec, "fw", d->fw, sizeof d->fw);
    }

    d->valid = 1;
    return 1;
}
