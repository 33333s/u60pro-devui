#!/usr/bin/env python3
"""fb_view.py - Pull device framebuffer via ADB and display as PNG.
   Runs on Windows with Python + ADB on PATH.
   Usage: python fb_view.py [--watch]
"""
import struct, zlib, subprocess, sys, os, time

W, H = 320, 480

def crc32(data):
    return struct.pack('>I', zlib.crc32(data) & 0xffffffff)

def write_png(path, w, h, rgb_bytes):
    def chunk(ctype, cdata):
        c = ctype + cdata
        return struct.pack('>I', len(cdata)) + c + crc32(c)
    raw = b''
    for y in range(h):
        raw += b'\x00'
        raw += rgb_bytes[y*w*3:(y+1)*w*3]
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw)))
        f.write(chunk(b'IEND', b''))

def pull_decode():
    tmp = os.path.join(os.environ.get('TEMP', '/tmp'), 'fb.dump')
    r = subprocess.run(['adb', 'pull', '/tmp/fb.dump', tmp],
                       capture_output=True, timeout=10)
    try:
        with open(tmp, 'rb') as f:
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

def save_scaled(path, rgb, scale=4):
    bw, bh = W * scale, H * scale
    big = bytearray(bw * bh * 3)
    for y in range(bh):
        for x in range(bw):
            off = ((y//scale)*W + (x//scale)) * 3
            big[(y*bw + x)*3:(y*bw + x)*3+3] = rgb[off:off+3]
    write_png(path, bw, bh, bytes(big))

if __name__ == '__main__':
    watch = '--watch' in sys.argv
    if watch:
        frame = 0
        print("Watching (Ctrl-C to stop)...")
        try:
            while True:
                rgb = pull_decode()
                if rgb:
                    path = f"frame_{frame:04d}.png"
                    save_scaled(path, rgb)
                    print(f"  {path}", end='\r')
                    frame += 1
                time.sleep(1)
        except KeyboardInterrupt:
            print(f"\n{frame} frames saved.")
    else:
        rgb = pull_decode()
        if rgb:
            out = 'screenshot.png'
            for a in sys.argv[1:]:
                if a.startswith('--out='):
                    out = a.split('=', 1)[1]
            save_scaled(out, rgb)
            print(f"Saved {out}")
        else:
            print("ERROR: cannot pull framebuffer")
