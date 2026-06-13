/*
 * data.h - device state consumed from the u60-datad backend.
 *
 * Reads /tmp/u60-datad/state.json (the single ubus aggregator's snapshot).
 * The GUI never calls ubus directly. If the backend isn't running, refresh
 * returns 0 and the UI shows placeholders.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_DATA_H
#define U60PRO_DATA_H

typedef struct {
    int  valid;

    /* network */
    char net_type[16];
    int  bars;
    char operator_name[48];
    char band[16];
    int  nr_rsrp, nr_rsrq, nr_rssi;
    char nr_snr[12];
    int  lte_rsrp, lte_rsrq, lte_rssi;
    char lte_snr[12];
    int  rssi, mcc, mnc, nr_pci;
    long nr_cell_id, nr_channel;
    char nr_bw[12];
    char nrca[256], lteca[256];
    char wan_status[32];
    char net_select[16];
    char sa_bands[256], nsa_bands[256], lte_bands[256];

    /* battery */
    int  bat_percent, bat_temp, charging, charger_connect;

    /* clients */
    int  clients_total, clients_wifi, clients_lan;
    struct { char name[40], ip[24], mac[20]; } client[16];
    int  client_n;

    /* wifi (main SSID) */
    char wifi_ssid[64], wifi_key[64], wifi_enc[24];
    int  wifi_enabled;

    /* nfc tap-to-share */
    int  nfc_switch;

    /* dhcp / lan */
    char dhcp_ip[24], dhcp_start[24], dhcp_limit[8], dhcp_leasetime[12];

    /* traffic (bytes, bytes/s) */
    long rx_speed, tx_speed, rx_bytes, tx_bytes;

    /* qos (parsed from modem key.log by the backend) */
    int    qci;
    double ambr_dl, ambr_ul;   /* Mbps */
    char   usb_mode[16];       /* "user" = adb off, "debug" = adb on */

    /* system */
    long uptime, cpu_temp, cpu_usage, mem_used_pct, mem_total, mem_avail;
    char model[64], fw[80], sw_version[80], imei[24];
} devui_data_t;

/* Re-read the snapshot. Returns 1 on success (d->valid set), 0 otherwise. */
int data_refresh(devui_data_t *d);

#endif /* U60PRO_DATA_H */
