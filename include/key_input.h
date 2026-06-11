/*
 * key_input.h - power-key (KEY_POWER) reader via Linux evdev.
 *
 * Auto-probes /dev/input/event* for the device that reports KEY_POWER (the
 * U60Pro power button = pmic_pwrkey on event0) and reports short/long presses.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_KEY_INPUT_H
#define U60PRO_KEY_INPUT_H

#include <stdint.h>

typedef struct {
    int      fd;
    int      pressed;
    uint32_t press_ms;
    int      long_emitted;
} key_input_t;

#define KEY_EV_NONE  0
#define KEY_EV_SHORT 1   /* released before the long-press threshold */
#define KEY_EV_LONG  2   /* held past the long-press threshold (emitted once) */

#define KEY_LONG_MS 1200

int  key_input_init(key_input_t *k);
/* Non-blocking: drain events, return a KEY_EV_* edge (one per call) or NONE. */
int  key_input_poll(key_input_t *k, uint32_t now_ms);
void key_input_close(key_input_t *k);

#endif /* U60PRO_KEY_INPUT_H */
