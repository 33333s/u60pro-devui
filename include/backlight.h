/*
 * backlight.h - LCD backlight control via the LED sysfs interface.
 *
 * On the U60Pro the panel backlight is /sys/class/leds/led:lcd/brightness
 * (0..max_brightness). Short-press the power key toggles it (screen on/off).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef U60PRO_BACKLIGHT_H
#define U60PRO_BACKLIGHT_H

void backlight_init(void);
void backlight_on(void);      /* restore the remembered on-level */
void backlight_off(void);
void backlight_toggle(void);
int  backlight_is_on(void);

void backlight_set(int level); /* set brightness directly (remembers on-level) */
int  backlight_get(void);      /* current brightness */
int  backlight_max(void);      /* max_brightness */

#endif /* U60PRO_BACKLIGHT_H */
