#!/usr/bin/env python3
"""touchsim.py - Send simulated touch events to device via ADB.
   Usage: python touchsim.py tap 160 240
          python touchsim.py swipe 50 200 270 200 [dur_ms]
          python touchsim.py left          — swipe right→left (next page)
          python touchsim.py right         — swipe left→right (prev page)
"""
import subprocess, sys, os

TOUCHSIM = '/tmp/touchsim'
BIN_SRC = os.path.join(os.path.dirname(__file__), '..', 'touchsim')

def ensure_pushed():
    """Push the touchsim binary if needed."""
    # Check if binary exists locally
    if not os.path.exists(BIN_SRC):
        print(f"ERROR: {BIN_SRC} not found. Build it first.")
        sys.exit(1)
    subprocess.run(['adb', 'push', BIN_SRC, TOUCHSIM],
                   capture_output=True)
    subprocess.run(['adb', 'shell', f'chmod 755 {TOUCHSIM}'],
                   capture_output=True)

def run(cmd):
    ensure_pushed()
    full = f"{TOUCHSIM} auto {cmd}"
    r = subprocess.run(['adb', 'shell', full], capture_output=True, text=True)
    if r.stdout: print(r.stdout)
    if r.stderr: print(r.stderr, file=sys.stderr)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python touchsim.py tap <x> <y>")
        print("       python touchsim.py swipe <x1> <y1> <x2> <y2> [ms]")
        print("       python touchsim.py left|right")
        print("       python touchsim.py menu    — long-press power")
        print("       python touchsim.py power   — short-press power")
        sys.exit(1)

    a = sys.argv[1]
    if a == 'left':       run('swipe 270 240 50 240 300')
    elif a == 'right':    run('swipe 50 240 270 240 300')
    elif a == 'tap':      run(f'tap {sys.argv[2]} {sys.argv[3]}')
    elif a == 'swipe':    run(f'swipe {sys.argv[2]} {sys.argv[3]} {sys.argv[4]} {sys.argv[5]} {sys.argv[6] if len(sys.argv)>6 else "300"}')
    # convenience: UI-specific taps (approximate positions for 320x480)
    elif a == 'page1':    run('tap 30 450')    # bottom-left → page dots area
    elif a == 'page2':    run('tap 160 450')
    elif a == 'page3':    run('tap 290 450')
    # settings page toggles (y positions approximate)
    elif a == 'theme':    run('tap 280 190')    # dark mode toggle
    elif a == 'adb_tgl':  run('tap 280 230')    # ADB toggle
    elif a == 'speed_tgl':run('tap 280 270')    # speed unit toggle
    else:
        print(f"Unknown: {a}")
        sys.exit(1)
