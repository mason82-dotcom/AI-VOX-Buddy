#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define LCD_WIDTH 240
#define LCD_HEIGHT 240

/* RGB565 in host order; lcd.c swaps bytes for the ST7789's big-endian SPI. */
#define LCD_RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

esp_err_t lcd_init(void);
bool lcd_ready(void);
esp_err_t lcd_fill(int x, int y, int w, int h, uint16_t color);
/*
 * Draws one full-width text row of height 16 * scale starting at y. Text is
 * clipped to the panel; `center` centers it, otherwise it starts at x = 8.
 */
esp_err_t lcd_text_row(int y, const char *text, int scale, uint16_t fg, uint16_t bg, bool center);
