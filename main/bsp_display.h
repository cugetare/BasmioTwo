/* bsp_display.h */
#pragma once
#include <stdint.h>

void bsp_display_init(void);   /* call once after power-on, before ui_app_init() */
void bsp_backlight_set(uint8_t pct);  /* 0 = off, 100 = full */
void ui_set_backlight_target(uint8_t pct);  /* animates backlight to target; call from UI code instead of bsp_backlight_set() */
void ui_set_user_brightness(uint8_t pct);    /* preferred active brightness 0..100 */
uint8_t ui_get_user_brightness(void);        /* current preferred active brightness */
