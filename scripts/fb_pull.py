#!/usr/bin/env python3
"""fb_pull.py - Pull device framebuffer and save as PNG.
   No PIL needed — uses pure Python PNG writer via zlib.
   Usage: python3 fb_pull.py [--watch] [--file output.png]
   --watch: keep pulling every second, write frame_N.png
"""
import struct, zlib, subprocess, sys, os, time

W, H = 320, 480

def crc32(data):
    return struct.pack('>I', zlib.crc32(data) & 0xffffffff)

def write_png(path, w, h, rgb_bytes):
    """Write a minimal RGBA/PNG file from raw RGB bytes (no PIL)."""
    def chunk(ctype, cdata):
        c = ctype + cdata
        return struct.pack('>I', len(cdata)) + c + crc32(c)

    raw = b''
    for y in range(h):
        raw += b'\x00'  # filter none
        raw += rgb_bytes[y*w*3:(y+1)*w*3]

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw)))
        f.write(chunk(b'IEND', b''))

def pull_decode():
    """adb pull + RGB565 decode → raw RGB bytes"""
    subprocess.run(["adb", "pull", "/tmp/fb.dump", "/tmp/fb.dump"],
                   capture_output=True, timeout=10)
    try:
        with open("/tmp/fb.dump", "rb") as f:
            data = f.read()
    except:
        return None
    if len(data) != W * H * 2:
        return None
    pixels = struct.unpack('<' + 'H' * (W * H), data)
    rgb = bytearray(W * H * 3)
    for i, px in enumerate(pixels):
        rgb[i*3]   = ((px >> 11) & 0x1F) << 3
        rgb[i*3+1] = ((px >> 5)  & 0x3F) << 2
        rgb[i*3+2] = (px & 0x1F) << 3
    return bytes(rgb)

def save_frame(path, rgb):
    """Save raw RGB as PNG, 4x scaled"""
    # Build 4x scaled version
    big_w, big_h = W * 4, H * 4
    big = bytearray(big_w * big_h * 3)
    for y in range(big_h):
        sy = y // 4
        for x in range(big_w):
            sx = x // 4
            off = (sy * W + sx) * 3
            big[(y * big_w + x) * 3]     = rgb[off]
            big[(y * big_w + x) * 3 + 1] = rgb[off + 1]
            big[(y * big_w + x) * 3 + 2] = rgb[off + 2]
    write_png(path, big_w, big_h, bytes(big))

if __name__ == '__main__':
    watch = '--watch' in sys.argv
    out = 'screen.png'
    for a in sys.argv[1:]:
        if a.startswith('--file='):
            out = a.split('=', 1)[1]

    if watch:
        frame = 0
        print(f"Watching... (Ctrl-C to stop)")
        try:
            while True:
                rgb = pull_decode()
                if rgb:
                    path = f"frame_{frame:04d}.png"
                    save_frame(path, rgb)
                    print(f"  {path}")
                    frame += 1
                time.sleep(1)
        except KeyboardInterrupt:
            print(f"Done. {frame} frames.")
    else:
        rgb = pull_decode()
        if rgb:
            save_frame(out, rgb)
            print(f"Saved {out} ({W*4}x{H*4})")
        else:
            print("ERROR: could not pull framebuffer")
            sys.exit(1)
