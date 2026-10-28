#!/bin/sh
# Host-side tests: build the datad client natively and run it against
# tests/mock_datad.py, then check what the mock received.
# usage: sh tests/run_host_tests.sh   (needs cc and python3)
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${DEVUI_TEST_PORT:-29461}
WORK=$(mktemp -d)
MOCK_PID=
cleanup() {
    if [ -n "$MOCK_PID" ]; then
        kill "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

${CC:-cc} -std=c11 -O1 -Wall -Wextra -Wno-unused-parameter -Werror -D_GNU_SOURCE \
    -DDEVUI_BACKEND_PORT="$PORT" -I"$ROOT/include" -pthread \
    "$ROOT/src/data.c" "$ROOT/src/json.c" "$ROOT/tests/datad_client_test.c" \
    -o "$WORK/datad_client_test"

python3 "$ROOT/tests/mock_datad.py" "$PORT" "$WORK/calls.log" &
MOCK_PID=$!
i=0
until python3 -c "import socket,sys; socket.create_connection(('127.0.0.1', int(sys.argv[1])), 1)" "$PORT" 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -lt 50 ] || { echo "mock datad did not start" >&2; exit 1; }
    sleep 0.1
done

(cd "$WORK" && ./datad_client_test)

python3 - "$WORK/calls.log" <<'PY'
import json, sys
calls = [tuple(json.loads(line)) for line in open(sys.argv[1])]
want = [
    ("/control", {"action": "wifi.status", "params": {}}),
    ("/control", {"action": "wifi.radio.set", "params": {"band": "2g", "enabled": False}}),
    ("/control", {"action": "qos.reload", "params": {}}),
    ("/control", {"action": "wifi.radio.set", "params": {"band": "5g", "enabled": True}}),
    ("/ubus/call", {"service": "zte_nwinfo_api", "method": "nwinfo_reset_band_cell_setting", "args": {}}),
]
missing = [w for w in want if w not in calls]
if missing:
    sys.exit("mock datad did not receive: %r\ngot: %r" % (missing, calls))
print("mock datad: received every expected request")
PY
