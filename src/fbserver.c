/* fbserver.c - stream /tmp/fb.dump over TCP + touch injection.
 * Reads the UI's framebuffer dump written by html-poc (render() → dump_fb()).
 * Compile: aarch64-linux-gcc -D_GNU_SOURCE -static -O2 fbserver.c -o fbserver
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ── evdev touch injection (same as touchsim.c) ── */
struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};
#define EV_SYN  0x00
#define EV_KEY  0x01
#define EV_ABS  0x03
#define SYN_REPORT       0x00
#define BTN_TOUCH        0x14a
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39

static int g_touch_fd = -1;
static int g_run = 1;

static void send_ev(int type, int code, int val) {
    if (g_touch_fd < 0) return;
    struct input_event ev = {{0,0}, type, code, val};
    write(g_touch_fd, &ev, sizeof(ev));
}
static void do_tap(int x, int y) {
    send_ev(EV_ABS, ABS_MT_TRACKING_ID, 0);
    send_ev(EV_ABS, ABS_MT_POSITION_X, x);
    send_ev(EV_ABS, ABS_MT_POSITION_Y, y);
    send_ev(EV_KEY, BTN_TOUCH, 1);
    send_ev(EV_SYN, SYN_REPORT, 0);
    usleep(40000);
    send_ev(EV_KEY, BTN_TOUCH, 0);
    send_ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
    send_ev(EV_SYN, SYN_REPORT, 0);
}
static void do_swipe(int x1, int y1, int x2, int y2, int dur) {
    int steps = dur / 16; if (steps < 4) steps = 4;
    send_ev(EV_ABS, ABS_MT_TRACKING_ID, 0);
    send_ev(EV_ABS, ABS_MT_POSITION_X, x1);
    send_ev(EV_ABS, ABS_MT_POSITION_Y, y1);
    send_ev(EV_KEY, BTN_TOUCH, 1);
    send_ev(EV_SYN, SYN_REPORT, 0);
    for (int i = 1; i <= steps; i++) {
        usleep(dur * 1000 / steps);
        send_ev(EV_ABS, ABS_MT_POSITION_X, x1 + (x2-x1)*i/steps);
        send_ev(EV_ABS, ABS_MT_POSITION_Y, y1 + (y2-y1)*i/steps);
        send_ev(EV_SYN, SYN_REPORT, 0);
    }
    usleep(30000);
    send_ev(EV_KEY, BTN_TOUCH, 0);
    send_ev(EV_ABS, ABS_MT_TRACKING_ID, -1);
    send_ev(EV_SYN, SYN_REPORT, 0);
}

static int open_touch(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5)) continue;
        char p[288]; snprintf(p, sizeof p, "/dev/input/%s", de->d_name);
        int fd = open(p, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long bits[4] = {0};
        if (ioctl(fd, _IOC(_IOC_READ, 'E', 0x20 + EV_ABS, sizeof bits), bits) >= 0) {
            if ((bits[ABS_MT_POSITION_X / (8*sizeof(long))] >> (ABS_MT_POSITION_X % (8*sizeof(long)))) & 1UL &&
                (bits[ABS_MT_POSITION_Y / (8*sizeof(long))] >> (ABS_MT_POSITION_Y % (8*sizeof(long)))) & 1UL) {
                closedir(d);
                /* make blocking for writes */
                int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
                return fd;
            }
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

/* ── TCP server ── */
#define PORT 9876
#define MAX_CLIENTS 4

static void handle_client(int fd) {
    char buf[128];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    /* Parse "X x y" or "S x1 y1 x2 y2 dur" */
    int x, y, x2, y2, dur;
    if (sscanf(buf, "T %d %d", &x, &y) == 2)
        do_tap(x, y);
    else if (sscanf(buf, "S %d %d %d %d %d", &x, &y, &x2, &y2, &dur) >= 4) {
        if (dur < 50) dur = 300;
        do_swipe(x, y, x2, y2, dur);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    g_touch_fd = open_touch();
    fprintf(stderr, "fbserver: touch=%s\n", g_touch_fd >= 0 ? "yes" : "no");

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PORT),
                                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(srv, MAX_CLIENTS) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "fbserver: :%d (reading /tmp/fb.dump)\n", PORT);

    int clients[MAX_CLIENTS] = {-1,-1,-1,-1};
    const int W = 320, H = 480;
    const size_t frame_sz = (size_t)W * H * 2;
    static uint8_t fbuf[320 * 480 * 2];
    uint32_t frame_hdr;
    size_t last_size = 0;

    while (g_run) {
        struct pollfd fds[MAX_CLIENTS + 1];
        fds[0].fd = srv; fds[0].events = POLLIN;
        int nfds = 1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] >= 0) {
                fds[nfds].fd = clients[i];
                fds[nfds].events = POLLIN;
                nfds++;
            }
        }

        int r = poll(fds, nfds, 30); /* ~30Hz */
        if (r < 0 && errno != EINTR) break;

        if (fds[0].revents & POLLIN) {
            int c = accept(srv, NULL, NULL);
            if (c >= 0) {
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (clients[i] < 0) { clients[i] = c; break; }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i] >= 0 && i+1 < nfds && fds[i+1].revents & POLLIN)
                handle_client(clients[i]);

        /* Read framebuffer dump from file */
        FILE *fp = fopen("/tmp/fb.dump", "rb");
        if (fp) {
            last_size = fread(fbuf, 1, frame_sz, fp);
            fclose(fp);
        }

        if (last_size == frame_sz) {
            frame_hdr = (uint32_t)frame_sz;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i] < 0) continue;
                if (write(clients[i], &frame_hdr, 4) < 0 ||
                    write(clients[i], fbuf, frame_sz) < (ssize_t)frame_sz) {
                    close(clients[i]); clients[i] = -1;
                }
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] >= 0) close(clients[i]);
    close(srv);
    if (g_touch_fd >= 0) close(g_touch_fd);
    return 0;
}
