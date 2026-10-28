/*
 * datad_client_test.c - host-side checks of the zwrt-datad client in data.c,
 * run against tests/mock_datad.py by tests/run_host_tests.sh.
 *
 * SPDX-License-Identifier: MIT
 */
#include "data.h"
#include "json.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(void)
{
    static char out[8192], result[4096], sec[512], val[64];
    static devui_data_t d;

    /* the public datad has no /modem/... routes */
    CHECK(datad_request("GET", "/modem/signal-metrics", NULL, NULL, 0, 1000) == 404);

    /* chunked /control reply, escaped password */
    CHECK(datad_control("wifi.status", NULL, out, sizeof out, 1500) == 1);
    CHECK(json_get(out, "result", result, sizeof result));
    CHECK(json_get(result, "main_5g", sec, sizeof sec));
    CHECK(json_get(sec, "key", val, sizeof val) && !strcmp(val, "pw \"5g\""));

    /* a refusal is reported as failure, not success */
    CHECK(datad_control("wifi.radio.set", "{\"band\":\"2g\",\"enabled\":false}", out, sizeof out, 1500) == 0);

    /* async calls; the 400 must not trigger the shell fallback */
    unlink("fallback-ran");
    datad_control_async("qos.reload", NULL, "touch fallback-ran");
    datad_control_async("wifi.radio.set", "{\"band\":\"5g\",\"enabled\":true}", "touch fallback-ran");
    datad_ubus_call_async("zte_nwinfo_api", "nwinfo_reset_band_cell_setting", NULL, "touch fallback-ran");

    /* snapshot over HTTP, then the SSE stream bumps the generation */
    CHECK(data_backend_init() == 1);
    {
        uint32_t until = now_ms() + 3000;
        while (data_backend_generation() == 0 && (int32_t)(now_ms() - until) < 0) {
            (void)data_backend_poll(now_ms());
            usleep(50000);
        }
    }
    CHECK(data_backend_generation() >= 1);
    CHECK(data_refresh_live(&d) == 1);
    CHECK(d.dps == 1);
    CHECK(d.neighbor_enabled == 0);
    CHECK(!strcmp(d.neighbor_status, "disabled"));
    CHECK(d.sample_interval_ms == 1000);
    CHECK(!strcmp(d.datad_version, "0.10.79"));
    CHECK(!strcmp(d.wifi_ssid, "Home5G") && d.wifi_key[0] == 0);
    CHECK(!strcmp(d.operator_name, "中国移动"));
    /* empty neighbor.cells falls back to the vendor neighbor.lte scan */
    CHECK(d.neighbor_n == 2);
    CHECK(!strcmp(d.neighbor[0].rat, "LTE") && d.neighbor[0].pci == 490 &&
          d.neighbor[0].arfcn == 2850 && d.neighbor[0].band == 7 && d.neighbor[0].has_band &&
          d.neighbor[0].rsrp == -93 && d.neighbor[0].rsrq == -18);
    /* JSON null stays "unknown" */
    CHECK(d.neighbor[1].pci == 12 && !d.neighbor[1].has_band && !d.neighbor[1].has_rsrp &&
          d.neighbor[1].has_rsrq && d.neighbor[1].rsrq == -11);

    sleep(1);   /* let the detached async jobs finish */
    CHECK(access("fallback-ran", F_OK) != 0);
    data_backend_close();

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("datad client: all checks passed\n");
    return 0;
}
