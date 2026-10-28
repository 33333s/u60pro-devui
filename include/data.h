/*
 * data.h - device state consumed from the zwrt-datad backend.
 *
 * Reads the zwrt-datad HTTP/SSE backend snapshot.
 * The GUI never calls ubus directly. If the backend isn't running, refresh
 * returns 0 and the UI shows placeholders.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_DATA_H
#define U60PRO_DATA_H

#include <stddef.h>
#include <stdint.h>

#define DEVUI_SMS_MAX 32
#define DEVUI_SMS_TEXT_MAX 16384
#define DEVUI_NEIGHBOR_MAX 32

/* One neighbor cell; has_* is 0 where datad reported null. */
typedef struct {
    char rat[4];                  /* "LTE" / "NR" */
    int  pci, band, rsrp, rsrq;   /* band number, dBm, dB */
    long arfcn;
    unsigned char has_band, has_arfcn, has_rsrp, has_rsrq;
} devui_neighbor_t;

typedef struct {
    int  valid;

    /* network */
    char net_type[16];
    int  bars;
    char operator_name[48];
    char band[16];
    char nr_band[16];
    int  nr_rsrp, nr_rsrq, nr_rssi;
    char nr_snr[12];
    int  lte_rsrp, lte_rsrq, lte_rssi;
    char lte_snr[12];
    int  rssi, mcc, mnc, nr_pci;
    long nr_cell_id, nr_channel;
    char nr_bw[12];
    char nrca[256], lteca[256], ltecasig[256];
    char wan_status[32];
    char net_select[16];
    int  hsr;
    char sa_bands[256], nsa_bands[256], lte_bands[256];

    /* battery */
    int  bat_percent, bat_temp, charging, charger_connect;
    long chg_uv, chg_ua, bat_uv, bat_ua;   /* charger/battery voltage(µV)/current(µA) */

    /* clients */
    int  clients_total, clients_wifi, clients_lan;
    struct { char name[40], ip[24], mac[20]; } client[16];
    int  client_n;

    /* sms (read-only) */
    int  sms_unread;
    struct { long id; char num[40], date[16], text[DEVUI_SMS_TEXT_MAX]; int unread; } sms[DEVUI_SMS_MAX];
    int  sms_n;

    /* wifi (main SSID) */
    char wifi_ssid[64], wifi_key[64], wifi_enc[24];
    int  wifi_enabled;

    /* nfc tap-to-share */
    int  nfc_switch;

    /* power.direct_supply: -1 when the block is absent or the mode is unknown */
    int  dps;

    /* neighbor: DIAG collector state plus the merged cell list
     * (neighbor.cells while the collector runs, else the vendor neighbor.lte scan).
     * neighbor_enabled is -1 when the block is absent. */
    int  neighbor_enabled;
    char neighbor_status[20];
    devui_neighbor_t neighbor[DEVUI_NEIGHBOR_MAX];
    int  neighbor_n;

    /* datad runtime: global sample/SSE period (0 if absent) and own version */
    int  sample_interval_ms;
    char datad_version[24];

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

/* Start the backend transport and seed the first visible snapshot if available. */
int  data_backend_init(void);

/* Drain SSE events and reconnect as needed. Returns 1 when the live snapshot changed. */
int  data_backend_poll(uint32_t now_ms);

/* Promote the latest live snapshot to the UI-visible snapshot. */
int  data_backend_commit_latest(void);

/* Bumped on every new SSE connection. datad keeps runtime switches such as
 * neighbor.set and state.set_interval only per process, so a change here means
 * they may need to be applied again. */
unsigned data_backend_generation(void);

/* Close the transport socket. Safe to call during shutdown. */
void data_backend_close(void);

/* Copy the current UI-visible snapshot. Returns 1 on success. */
int data_refresh(devui_data_t *d);

/* Copy the latest live snapshot even if UI-visible refresh is paused. */
int data_refresh_live(devui_data_t *d);

/* One request to the local datad listener (127.0.0.1:9460, no token needed
 * there). Copies the response body into out (may be NULL) and returns the HTTP
 * status, or -1 when datad is unreachable or the reply is malformed. Blocking. */
int datad_request(const char *method, const char *path, const char *body,
                  char *out, size_t cap, int timeout_ms);

/* POST /control {"action":action,"params":params_json}. params_json is a JSON
 * object or NULL for {}. Returns 1 when datad answered ok:true. The full reply
 * (including "result") is copied into out when given. Blocking. */
int datad_control(const char *action, const char *params_json,
                  char *out, size_t cap, int timeout_ms);

/* Fire-and-forget datad_control on a detached thread. When the call fails and
 * fallback_cmd is non-NULL it is run with system() instead, so device actions
 * keep working while datad is down. */
void datad_control_async(const char *action, const char *params_json,
                         const char *fallback_cmd);

/* Fire-and-forget POST /ubus/call for vendor calls datad has no action for. */
void datad_ubus_call_async(const char *service, const char *method,
                           const char *args_json, const char *fallback_cmd);

#endif /* U60PRO_DATA_H */
