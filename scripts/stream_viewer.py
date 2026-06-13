#!/usr/bin/env python3
"""stream_viewer.py - Real-time screen mirroring + click-to-touch for U60Pro.
   Requires: tkinter (built-in)
   Usage:
     1. Build & push fbserver to device, start it  (listens on :9876)
     2. adb forward tcp:9876 tcp:9876
     3. python stream_viewer.py

   Click on the window → tap on device at that position.
   Drag → swipe.
"""
import socket, struct, threading, sys, os, subprocess, time

W, H = 320, 480
SCALE = 3  # display scale factor

# Try to import tkinter
try:
    import tkinter as tk
except ImportError:
    print("ERROR: tkinter not available")
    sys.exit(1)

class StreamViewer:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title(f"U60Pro Stream ({W}x{H} @ {SCALE}x)")
        self.canvas = tk.Canvas(self.root, width=W*SCALE, height=H*SCALE,
                                bg='black', cursor='crosshair')
        self.canvas.pack()

        self.frame_data = None
        self.frame_lock = threading.Lock()
        self.running = True
        self.drag_start = None

        # Mouse events → touch
        self.canvas.bind('<ButtonPress-1>', self.on_press)
        self.canvas.bind('<B1-Motion>', self.on_drag)
        self.canvas.bind('<ButtonRelease-1>', self.on_release)

        # Keyboard shortcuts
        self.root.bind('<Left>', lambda e: self.send_touch('S 270 240 50 240 300'))
        self.root.bind('<Right>', lambda e: self.send_touch('S 50 240 270 240 300'))
        self.root.bind('<Escape>', lambda e: self.root.destroy())

        # Connect to device
        self.sock = None
        self.connect()

        # Start receiver thread
        self.recv_thread = threading.Thread(target=self.recv_loop, daemon=True)
        self.recv_thread.start()

        # Start display updater
        self.update_display()

    def connect(self):
        """Connect to fbserver via adb-forwarded port."""
        # Ensure adb forward
        subprocess.run(['adb', 'forward', 'tcp:9876', 'tcp:9876'], capture_output=True)
        time.sleep(0.5)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect(('127.0.0.1', 9876))
        self.sock.settimeout(3)
        print("Connected to device")

    def recv_loop(self):
        """Continuously receive frames from device."""
        buf = b''
        while self.running:
            try:
                # Read 4-byte size header
                while len(buf) < 4:
                    chunk = self.sock.recv(4 - len(buf))
                    if not chunk:
                        print("Connection lost, reconnecting...")
                        self.reconnect()
                        buf = b''
                        continue
                    buf += chunk
                size = struct.unpack('<I', buf[:4])[0]
                buf = buf[4:]

                # Read frame data
                while len(buf) < size:
                    chunk = self.sock.recv(size - len(buf))
                    if not chunk:
                        print("Connection lost")
                        self.reconnect()
                        buf = b''
                        break
                    buf += chunk

                if len(buf) >= size:
                    with self.frame_lock:
                        self.frame_data = buf[:size]
                    buf = buf[size:]
            except (socket.timeout, ConnectionResetError, BrokenPipeError):
                self.reconnect()
                buf = b''

    def reconnect(self):
        try:
            self.sock.close()
        except:
            pass
        time.sleep(1)
        self.connect()

    def send_touch(self, cmd):
        """Send touch command to device."""
        try:
            self.sock.sendall((cmd + '\n').encode())
        except:
            pass

    def on_press(self, event):
        self.drag_start = (event.x, event.y, time.time())

    def on_drag(self, event):
        pass  # handled on release

    def on_release(self, event):
        if not self.drag_start:
            return
        sx, sy, st = self.drag_start
        ex, ey = event.x, event.y
        dx = abs(ex - sx)
        dy = abs(ey - sy)
        dt = time.time() - st

        # Convert window coords → device coords
        dx1, dy1 = int(sx / SCALE), int(sy / SCALE)
        dx2, dy2 = int(ex / SCALE), int(ey / SCALE)

        if dx < 5 and dy < 5:  # tap
            self.send_touch(f'T {dx1} {dy1}')
        else:  # swipe
            dur = max(100, int(dt * 1000))
            self.send_touch(f'S {dx1} {dy1} {dx2} {dy2} {dur}')
        self.drag_start = None

    def update_display(self):
        """Decode latest frame and blit to canvas."""
        with self.frame_lock:
            data = self.frame_data
        if data and len(data) == W * H * 2:
            # Decode RGB565 → tkinter PhotoImage
            pixels = struct.unpack('<' + 'H' * (W * H), data)
            # Build PPM-like string for tkinter (slow but works)
            img_data = []
            for y in range(H):
                row = []
                for x in range(W):
                    px = pixels[y * W + x]
                    r = ((px >> 11) & 0x1F) << 3
                    g = ((px >> 5)  & 0x3F) << 2
                    b = (px & 0x1F) << 3
                    row.append(f'#{r:02x}{g:02x}{b:02x}')
                img_data.append('{' + ' '.join(row) + '}')
            # Clear and redraw using rectangles (simpler approach)
            self.canvas.delete('all')
            # Draw a scaled image using pixel rectangles (not efficient but works)
            # For performance, group same-color runs
            for y in range(H):
                run_start = 0
                run_color = None
                for x in range(W + 1):
                    if x < W:
                        px = pixels[y * W + x]
                        r = ((px >> 11) & 0x1F) << 3
                        g = ((px >> 5)  & 0x3F) << 2
                        b = (px & 0x1F) << 3
                        color = f'#{r:02x}{g:02x}{b:02x}'
                    else:
                        color = None
                    if color != run_color:
                        if run_start < x and run_color:
                            self.canvas.create_rectangle(
                                run_start * SCALE, y * SCALE,
                                x * SCALE, (y + 1) * SCALE,
                                fill=run_color, outline='')
                        run_start = x
                        run_color = color
        if self.running:
            self.root.after(33, self.update_display)  # ~30 fps

    def run(self):
        self.root.protocol('WM_DELETE_WINDOW', self.on_close)
        self.root.mainloop()

    def on_close(self):
        self.running = False
        self.root.destroy()
        try: self.sock.close()
        except: pass

if __name__ == '__main__':
    app = StreamViewer()
    app.run()
