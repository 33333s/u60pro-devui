#!/usr/bin/env python3
"""
Push a DevUI test frame to a U60Pro over SSH.

This is a host-side helper intended for macOS first. It does not use adb.
Instead, it opens an SSH session, then forwards one DevUI-IPC command to the
device-local Unix socket `/tmp/zwrt-devui.sock`.

Examples:
  python3 scripts/devui_ssh_frame.py ping user@device-host
  python3 scripts/devui_ssh_frame.py frame user@device-host
  python3 scripts/devui_ssh_frame.py frame user@device-host --ttl 0
  python3 scripts/devui_ssh_frame.py frame user@device-host --raw ./frame.rgb565
  python3 scripts/devui_ssh_frame.py close user@device-host

Notes:
  - SSH must be usable non-interactively (for example via key auth or an
    existing agent), because stdin is used for the frame payload.
  - The default frame size is the DevUI content area: 320x454.
"""

from __future__ import annotations

import argparse
import pathlib
import shlex
import subprocess
import sys
from typing import Iterable, List


DEFAULT_SOCK = "/tmp/zwrt-devui.sock"
DEFAULT_W = 320
DEFAULT_H = 454


def rgb565le(r: int, g: int, b: int) -> bytes:
    px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes((px & 0xFF, (px >> 8) & 0xFF))


def build_test_pattern(width: int, height: int, pattern: str) -> bytes:
    data = bytearray(width * height * 2)
    mid_x = width // 2
    mid_y = height // 2

    for y in range(height):
        for x in range(width):
            if x < 4 or y < 4 or x >= width - 4 or y >= height - 4:
                r, g, b = 255, 255, 255
            elif abs(x - mid_x) <= 1 or abs(y - mid_y) <= 1:
                r, g, b = 255, 220, 40
            elif pattern == "bars":
                band = (x * 6) // max(1, width)
                colors = (
                    (255, 64, 64),
                    (255, 160, 64),
                    (255, 255, 64),
                    (64, 220, 96),
                    (80, 160, 255),
                    (196, 96, 255),
                )
                r, g, b = colors[min(band, len(colors) - 1)]
            elif pattern == "checker":
                tile = 24
                on = ((x // tile) + (y // tile)) % 2 == 0
                if on:
                    r = 50 + (x * 205) // max(1, width - 1)
                    g = 90 + (y * 120) // max(1, height - 1)
                    b = 180
                else:
                    r, g, b = 18, 28, 44
            else:
                r = (x * 255) // max(1, width - 1)
                g = (y * 255) // max(1, height - 1)
                b = ((x + y) * 255) // max(1, width + height - 2)

            i = (y * width + x) * 2
            data[i : i + 2] = rgb565le(r, g, b)

    return bytes(data)


def load_raw_frame(path: pathlib.Path, width: int, height: int) -> bytes:
    payload = path.read_bytes()
    expected = width * height * 2
    if len(payload) != expected:
        raise ValueError(
            f"raw frame size mismatch: got {len(payload)} bytes, expected {expected} "
            f"for {width}x{height} rgb565"
        )
    return payload


def build_frame_command(width: int, height: int, ttl_ms: int, payload: bytes) -> bytes:
    head = f"FRAME rgb565 {width} {height} {ttl_ms}\n".encode("ascii")
    return head + payload


def build_remote_sender(sock_path: str) -> str:
    py_bridge = (
        "import socket,sys;"
        "s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);"
        "s.connect(sys.argv[1]);"
        "data=sys.stdin.buffer.read();"
        "s.sendall(data);"
        "sys.stdout.buffer.write(s.recv(256));"
        "s.close()"
    )

    script = f"""
SOCK={shlex.quote(sock_path)}
[ -S "$SOCK" ] || {{ echo "ERR no-socket"; exit 11; }}

if command -v nc >/dev/null 2>&1 && nc -h 2>&1 | grep -q -- '-U'; then
    exec nc -U "$SOCK"
fi

if command -v busybox >/dev/null 2>&1 && busybox nc -h 2>&1 | grep -q -- '-U'; then
    exec busybox nc -U "$SOCK"
fi

if command -v python3 >/dev/null 2>&1; then
    exec python3 -c {shlex.quote(py_bridge)} "$SOCK"
fi

if command -v python >/dev/null 2>&1; then
    exec python -c {shlex.quote(py_bridge)} "$SOCK"
fi

echo "ERR no-sender"
exit 12
""".strip()

    return f"sh -lc {shlex.quote(script)}"


def build_ssh_command(args: argparse.Namespace, remote_cmd: str) -> List[str]:
    cmd = ["ssh", "-T"]
    if args.port is not None:
        cmd.extend(["-p", str(args.port)])
    if args.identity is not None:
        cmd.extend(["-i", args.identity])
    for opt in args.ssh_opt:
        cmd.extend(shlex.split(opt))
    cmd.extend([args.target, remote_cmd])
    return cmd


def run_ssh(args: argparse.Namespace, payload: bytes) -> str:
    remote_cmd = build_remote_sender(args.sock)
    cmd = build_ssh_command(args, remote_cmd)
    proc = subprocess.run(cmd, input=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    stdout = proc.stdout.decode("utf-8", "replace").strip()
    stderr = proc.stderr.decode("utf-8", "replace").strip()

    if proc.returncode != 0:
        msg = stderr or stdout or f"ssh exited with code {proc.returncode}"
        raise RuntimeError(msg)
    if not stdout:
        raise RuntimeError("empty response from remote sender")
    return stdout


def positive_int(value: str) -> int:
    iv = int(value, 10)
    if iv <= 0:
        raise argparse.ArgumentTypeError("value must be > 0")
    return iv


def non_negative_int(value: str) -> int:
    iv = int(value, 10)
    if iv < 0:
        raise argparse.ArgumentTypeError("value must be >= 0")
    return iv


def add_ssh_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("target", help="ssh target, for example user@device-host")
    parser.add_argument("--port", type=int, help="ssh port")
    parser.add_argument("--identity", help="ssh private key path")
    parser.add_argument(
        "--ssh-opt",
        action="append",
        default=[],
        help='extra ssh option chunk, e.g. --ssh-opt="-o StrictHostKeyChecking=no"',
    )
    parser.add_argument("--sock", default=DEFAULT_SOCK, help=f"remote DevUI socket path (default: {DEFAULT_SOCK})")


def cmd_ping(args: argparse.Namespace) -> int:
    resp = run_ssh(args, b"PING\n")
    print(resp)
    return 0 if resp.startswith("OK ") else 1


def cmd_close(args: argparse.Namespace) -> int:
    resp = run_ssh(args, b"CLOSE\n")
    print(resp)
    return 0 if resp.startswith("OK ") else 1


def cmd_frame(args: argparse.Namespace) -> int:
    if args.raw is not None:
        payload = load_raw_frame(pathlib.Path(args.raw), args.width, args.height)
    else:
        payload = build_test_pattern(args.width, args.height, args.pattern)

    frame = build_frame_command(args.width, args.height, args.ttl, payload)
    resp = run_ssh(args, frame)
    print(resp)
    return 0 if resp.startswith("OK ") else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Send a DevUI-IPC command to a device over SSH.",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    ping = sub.add_parser("ping", help="check whether the DevUI socket is reachable")
    add_ssh_options(ping)
    ping.set_defaults(func=cmd_ping)

    close = sub.add_parser("close", help="close the current external DevUI view")
    add_ssh_options(close)
    close.set_defaults(func=cmd_close)

    frame = sub.add_parser("frame", help="send one rgb565 frame")
    add_ssh_options(frame)
    frame.add_argument("--width", type=positive_int, default=DEFAULT_W, help=f"source frame width (default: {DEFAULT_W})")
    frame.add_argument("--height", type=positive_int, default=DEFAULT_H, help=f"source frame height (default: {DEFAULT_H})")
    frame.add_argument(
        "--ttl",
        type=non_negative_int,
        default=5000,
        help="display duration in milliseconds, 0 means stay until CLOSE (default: 5000)",
    )
    frame.add_argument("--raw", help="path to a local little-endian rgb565 raw frame file")
    frame.add_argument(
        "--pattern",
        choices=("gradient", "checker", "bars"),
        default="gradient",
        help="generated pattern when --raw is omitted (default: gradient)",
    )
    frame.set_defaults(func=cmd_frame)

    return parser


def main(argv: Iterable[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(list(argv))
    try:
        return args.func(args)
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
