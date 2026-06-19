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
void backlight_on(void);      /* restore the remembered on-level (instant) */
void backlight_off(void);
void backlight_toggle(void);
int  backlight_is_on(void);

/* Animated screen off/on (blocking): fade the live brightness without changing
 * the remembered user level. Off ramps level->1 then 0 (about 0.125s at full,
 * scaled by brightness); on ramps 1->level in about half that time. */
void backlight_fade_off(void);
void backlight_fade_on(void);
void backlight_predim(void);  /* force live brightness to 1 (before a wake render) */

void backlight_set(int level); /* set brightness directly (remembers on-level) */
int  backlight_get(void);      /* current brightness */
int  backlight_max(void);      /* max_brightness */

#endif /* U60PRO_BACKLIGHT_H */
