#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Single on-board WS2812B, driven through SPI2 MOSI. */
esp_err_t ws2812_init(void);
esp_err_t ws2812_set(uint8_t r, uint8_t g, uint8_t b);
