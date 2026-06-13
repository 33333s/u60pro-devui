#!/usr/bin/env python3
"""fb_viewer.py - Pull /tmp/fb.dump from device and display in a window.
   Requires: pip install pillow
   Usage:   python3 fb_viewer.py [--fps N] [--save frame_%04d.png]
   Keeps pulling the framebuffer dump and shows it in a tkinter window.
"""
import struct, subprocess, sys, time, os

try:
    from PIL import Image, ImageTk
    import tkinter as tk
except ImportError:
    print("ERROR: need pillow + tkinter.  pip install pillow")
    sys.exit(1)

W, H = 320, 480
SAVE_PATTERN = None
root = tk.Tk()
root.title("U60Pro Screen")
label = tk.Label(root)
label.pack()

def pull_frame():
    """adb pull + decode RGB565 → PIL Image"""
    try:
        subprocess.run(["adb", "pull", "/tmp/fb.dump", "/tmp/fb.dump"],
                       capture_output=True, timeout=5)
        with open("/tmp/fb.dump", "rb") as f:
            raw = f.read()
        if len(raw) != W * H * 2:
            return None
        pixels = struct.unpack('<' + 'H' * (W * H), raw)
        img = Image.new('RGB', (W, H))
        data = []
        for px in pixels:
            r = ((px >> 11) & 0x1F) << 3
            g = ((px >> 5) & 0x3F) << 2
            b = (px & 0x1F) << 3
            data.extend([r, g, b])
        img.frombytes(bytes(data))
        return img
    except Exception as e:
        return None

def update():
    global update_count
    img = pull_frame()
    if img:
        photo = ImageTk.PhotoImage(img)
        label.config(image=photo)
        label.image = photo
        if SAVE_PATTERN:
            img.save(SAVE_PATTERN % (update_count % 10000))
        update_count += 1
    root.after(250, update)  # 4 fps

if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--save', help='Save frames as PNG (e.g. frame_%04d.png)')
    args = p.parse_args()
    if args.save:
        SAVE_PATTERN = args.save

    update_count = 0
    update()
    root.mainloop()
