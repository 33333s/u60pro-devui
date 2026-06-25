/*
 * data.c - read device state from the zwrt-datad HTTP/SSE backend.
 *
 * SPDX-License-Identifier: MIT
 */
#include "data.h"
#include "json.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef DEVUI_BACKEND_HOST
#define DEVUI_BACKEND_HOST "127.0.0.1"
#endif

#ifndef DEVUI_BACKEND_PORT
#define DEVUI_BACKEND_PORT 9460
#endif

#ifndef DEVUI_BACKEND_STATE_PATH
#define DEVUI_BACKEND_STATE_PATH "/state"
#endif

#ifndef DEVUI_BACKEND_EVENTS_PATH
#define DEVUI_BACKEND_EVENTS_PATH "/events"
#endif

#ifndef DEVUI_BACKEND_RETRY_MS
#define DEVUI_BACKEND_RETRY_MS 1000
#endif

#ifndef DEVUI_BACKEND_IO_TIMEOUT_MS
#define DEVUI_BACKEND_IO_TIMEOUT_MS 300
#endif

#ifndef DEVUI_SMS_BUF_MAX
#define DEVUI_SMS_BUF_MAX 1048576
#endif

#ifndef DEVUI_STATE_BUF_MAX
#define DEVUI_STATE_BUF_MAX 1048576
#endif

#ifndef DEVUI_HTTP_BUF_MAX
#define DEVUI_HTTP_BUF_MAX (DEVUI_STATE_BUF_MAX + 8192)
#endif

#ifndef DEVUI_SSE_BUF_MAX
#define DEVUI_SSE_BUF_MAX (DEVUI_STATE_BUF_MAX + 8192)
#endif

/* Keep parser buffers large enough for long UTF-8 SMS payloads. */
#ifndef DEVUI_SMS_OBJECT_MAX
#define DEVUI_SMS_OBJECT_MAX (DEVUI_SMS_TEXT_MAX + 32768)
#endif

struct backend_state {
    int inited;
    int sse_fd;
    uint32_t next_retry_ms;
    char sse_buf[DEVUI_SSE_BUF_MAX];
    size_t sse_len;
    char live_json[DEVUI_STATE_BUF_MAX];
    size_t live_json_len;
    devui_data_t live_data;
    int live_valid;
    unsigned long long live_version;
    unsigned long long committed_live_version;
    devui_data_t current_data;
    int current_valid;
};

static struct backend_state g_backend;

static uint32_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void backend_init_once(void)
{
    if (g_backend.inited) return;
    memset(&g_backend, 0, sizeof g_backend);
    g_backend.sse_fd = -1;
    g_backend.inited = 1;
}

static void getstr(const char *obj, const char *key, char *dst, size_t n)
{
    if (!json_get(obj, key, dst, n)) dst[0] = 0;
}

static const char *mainland_operator_cn(int mcc, int mnc, const char *raw)
{
    if (mcc == 460) {
        if (mnc == 0 || mnc == 2 || mnc == 4 || mnc == 7 || mnc == 8) return "中国移动";
        if (mnc == 1 || mnc == 6 || mnc == 9) return "中国联通";
        if (mnc == 3 || mnc == 5 || mnc == 11) return "中国电信";
        if (mnc == 15) return "中国广电";
    }
    if (!raw || !raw[0]) return NULL;
    if (!strcmp(raw, "China Mobile") || !strcmp(raw, "CMCC")) return "中国移动";
    if (!strcmp(raw, "China Unicom") || !strcmp(raw, "CUCC")) return "中国联通";
    if (!strcmp(raw, "China Telecom") || !strcmp(raw, "CTCC")) return "中国电信";
    if (!strcmp(raw, "China Broadnet") ||
        !strcmp(raw, "China Broadcasting Network")) return "中国广电";
    return NULL;
}

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

static int parse_snapshot(devui_data_t *d, const char *buf)
{
    static char sec[DEVUI_STATE_BUF_MAX];

    memset(d, 0, sizeof *d);
    d->cpu_usage = -1;
    d->valid = 0;
    if (!buf || !buf[0]) return 0;

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
        {
            const char *cn = mainland_operator_cn(d->mcc, d->mnc, d->operator_name);
            if (cn) snprintf(d->operator_name, sizeof d->operator_name, "%s", cn);
        }
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
        {
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
                    {
                        char obj[160];
                        size_t L = (size_t)(end - start) + 1;
                        if (L >= sizeof obj) {
                            p = end + 1;
                            continue;
                        }
                        memcpy(obj, start, L);
                        obj[L] = 0;
                        getstr(obj, "name", d->client[d->client_n].name, sizeof d->client[0].name);
                        getstr(obj, "ip",   d->client[d->client_n].ip,   sizeof d->client[0].ip);
                        getstr(obj, "mac",  d->client[d->client_n].mac,  sizeof d->client[0].mac);
                        d->client_n++;
                    }
                    p = end + 1;
                }
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
                    {
                        char *end = obj_end(p);
                        if (!end) break;
                        static char obj[DEVUI_SMS_OBJECT_MAX];
                        size_t L = (size_t)(end - p) + 1;
                        if (L >= sizeof obj) {
                            p = end + 1;
                            continue;
                        }
                        memcpy(obj, p, L);
                        obj[L] = 0;
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

static int set_nonblock(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (on) flags |= O_NONBLOCK;
    else    flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

static int wait_fd_ready(int fd, int want_write, int timeout_ms)
{
    fd_set rfds, wfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (want_write) FD_SET(fd, &wfds);
    else            FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    for (;;) {
        int rc = select(fd + 1, want_write ? NULL : &rfds, want_write ? &wfds : NULL, NULL, &tv);
        if (rc < 0 && errno == EINTR) continue;
        return rc;
    }
}

static int connect_tcp(const char *addr, int port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    int rc;

    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (set_nonblock(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc < 0) {
        int err = 0;
        socklen_t errlen = sizeof err;
        if (wait_fd_ready(fd, 1, timeout_ms) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0 || err != 0) {
            close(fd);
            if (err) errno = err;
            return -1;
        }
    }
    if (set_nonblock(fd, 0) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *buf, size_t len, int timeout_ms)
{
    size_t off = 0;
    while (off < len) {
        ssize_t wr = write(fd, buf + off, len - off);
        if (wr > 0) {
            off += (size_t)wr;
            continue;
        }
        if (wr < 0 && errno == EINTR) continue;
        if (wr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd_ready(fd, 1, timeout_ms) <= 0) return 0;
            continue;
        }
        return 0;
    }
    return 1;
}

static void close_sse_stream(void)
{
    if (g_backend.sse_fd >= 0) close(g_backend.sse_fd);
    g_backend.sse_fd = -1;
    g_backend.sse_len = 0;
    g_backend.next_retry_ms = mono_ms() + DEVUI_BACKEND_RETRY_MS;
}

static int trim_json_copy(const char *src, size_t len, char *dst, size_t cap, size_t *outlen)
{
    while (len && (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n')) {
        src++;
        len--;
    }
    while (len && (src[len - 1] == ' ' || src[len - 1] == '\t' ||
                   src[len - 1] == '\r' || src[len - 1] == '\n'))
        len--;
    if (len == 0 || len >= cap) return 0;
    memcpy(dst, src, len);
    dst[len] = 0;
    if (outlen) *outlen = len;
    return 1;
}

static int apply_snapshot_json(const char *json, size_t len)
{
    static char clean[DEVUI_STATE_BUF_MAX];
    devui_data_t parsed;
    size_t clean_len = 0;

    if (!trim_json_copy(json, len, clean, sizeof clean, &clean_len)) return 0;
    if (clean_len == g_backend.live_json_len &&
        memcmp(g_backend.live_json, clean, clean_len) == 0)
        return 0;
    if (!parse_snapshot(&parsed, clean)) return 0;
    memcpy(g_backend.live_json, clean, clean_len + 1);
    g_backend.live_json_len = clean_len;
    g_backend.live_data = parsed;
    g_backend.live_valid = 1;
    g_backend.live_version++;
    return 1;
}

static int fetch_state_http(void)
{
    static char resp[DEVUI_HTTP_BUF_MAX];
    char req[256];
    char *body;
    size_t n = 0;
    int fd = connect_tcp(DEVUI_BACKEND_HOST, DEVUI_BACKEND_PORT, DEVUI_BACKEND_IO_TIMEOUT_MS);

    if (fd < 0) return -1;
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Connection: close\r\n"
             "\r\n",
             DEVUI_BACKEND_STATE_PATH, DEVUI_BACKEND_HOST, DEVUI_BACKEND_PORT);
    if (!send_all(fd, req, strlen(req), DEVUI_BACKEND_IO_TIMEOUT_MS)) {
        close(fd);
        return -1;
    }
    for (;;) {
        ssize_t rd;
        if (n + 1 >= sizeof resp) {
            close(fd);
            return -1;
        }
        if (wait_fd_ready(fd, 0, DEVUI_BACKEND_IO_TIMEOUT_MS) <= 0) {
            close(fd);
            return -1;
        }
        rd = read(fd, resp + n, sizeof resp - 1 - n);
        if (rd == 0) break;
        if (rd < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        n += (size_t)rd;
    }
    close(fd);
    resp[n] = 0;
    if (strncmp(resp, "HTTP/1.1 200", 12) != 0 &&
        strncmp(resp, "HTTP/1.0 200", 12) != 0)
        return -1;
    body = strstr(resp, "\r\n\r\n");
    if (body) body += 4;
    else {
        body = strstr(resp, "\n\n");
        if (body) body += 2;
    }
    if (!body) return -1;
    return apply_snapshot_json(body, n - (size_t)(body - resp));
}

static size_t next_sse_event_bytes(const char *buf, size_t len)
{
    size_t line_start = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != '\n') continue;
        {
            size_t line_len = i - line_start;
            if (line_len == 0 || (line_len == 1 && buf[line_start] == '\r'))
                return i + 1;
        }
        line_start = i + 1;
    }
    return 0;
}

static int process_sse_event(const char *buf, size_t len)
{
    static char payload[DEVUI_STATE_BUF_MAX];
    char event_name[32];
    size_t line_start = 0;
    size_t payload_len = 0;

    event_name[0] = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != '\n') continue;
        {
            size_t line_len = i - line_start;
            const char *line = buf + line_start;
            if (line_len && line[line_len - 1] == '\r') line_len--;
            if (line_len >= 6 && !strncmp(line, "event:", 6)) {
                const char *v = line + 6;
                while ((size_t)(v - line) < line_len && *v == ' ') v++;
                {
                    size_t vn = line_len - (size_t)(v - line);
                    if (vn >= sizeof event_name) vn = sizeof event_name - 1;
                    memcpy(event_name, v, vn);
                    event_name[vn] = 0;
                }
            } else if (line_len >= 5 && !strncmp(line, "data:", 5)) {
                const char *v = line + 5;
                size_t vn;
                while ((size_t)(v - line) < line_len && *v == ' ') v++;
                vn = line_len - (size_t)(v - line);
                if (payload_len && payload_len + 1 < sizeof payload)
                    payload[payload_len++] = '\n';
                if (payload_len + vn >= sizeof payload) return 0;
                memcpy(payload + payload_len, v, vn);
                payload_len += vn;
            }
        }
        line_start = i + 1;
    }
    if (!payload_len) return 0;
    if (event_name[0] && strcmp(event_name, "state") != 0) return 0;
    payload[payload_len] = 0;
    return apply_snapshot_json(payload, payload_len);
}

static int process_sse_buffer(void)
{
    size_t consumed = 0;
    int changed = 0;

    for (;;) {
        size_t ev_len = next_sse_event_bytes(g_backend.sse_buf + consumed, g_backend.sse_len - consumed);
        if (!ev_len) break;
        changed |= process_sse_event(g_backend.sse_buf + consumed, ev_len);
        consumed += ev_len;
    }
    if (consumed) {
        memmove(g_backend.sse_buf, g_backend.sse_buf + consumed, g_backend.sse_len - consumed);
        g_backend.sse_len -= consumed;
    }
    return changed;
}

static int open_sse_stream(void)
{
    char req[256];
    char hdr[16384];
    size_t n = 0;
    int fd = connect_tcp(DEVUI_BACKEND_HOST, DEVUI_BACKEND_PORT, DEVUI_BACKEND_IO_TIMEOUT_MS);

    if (fd < 0) return -1;
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Accept: text/event-stream\r\n"
             "Cache-Control: no-cache\r\n"
             "Connection: keep-alive\r\n"
             "\r\n",
             DEVUI_BACKEND_EVENTS_PATH, DEVUI_BACKEND_HOST, DEVUI_BACKEND_PORT);
    if (!send_all(fd, req, strlen(req), DEVUI_BACKEND_IO_TIMEOUT_MS)) {
        close(fd);
        return -1;
    }
    for (;;) {
        char *body;
        ssize_t rd;
        if (n + 1 >= sizeof hdr) {
            close(fd);
            return -1;
        }
        if (wait_fd_ready(fd, 0, DEVUI_BACKEND_IO_TIMEOUT_MS) <= 0) {
            close(fd);
            return -1;
        }
        rd = read(fd, hdr + n, sizeof hdr - 1 - n);
        if (rd <= 0) {
            if (rd < 0 && errno == EINTR) continue;
            close(fd);
            return -1;
        }
        n += (size_t)rd;
        hdr[n] = 0;
        body = strstr(hdr, "\r\n\r\n");
        if (body) body += 4;
        else {
            body = strstr(hdr, "\n\n");
            if (body) body += 2;
        }
        if (body) {
            if (strncmp(hdr, "HTTP/1.1 200", 12) != 0 &&
                strncmp(hdr, "HTTP/1.0 200", 12) != 0) {
                close(fd);
                return -1;
            }
            if (set_nonblock(fd, 1) < 0) {
                close(fd);
                return -1;
            }
            g_backend.sse_fd = fd;
            g_backend.sse_len = n - (size_t)(body - hdr);
            if (g_backend.sse_len >= sizeof g_backend.sse_buf) {
                close_sse_stream();
                return -1;
            }
            memcpy(g_backend.sse_buf, body, g_backend.sse_len);
            g_backend.next_retry_ms = 0;
            return 0;
        }
    }
}

static int drain_sse_stream(void)
{
    int changed = process_sse_buffer();

    while (g_backend.sse_fd >= 0) {
        ssize_t rd;
        if (g_backend.sse_len + 1 >= sizeof g_backend.sse_buf) {
            close_sse_stream();
            break;
        }
        rd = read(g_backend.sse_fd, g_backend.sse_buf + g_backend.sse_len,
                  sizeof g_backend.sse_buf - 1 - g_backend.sse_len);
        if (rd > 0) {
            g_backend.sse_len += (size_t)rd;
            g_backend.sse_buf[g_backend.sse_len] = 0;
            changed |= process_sse_buffer();
            continue;
        }
        if (rd == 0) {
            close_sse_stream();
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close_sse_stream();
        break;
    }
    return changed;
}

int data_backend_init(void)
{
    backend_init_once();
    (void)fetch_state_http();
    if (g_backend.live_valid) (void)data_backend_commit_latest();
    if (g_backend.sse_fd < 0 && open_sse_stream() < 0)
        g_backend.next_retry_ms = mono_ms() + DEVUI_BACKEND_RETRY_MS;
    return g_backend.current_valid;
}

int data_backend_poll(uint32_t now_ms)
{
    int changed = 0;

    backend_init_once();
    if (!g_backend.current_valid && !g_backend.live_valid)
        (void)data_backend_init();
    if (g_backend.sse_fd >= 0) changed |= drain_sse_stream();
    if (g_backend.sse_fd < 0 && now_ms >= g_backend.next_retry_ms) {
        if (open_sse_stream() == 0) {
            changed |= drain_sse_stream();
        } else {
            int http_changed = fetch_state_http();
            if (http_changed > 0) changed = 1;
            g_backend.next_retry_ms = now_ms + DEVUI_BACKEND_RETRY_MS;
        }
    }
    return changed;
}

int data_backend_commit_latest(void)
{
    backend_init_once();
    if (!g_backend.live_valid) return 0;
    if (g_backend.current_valid &&
        g_backend.committed_live_version == g_backend.live_version)
        return 0;
    g_backend.current_data = g_backend.live_data;
    g_backend.current_valid = 1;
    g_backend.committed_live_version = g_backend.live_version;
    return 1;
}

void data_backend_close(void)
{
    backend_init_once();
    if (g_backend.sse_fd >= 0) close(g_backend.sse_fd);
    g_backend.sse_fd = -1;
    g_backend.sse_len = 0;
}

int data_refresh(devui_data_t *d)
{
    if (!d) return 0;
    backend_init_once();
    if (!g_backend.current_valid) (void)data_backend_init();
    if (!g_backend.current_valid) {
        memset(d, 0, sizeof *d);
        d->cpu_usage = -1;
        return 0;
    }
    *d = g_backend.current_data;
    return 1;
}

int data_refresh_live(devui_data_t *d)
{
    if (!d) return 0;
    backend_init_once();
    if (!g_backend.live_valid) (void)data_backend_init();
    if (!g_backend.live_valid) {
        memset(d, 0, sizeof *d);
        d->cpu_usage = -1;
        return 0;
    }
    *d = g_backend.live_data;
    return 1;
}
