#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_DURA_LCD_WIDTH
#define CONFIG_DURA_LCD_WIDTH 128
#endif
#ifndef CONFIG_DURA_LCD_HEIGHT
#define CONFIG_DURA_LCD_HEIGHT 64
#endif

#define DURA_LCD_WIDTH   CONFIG_DURA_LCD_WIDTH
#define DURA_LCD_HEIGHT  CONFIG_DURA_LCD_HEIGHT
#define DURA_LCD_PAGES   (DURA_LCD_HEIGHT / 8)
#define DURA_LCD_FB_SIZE (DURA_LCD_WIDTH * DURA_LCD_PAGES)

esp_err_t dura_lcd_init(void);
esp_err_t dura_lcd_set_backlight(bool on);
void dura_lcd_clear(void);
void dura_lcd_set_pixel(int x, int y, bool on);
void dura_lcd_draw_hline(int x, int y, int w, bool on);
void dura_lcd_draw_vline(int x, int y, int h, bool on);
void dura_lcd_draw_rect(int x, int y, int w, int h, bool on);
void dura_lcd_fill_rect(int x, int y, int w, int h, bool on);
void dura_lcd_draw_text_5x7(int x, int y, const char *text);
void dura_lcd_draw_bitmap_1bpp(int x, int y, const uint8_t *data, int w, int h, bool on);
void dura_lcd_load_frame_1bpp_128x64(const uint8_t data[DURA_LCD_FB_SIZE]);
esp_err_t dura_lcd_draw_splash(void);
esp_err_t dura_lcd_flush(void);

#ifdef __cplusplus
}
#endif
