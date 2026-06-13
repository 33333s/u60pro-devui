#!/usr/bin/env python3
"""Generate 20x20 RGBA PNG icons for u60pro-devui."""

import struct, zlib, os, math

def crc32(data):
    return struct.pack('>I', zlib.crc32(data) & 0xffffffff)

def make_png(w, h, pixels, path):
    def chunk(ctype, data):
        c = ctype + data
        return struct.pack('>I', len(data)) + c + crc32(c)
    raw = b''
    for y in range(h):
        raw += b'\x00'
        raw += bytes(pixels[y*w*4:(y+1)*w*4])
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw)))
        f.write(chunk(b'IEND', b''))

def new_canvas(w, h):
    return [0] * (w * h * 4)

def set_px(px, w, x, y, r, g, b, a=255):
    if 0 <= x < w and 0 <= ((y * w + x) * 4 + 3) < len(px):
        i = (y * w + x) * 4
        px[i]=r; px[i+1]=g; px[i+2]=b; px[i+3]=a

def fill_rect(px, w, x, y, rw, rh, r, g, b, a=255):
    for dy in range(rh):
        for dx in range(rw):
            set_px(px, w, x+dx, y+dy, r, g, b, a)

def hline(px, w, y, x1, x2, r, g, b, a=255):
    for x in range(x1, x2+1):
        set_px(px, w, x, y, r, g, b, a)

OUT = '/tmp/ui-icons'
os.makedirs(OUT, exist_ok=True)
W = 20

G = (76, 197, 106)    # green
O = (255, 160, 64)    # orange
B = (78, 161, 255)    # blue
D = (140, 150, 160)   # dim gray
WHT = (255, 255, 255)

# ── signal ── (4 ascending bars)
px = new_canvas(W, W)
bars = [(1,14,4,5), (6,10,4,9), (11,6,4,13), (16,2,4,17)]  # x,y,w,h
for x, y, w, h in bars:
    fill_rect(px, W, x, y, w, h, *B)
make_png(W, W, px, f'{OUT}/signal.png')

# ── wifi ── (3 arcs + dot)
px = new_canvas(W, W)
cx = 10
# dot
fill_rect(px, W, cx-2, 17, 4, 4, *WHT)
# arcs
for arc, (rx, ry) in enumerate([(5,3), (9,5), (13,7)]):
    for a in range(-55, 56, 6):
        rad = math.radians(a)
        x = int(cx + rx * math.sin(rad))
        y = int(17 - ry * math.cos(rad))
        set_px(px, W, x, y, *WHT)
        set_px(px, W, x+1, y, *WHT)
make_png(W, W, px, f'{OUT}/wifi.png')

# ── download ── (arrow down)
px = new_canvas(W, W)
fill_rect(px, W, 9, 3, 3, 11, *G)         # stem
for i in range(6):                         # arrowhead
    fill_rect(px, W, 3+i, 12+i, 15-i*2, 2, *G)
hline(px, W, 1, 6, 14, *G)                # top bar
make_png(W, W, px, f'{OUT}/download.png')

# ── upload ── (arrow up)
px = new_canvas(W, W)
fill_rect(px, W, 9, 7, 3, 11, *O)
for i in range(6):
    fill_rect(px, W, 3+i, 5-i, 15-i*2, 2, *O)
hline(px, W, 17, 6, 14, *O)
make_png(W, W, px, f'{OUT}/upload.png')

# ── gear ──
px = new_canvas(W, W)
cx, cy, r = 10, 10, 5
for a in range(0, 360, 4):
    x = int(cx + r*math.cos(math.radians(a)))
    y = int(cy + r*math.sin(math.radians(a)))
    set_px(px, W, x, y, *D)
fill_rect(px, W, cx-2, cy-2, 5, 5, *D)
for t in range(4):
    rad = math.radians(t*90 + 45)
    tx = int(cx + 6.5*math.cos(rad))
    ty = int(cy + 6.5*math.sin(rad))
    fill_rect(px, W, tx-1, ty-1, 3, 3, *D)
make_png(W, W, px, f'{OUT}/gear.png')

# ── theme (contrast circle) ──
px = new_canvas(W, W)
for y in range(W):
    for x in range(W):
        dx, dy = x-10, y-10
        if dx*dx + dy*dy <= 64:
            set_px(px, W, x, y, *D)
fill_rect(px, W, 2, 2, 8, 8, *(255,200,80))   # sun corner
make_png(W, W, px, f'{OUT}/theme.png')

# ── usb ──
px = new_canvas(W, W)
fill_rect(px, W, 9, 4, 3, 7, *G)         # main stem
fill_rect(px, W, 9, 1, 3, 3, *G)         # top
fill_rect(px, W, 5, 9, 4, 2, *G)         # left branch
fill_rect(px, W, 12, 9, 4, 2, *G)        # right branch
fill_rect(px, W, 9, 11, 3, 4, *G)        # bottom stem
fill_rect(px, W, 7, 15, 7, 2, *G)        # base
make_png(W, W, px, f'{OUT}/usb.png')

# ── 5g ──
px = new_canvas(W, W)
fill_rect(px, W, 1, 1, 18, 18, *B)       # bg
fill_rect(px, W, 4, 3, 12, 3, 0,0,0)     # 5 top
fill_rect(px, W, 4, 7, 4, 3, 0,0,0)      # 5 mid-left
fill_rect(px, W, 4, 11, 12, 3, 0,0,0)    # 5 bottom
fill_rect(px, W, 12, 7, 4, 3, 0,0,0)     # 5 mid-right
make_png(W, W, px, f'{OUT}/5g.png')

for f in sorted(os.listdir(OUT)):
    sz = os.path.getsize(os.path.join(OUT, f))
    print(f'  {f} ({sz}B)')
print(f'{len(os.listdir(OUT))} icons OK')
