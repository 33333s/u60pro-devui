#!/bin/sh
# Lightweight memory/IO drift monitor for on-device verification.
#
# Usage:
#   adb push u60pro-workspace/u60pro-devui/scripts/monitor-memory.sh /tmp/
#   adb shell sh /tmp/monitor-memory.sh
# Optional:
#   INTERVAL=2 LOOPS=600 SH_LOG=1 adb shell sh /tmp/monitor-memory.sh

UI_BIN=${UI_BIN:-u60pro-devui}
DATAD_BIN=${DATAD_BIN:-zwrt-datad}
INTERVAL=${INTERVAL:-2}
LOOPS=${LOOPS:-0}
STATE_URL=${STATE_URL:-http://127.0.0.1:9460/state}
STATE_TIMEOUT=${STATE_TIMEOUT:-2}
OUT_FILE=/tmp/u60pro-devui-memory.csv

read_mem() {
    proc=$1
    if [ -z "$proc" ] || [ ! -r "/proc/$proc/status" ]; then
        echo "0 0 0"
        return
    fi
    set -- $(awk '
    /^VmRSS:/ { rss=$2 }
    /^VmData:/ { data=$2 }
    /^VmSwap:/ { swap=$2 }
    END { print (rss+0) " " (data+0) " " (swap+0) }' "/proc/$proc/status" 2>/dev/null)
    if [ "$#" -lt 3 ]; then
        echo "0 0 0"
        return
    fi
    echo "$1 $2 $3"
}

read_state_meta() {
    body=$(curl -fsS --max-time "$STATE_TIMEOUT" "$STATE_URL" 2>/dev/null || true)
    if [ -z "$body" ]; then
        echo "0 0 0"
        return
    fi
    ts=$(printf '%s' "$body" | sed -n 's/.*"ts"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -n 1)
    size=$(printf '%s' "$body" | wc -c | awk '{print $1}')
    sum=$(printf '%s' "$body" | cksum | awk '{print $1}')
    echo "${ts:-0} ${size:-0} ${sum:-0}"
}

UI_PID=$(pidof "$UI_BIN" 2>/dev/null | awk '{print $1}')
if [ -z "$UI_PID" ]; then
    echo "u60pro-devui not running"
    exit 1
fi
DATAD_PID=$(pidof "$DATAD_BIN" 2>/dev/null | awk '{print $1}')

if [ -n "$SH_LOG" ]; then
    echo "TS,UI_PID,UI_RSS_KB,UI_VmData_KB,UI_VmSwap_KB,DATAD_PID,DATAD_RSS_KB,DATAD_VmData_KB,DATAD_VmSwap_KB,STATE_TS,STATE_TS_DELTA,STATE_CHG,STATE_SIZE,STATE_CKSUM" \
        >"$OUT_FILE"
fi

prev_state_ts=""
prev_state_sum=""
prev_ui_rss=0
prev_datad_rss=0
iter=0
first=1
while :; do
    ts=$(date '+%Y-%m-%d %H:%M:%S')

    UI_PID=$(pidof "$UI_BIN" 2>/dev/null | awk '{print $1}')
    if [ -z "$UI_PID" ]; then
        echo "$ts u60pro-devui not running anymore"
        break
    fi
    DATAD_PID=$(pidof "$DATAD_BIN" 2>/dev/null | awk '{print $1}')

    set -- $(read_mem "$UI_PID")
    ui_rss=$1
    ui_data=$2
    ui_swap=$3
    set -- $(read_mem "$DATAD_PID")
    datad_rss=$1
    datad_data=$2
    datad_swap=$3

ui_rss_delta=0
datad_rss_delta=0
if [ "$first" -eq 0 ]; then
    ui_rss_delta=$((ui_rss - prev_ui_rss))
    datad_rss_delta=$((datad_rss - prev_datad_rss))
fi
if [ "$first" -eq 1 ]; then
    first_ui_rss=$ui_rss
    first_datad_rss=$datad_rss
fi
first=0
prev_ui_rss=$ui_rss
prev_datad_rss=$datad_rss

    set -- $(read_state_meta)
    state_ts=$1
    state_size=$2
    state_sum=$3
    if [ -n "$prev_state_ts" ]; then
        state_ts_delta=$((state_ts - prev_state_ts))
        [ "$state_ts_delta" -lt 0 ] && state_ts_delta=0
    else
        state_ts_delta=0
    fi
    if [ -n "$prev_state_sum" ] && [ "$state_sum" != "$prev_state_sum" ]; then
        changed=1
    else
        changed=0
    fi
    prev_state_ts=$state_ts
    prev_state_sum=$state_sum

    echo "$ts ui=$UI_PID rss=${ui_rss}KB(${ui_rss_delta}) data=${ui_data}KB swap=${ui_swap}KB datad=${DATAD_PID:-n/a} datad_rss=${datad_rss}KB(${datad_rss_delta}) datad_data=${datad_data}KB datad_swap=${datad_swap}KB state_ts=${state_ts} delta=${state_ts_delta} size=${state_size}B cksum=${state_sum} changed=$changed"

    if [ -n "$SH_LOG" ]; then
        echo "$ts,$UI_PID,$ui_rss,$ui_data,$ui_swap,$DATAD_PID,$datad_rss,$datad_data,$datad_swap,$state_ts,$state_ts_delta,$changed,$state_size,$state_sum" \
            >>"$OUT_FILE"
    fi

    iter=$((iter + 1))
if [ "$LOOPS" -gt 0 ] && [ "$iter" -ge "$LOOPS" ]; then
    if [ -n "$SUMMARY" ]; then
        ui_gain=$((ui_rss - first_ui_rss))
        datad_gain=$((datad_rss - first_datad_rss))
        if [ "$iter" -gt 1 ]; then
            echo "--- monitor summary ---"
            echo "ui samples=$iter ui_rss_first=${first_ui_rss}KB ui_rss_last=${ui_rss}KB ui_gain=${ui_gain}KB"
            echo "datad samples=$iter datad_rss_first=${first_datad_rss}KB datad_rss_last=${datad_rss}KB datad_gain=${datad_gain}KB"
        fi
        if [ -n "$SH_LOG" ] && [ -f "$OUT_FILE" ]; then
            echo "# summary written to $OUT_FILE"
        fi
    fi
    break
fi
    sleep "$INTERVAL"
done
