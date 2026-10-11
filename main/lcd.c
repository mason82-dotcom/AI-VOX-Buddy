#include "lcd.h"

#include <string.h>
#include "board.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_SCALE 2
#define ROW_PIXELS (LCD_WIDTH * 16 * MAX_SCALE)

extern const uint8_t font8x16[95][16];

static const char *TAG = "lcd";
static esp_lcd_panel_handle_t panel;
static SemaphoreHandle_t transfer_done;
static uint16_t *row;

static bool on_transfer_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(transfer_done, &woken);
    return woken == pdTRUE;
}

static uint16_t swap(uint16_t c) { return (uint16_t)(c << 8 | c >> 8); }

esp_err_t lcd_init(void)
{
    if (panel) return ESP_OK;
    transfer_done = xSemaphoreCreateBinary();
    row = heap_caps_malloc(ROW_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!transfer_done || !row) return ESP_ERR_NO_MEM;
    spi_bus_config_t bus = {.sclk_io_num = AIVOX3_LCD_SCLK, .mosi_io_num = AIVOX3_LCD_MOSI,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = ROW_PIXELS * sizeof(uint16_t)};
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI");
    esp_lcd_panel_io_spi_config_t io_cfg = {.dc_gpio_num = AIVOX3_LCD_DC,
        .cs_gpio_num = AIVOX3_LCD_CS, .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 1,
        .on_color_trans_done = on_transfer_done};
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST,
                                                 &io_cfg, &io), TAG, "IO");
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    esp_lcd_panel_handle_t p;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &pc, &p), TAG, "ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(p), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(p), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(p, true), TAG, "invert");
    /* esp_lcd's ST7789 init only leaves sleep mode; DISPON must be sent explicitly. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(p, true), TAG, "on");
    gpio_set_direction(AIVOX3_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(AIVOX3_LCD_BL, 1);
    panel = p;
    return ESP_OK;
}

bool lcd_ready(void) { return panel != NULL; }

static esp_err_t draw(int x, int y, int w, int h)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, x, y, x + w, y + h, row), TAG, "draw");
    return xSemaphoreTake(transfer_done, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    if (!panel) return ESP_ERR_INVALID_STATE;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > LCD_WIDTH || y + h > LCD_HEIGHT)
        return ESP_ERR_INVALID_ARG;
    const int lines = ROW_PIXELS / w;
    for (int i = 0; i < w * (h < lines ? h : lines); ++i) row[i] = swap(color);
    for (int done = 0; done < h;) {
        int n = h - done < lines ? h - done : lines;
        ESP_RETURN_ON_ERROR(draw(x, y + done, w, n), TAG, "fill");
        done += n;
    }
    return ESP_OK;
}

esp_err_t lcd_text_row(int y, const char *text, int scale, uint16_t fg, uint16_t bg, bool center)
{
    if (!panel) return ESP_ERR_INVALID_STATE;
    if (scale < 1 || scale > MAX_SCALE || y < 0 || y + 16 * scale > LCD_HEIGHT) return ESP_ERR_INVALID_ARG;
    const int h = 16 * scale, cw = 8 * scale;
    const uint16_t f = swap(fg), b = swap(bg);
    for (int i = 0; i < LCD_WIDTH * h; ++i) row[i] = b;
    int len = (int)strlen(text), max = (LCD_WIDTH - 16) / cw;
    if (len > max) len = max;
    int x0 = center ? (LCD_WIDTH - len * cw) / 2 : 8;
    for (int c = 0; c < len; ++c) {
        unsigned char ch = (unsigned char)text[c];
        const uint8_t *g = font8x16[(ch >= 0x20 && ch < 0x7f ? ch : '?') - 0x20];
        for (int gy = 0; gy < 16; ++gy) for (int gx = 0; gx < 8; ++gx) {
            if (!(g[gy] & (0x80 >> gx))) continue;
            for (int sy = 0; sy < scale; ++sy) for (int sx = 0; sx < scale; ++sx)
                row[(gy * scale + sy) * LCD_WIDTH + x0 + c * cw + gx * scale + sx] = f;
        }
    }
    return draw(0, y, LCD_WIDTH, h);
}
