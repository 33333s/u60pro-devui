/* touchsim.c - inject touch events into the device evdev node.
 * Usage: touchsim <device> <action> [args...]
 *   touchsim auto tap 160 240        — tap at (160,240)
 *   touchsim auto swipe 50 200 270 200 300 — swipe (50,200)→(270,200) 300ms
 *   touchsim /dev/input/event3 tap 160 240
 * "auto" probes /dev/input/event* for a touchscreen.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

/* Minimal evdev UAPI */
struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

#define EV_SYN  0x00
#define EV_KEY  0x01
#define EV_ABS  0x03
#define SYN_REPORT      0x00
#define BTN_TOUCH       0x14a
#define ABS_MT_SLOT     0x2f
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39
#define EVIOCGBIT_(ev, len) _IOC(_IOC_READ, 'E', 0x20 + (ev), len)

static int test_bit(const unsigned long *arr, int bit)
{
    return (arr[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
}

static void send_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
        fprintf(stderr, "write event failed\n");
}

/* tap at (x,y) */
static void do_tap(int fd, int x, int y)
{
    send_event(fd, EV_ABS, ABS_MT_TRACKING_ID, 0);
    send_event(fd, EV_ABS, ABS_MT_POSITION_X, x);
    send_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);
    send_event(fd, EV_KEY, BTN_TOUCH, 1);
    send_event(fd, EV_SYN, SYN_REPORT, 0);
    usleep(50000);
    send_event(fd, EV_KEY, BTN_TOUCH, 0);
    send_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    send_event(fd, EV_SYN, SYN_REPORT, 0);
}

/* swipe from (x1,y1) to (x2,y2) over `duration_ms` */
static void do_swipe(int fd, int x1, int y1, int x2, int y2, int dur_ms)
{
    int steps = dur_ms / 16;
    if (steps < 4) steps = 4;

    send_event(fd, EV_ABS, ABS_MT_TRACKING_ID, 0);
    send_event(fd, EV_ABS, ABS_MT_POSITION_X, x1);
    send_event(fd, EV_ABS, ABS_MT_POSITION_Y, y1);
    send_event(fd, EV_KEY, BTN_TOUCH, 1);
    send_event(fd, EV_SYN, SYN_REPORT, 0);

    for (int i = 1; i <= steps; i++) {
        usleep(dur_ms * 1000 / steps);
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        send_event(fd, EV_ABS, ABS_MT_POSITION_X, x);
        send_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);
        send_event(fd, EV_SYN, SYN_REPORT, 0);
    }

    usleep(30000);
    send_event(fd, EV_KEY, BTN_TOUCH, 0);
    send_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    send_event(fd, EV_SYN, SYN_REPORT, 0);
}

static int probe_touch(char *out_path, size_t n)
{
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[288];
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long abs_bits[4] = {0};
        if (ioctl(fd, EVIOCGBIT_(EV_ABS, sizeof abs_bits), abs_bits) >= 0 &&
            test_bit(abs_bits, ABS_MT_POSITION_X) &&
            test_bit(abs_bits, ABS_MT_POSITION_Y)) {
            snprintf(out_path, n, "%s", path);
            close(fd);
            closedir(d);
            return 0;
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: touchsim <dev|auto> tap <x> <y>\n");
        fprintf(stderr, "       touchsim <dev|auto> swipe <x1> <y1> <x2> <y2> [dur_ms]\n");
        return 1;
    }

    char dev[288];
    if (!strcmp(argv[1], "auto")) {
        if (probe_touch(dev, sizeof dev) != 0) {
            fprintf(stderr, "no touchscreen found\n");
            return 1;
        }
    } else {
        strncpy(dev, argv[1], sizeof dev - 1);
    }

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", dev, strerror(errno));
        return 1;
    }

    if (!strcmp(argv[2], "tap") && argc >= 5) {
        int x = atoi(argv[3]), y = atoi(argv[4]);
        printf("tap %d,%d on %s\n", x, y, dev);
        do_tap(fd, x, y);
    } else if (!strcmp(argv[2], "swipe") && argc >= 7) {
        int x1 = atoi(argv[3]), y1 = atoi(argv[4]);
        int x2 = atoi(argv[5]), y2 = atoi(argv[6]);
        int dur = argc > 7 ? atoi(argv[7]) : 300;
        printf("swipe %d,%d -> %d,%d (%dms) on %s\n", x1, y1, x2, y2, dur, dev);
        do_swipe(fd, x1, y1, x2, y2, dur);
    } else {
        fprintf(stderr, "unknown action\n");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}
