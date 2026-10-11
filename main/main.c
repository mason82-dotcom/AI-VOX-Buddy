#include <inttypes.h>
#include <stdlib.h>
#include "board.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"
#include "es8311.h"
#include "hermes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "ai-vox3-test";
static i2s_chan_handle_t rx_chan, tx_chan;

static void result(const char *test, esp_err_t err)
{
    ESP_LOGI(TAG, "[%-4s] %s: %s", err == ESP_OK ? "PASS" : "FAIL", test, esp_err_to_name(err));
}

static void identity(void)
{
    esp_chip_info_t chip;
    uint32_t flash = 0;
    uint8_t mac[6];
    esp_chip_info(&chip);
    esp_flash_get_size(NULL, &flash);
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "AI-VOX3 self-test / ESP-IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "ESP32-S3 cores=%d rev=%d flash=%" PRIu32 "MB PSRAM=%uMB",
             chip.cores, chip.revision, flash >> 20,
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM) >> 20);
    ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t buttons_init(void)
{
    gpio_config_t cfg = {.pin_bit_mask = BIT64(AIVOX3_BOOT) | BIT64(AIVOX3_BUTTON_A) |
                         BIT64(AIVOX3_BUTTON_B), .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE};
    return gpio_config(&cfg);
}

/* Encode WS2812 bits as 100 (zero) and 110 (one), clocked at 2.5 MHz. */
static esp_err_t rgb_test(void)
{
    spi_bus_config_t bus = {.mosi_io_num = AIVOX3_RGB, .miso_io_num = -1, .sclk_io_num = -1,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 9};
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
    if (err != ESP_OK) return err;
    spi_device_interface_config_t dc = {.clock_speed_hz = 2500000, .mode = 0,
                                        .spics_io_num = -1, .queue_size = 1};
    spi_device_handle_t dev;
    if ((err = spi_bus_add_device(SPI2_HOST, &dc, &dev)) != ESP_OK) return err;
    const uint8_t colors[][3] = {{0, 16, 0}, {16, 0, 0}, {0, 0, 16}, {0, 0, 0}};
    for (unsigned c = 0; c < 4 && err == ESP_OK; ++c) {
        uint8_t encoded[9] = {0};
        unsigned p = 0;
        for (unsigned b = 0; b < 3; ++b) for (int bit = 7; bit >= 0; --bit) {
            unsigned symbol = colors[c][b] & BIT(bit) ? 6 : 4;
            for (int s = 2; s >= 0; --s, ++p)
                encoded[p / 8] |= ((symbol >> s) & 1U) << (7 - p % 8);
        }
        spi_transaction_t t = {.length = 72, .tx_buffer = encoded};
        err = spi_device_transmit(dev, &t);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    spi_bus_remove_device(dev);
    spi_bus_free(SPI2_HOST);
    return err;
}

static esp_err_t audio_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &tx_chan, &rx_chan), TAG, "I2S channel");
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = AIVOX3_I2S_MCLK, .bclk = AIVOX3_I2S_BCLK, .ws = AIVOX3_I2S_WS,
                     .dout = AIVOX3_I2S_DOUT, .din = AIVOX3_I2S_DIN},
    };
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_chan, &sc), TAG, "I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_chan, &sc), TAG, "I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_chan), TAG, "I2S TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx_chan), TAG, "I2S RX enable");
    i2c_config_t ic = {.mode = I2C_MODE_MASTER, .sda_io_num = AIVOX3_I2C_SDA,
        .scl_io_num = AIVOX3_I2C_SCL, .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE, .master.clk_speed = 400000,
        .clk_flags = 0};
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM_0, &ic), TAG, "I2C config");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0), TAG, "I2C driver");
    es8311_handle_t codec = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    if (!codec) return ESP_ERR_NO_MEM;
    es8311_clock_config_t clk = {.mclk_frequency = 16000 * 384, .sample_frequency = 16000,
                                 .mclk_from_mclk_pin = true};
    ESP_RETURN_ON_ERROR(es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "ES8311");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(codec, 55, NULL), TAG, "volume");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(codec, false), TAG, "microphone");
    return es8311_microphone_gain_set(codec, ES8311_MIC_GAIN_24DB);
}

static esp_err_t audio_loopback(void)
{
    int16_t buf[320];
    int64_t sum = 0;
    int peak = 0;
    unsigned count = 0;
    for (int n = 0; n < CONFIG_AIVOX3_AUDIO_LOOPBACK_SECONDS * 50; ++n) {
        size_t got = 0, sent = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_read(rx_chan, buf, sizeof(buf), &got, 500), TAG, "read");
        if (got == 0) return ESP_ERR_INVALID_RESPONSE;
        for (unsigned i = 0; i < got / 2; ++i) {
            int a = abs(buf[i]); sum += a; if (a > peak) peak = a; ++count;
        }
        ESP_RETURN_ON_ERROR(i2s_channel_write(tx_chan, buf, got, &sent, 500), TAG, "write");
        if (sent != got) return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "microphone peak=%d mean_abs=%" PRId64, peak, count ? sum / count : 0);
    return peak == 0 ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
}

static esp_err_t sd_test(void)
{
#if CONFIG_AIVOX3_FORMAT_SD_ONCE
    const bool format_if_needed = true;
#else
    const bool format_if_needed = false;
#endif
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1; slot.clk = AIVOX3_SD_CLK; slot.cmd = AIVOX3_SD_CMD; slot.d0 = AIVOX3_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = format_if_needed,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    if (format_if_needed)
        ESP_LOGW(TAG, "ONE-TIME FORMAT PROFILE: the inserted microSD will be erased if no valid FAT volume exists");
    sdmmc_card_t *card;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &card);
    if (err == ESP_OK) {
        sdmmc_card_print_info(stdout, card);
        esp_vfs_fat_sdcard_unmount("/sdcard", card);
    } else ESP_LOGW(TAG, "microSD optional/not present: %s", esp_err_to_name(err));
    return err;
}

#if CONFIG_AIVOX3_ENABLE_LCD
static bool lcd_transfer_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &woken);
    return woken == pdTRUE;
}

static esp_err_t lcd_test(void)
{
    spi_bus_config_t bus = {.sclk_io_num = AIVOX3_LCD_SCLK, .mosi_io_num = AIVOX3_LCD_MOSI,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 240 * 16 * 2};
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "LCD SPI");
    esp_lcd_panel_io_spi_config_t io_cfg = {.dc_gpio_num = AIVOX3_LCD_DC,
        .cs_gpio_num = AIVOX3_LCD_CS, .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 1};
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST,
                                                 &io_cfg, &io), TAG, "LCD IO");
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16};
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &pc, &panel), TAG, "ST7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "LCD init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), TAG, "LCD invert");
    SemaphoreHandle_t transfer_done = xSemaphoreCreateBinary();
    if (!transfer_done) return ESP_ERR_NO_MEM;
    esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = lcd_transfer_done};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io, &callbacks, transfer_done), TAG, "LCD callback");
    gpio_set_direction(AIVOX3_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(AIVOX3_LCD_BL, 1);
    uint16_t line[240];
    for (int band = 0; band < 3; ++band) {
        uint16_t color = band == 0 ? 0xf800 : band == 1 ? 0x07e0 : 0x001f;
        for (int x = 0; x < 240; ++x) line[x] = color;
        for (int y = band * 80; y < (band + 1) * 80; ++y) {
            ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel, 0, y, 240, y + 1, line), TAG, "LCD draw");
            if (xSemaphoreTake(transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
        }
    }
    vSemaphoreDelete(transfer_done);
    return ESP_OK;
}
#endif

void app_main(void)
{
    identity();
    ESP_LOGI(TAG, "pin guard: PA=NC; invalid vendor-demo GPIO53 is not used");
    result("buttons", buttons_init());
    result("WS2812 RGB", rgb_test());
    esp_err_t err = audio_init();
    result("ES8311 + I2S", err);
    if (err == ESP_OK && CONFIG_AIVOX3_AUDIO_LOOPBACK_SECONDS > 0)
        result("audio loopback", audio_loopback());
    else if (CONFIG_AIVOX3_AUDIO_LOOPBACK_SECONDS == 0)
        ESP_LOGI(TAG, "[SKIP] audio loopback duration is zero");
    result("microSD", sd_test());
#if CONFIG_AIVOX3_ENABLE_LCD
    result("ST7789 LCD", lcd_test());
#else
    ESP_LOGI(TAG, "[SKIP] optional ST7789 LCD disabled");
#endif
    ESP_LOGI(TAG, "self-test complete");
#if CONFIG_AIVOX3_HERMES_ENABLE
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "starting Hermes voice satellite");
        hermes_run(rx_chan, tx_chan);
    } else {
        ESP_LOGE(TAG, "Hermes disabled: ES8311/I2S initialization failed");
    }
#endif
    ESP_LOGI(TAG, "reporting buttons every second");
    while (true) {
        ESP_LOGI(TAG, "BOOT=%d A=%d B=%d (0=pressed)", gpio_get_level(AIVOX3_BOOT),
                 gpio_get_level(AIVOX3_BUTTON_A), gpio_get_level(AIVOX3_BUTTON_B));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
