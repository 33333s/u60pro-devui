#!/usr/bin/env python3
"""Minimal zwrt-datad stand-in for the DevUI client tests.

Serves /state, /events (one SSE snapshot, then holds the stream), /control and
/ubus/call with reply shapes taken from zwrt-datad, and 404 for the private
/modem/... routes. Every POST is appended to the log file as one JSON line.

usage: mock_datad.py PORT LOGFILE [STATE.json]
"""
import http.server
import json
import sys
import time

PORT = int(sys.argv[1])
LOG = sys.argv[2]

STATE = {
    "ts": 1,
    "datad": {"name": "zwrt-datad", "version": "0.10.79"},
    "sample_interval_ms": 1000,
    "net": {"type": "SA", "bars": 4, "operator": "China Mobile", "mcc": 460, "mnc": 0, "HSR": False},
    "wlan": {"ssid": "Home5G", "enc": "sae-mixed", "enabled": 1,
             "bands": [{"band": "2g", "ssid": "Home", "enc": "psk2", "enabled": 1}]},
    "power": {"direct_supply": {"supported": True, "enabled": True, "mode": "enable"}},
    "neighbor": {
        "enabled": False, "status": "disabled", "cells": [],
        "lte": {"supported": True, "status": "ready", "cells": [
            {"rat": "LTE", "pci": 490, "arfcn": 2850, "band": 7, "rsrp_dbm": -93, "rsrq_db": -18},
            {"rat": "LTE", "pci": 12, "arfcn": 1300, "band": None, "rsrp_dbm": None, "rsrq_db": -11},
        ]},
    },
}
if len(sys.argv) > 3:
    with open(sys.argv[3], encoding="utf-8") as f:
        STATE = json.load(f)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, code, obj, chunked=False):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        if chunked:
            self.send_header("transfer-encoding", "chunked")
            self.end_headers()
            for i in range(0, len(body), 7):
                part = body[i:i + 7]
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        self.close_connection = True

    def do_GET(self):
        if self.path == "/state":
            return self.reply(200, STATE)
        if self.path == "/events":
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.end_headers()
            self.wfile.write(b"retry: 1000\n\nevent: state\ndata: " + json.dumps(STATE).encode() + b"\n\n")
            self.wfile.flush()
            time.sleep(30)
            return
        return self.reply(404, {"error": "not found"})

    def do_POST(self):
        size = int(self.headers.get("content-length", 0))
        body = json.loads(self.rfile.read(size) or b"{}")
        with open(LOG, "a") as f:
            f.write(json.dumps([self.path, body]) + "\n")
        if self.path == "/control":
            action = body.get("action")
            if action == "wifi.status":
                # chunked on purpose: the client must decode it
                return self.reply(200, {"ok": True, "action": action, "result": {
                    "main_2g": {"ssid": "Home", "key": "pw-24g"},
                    "main_5g": {"ssid": "Home5G", "key": "pw \"5g\""}}}, chunked=True)
            if action == "wifi.radio.set":
                return self.reply(400, {"ok": False, "action": action,
                                        "error": {"code": "invalid_parameter", "message": "three APs"}})
            return self.reply(200, {"ok": True, "action": action, "result": {}})
        if self.path == "/ubus/call":
            return self.reply(200, {"ok": True, "service": body.get("service"),
                                    "method": body.get("method"), "result": {}})
        return self.reply(404, {"error": "not found"})


http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
