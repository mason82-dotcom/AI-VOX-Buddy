#include <inttypes.h>
#include <stdlib.h>
#include "board.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"
#include "es8311.h"
#include "hermes.h"
#include "lcd.h"
#include "status.h"
#include "ws2812.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "ai-vox3-test";
static i2s_chan_handle_t rx_chan, tx_chan;
static es8311_handle_t codec;

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

static esp_err_t rgb_test(void)
{
    esp_err_t err = ws2812_init();
    const uint8_t colors[][3] = {{16, 0, 0}, {0, 16, 0}, {0, 0, 16}, {0, 0, 0}};
    for (unsigned c = 0; c < 4 && err == ESP_OK; ++c) {
        err = ws2812_set(colors[c][0], colors[c][1], colors[c][2]);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
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
    codec = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
    if (!codec) return ESP_ERR_NO_MEM;
    es8311_clock_config_t clk = {.mclk_frequency = 16000 * 384, .sample_frequency = 16000,
                                 .mclk_from_mclk_pin = true};
    ESP_RETURN_ON_ERROR(es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "ES8311");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(codec, CONFIG_AIVOX3_SPEAKER_VOLUME, NULL), TAG, "volume");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(codec, false), TAG, "microphone");
    return es8311_microphone_gain_set(codec, (es8311_mic_gain_t)(CONFIG_AIVOX3_MIC_GAIN_DB / 6));
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
static esp_err_t lcd_test(void)
{
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "LCD init");
    ESP_RETURN_ON_ERROR(lcd_fill(0, 0, LCD_WIDTH, 80, LCD_RGB(255, 0, 0)), TAG, "red");
    ESP_RETURN_ON_ERROR(lcd_fill(0, 80, LCD_WIDTH, 80, LCD_RGB(0, 255, 0)), TAG, "green");
    ESP_RETURN_ON_ERROR(lcd_fill(0, 160, LCD_WIDTH, 80, LCD_RGB(0, 0, 255)), TAG, "blue");
    return lcd_text_row(112, "AI-VOX3", 2, LCD_RGB(0, 0, 0), LCD_RGB(0, 255, 0), true);
}
#endif

#if CONFIG_AIVOX3_HERMES_ENABLE
/* Wi-Fi, MQTT publishing and logging need more stack than the main task has. */
static void hermes_task(void *arg)
{
    hermes_run(rx_chan, tx_chan);
    ESP_LOGE(TAG, "Hermes stopped; reset the board after fixing the configuration");
    status_set_error("Hermes gestoppt");
    vTaskDelete(NULL);
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
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Hermes disabled: ES8311/I2S initialization failed");
    } else {
        status_start(codec, CONFIG_AIVOX3_HERMES_SITE_ID);
        if (xTaskCreate(hermes_task, "hermes", 8192, NULL, 5, NULL) == pdPASS) {
            ESP_LOGI(TAG, "starting Hermes voice satellite");
            vTaskDelete(NULL);
        }
        ESP_LOGE(TAG, "Hermes disabled: task creation failed");
        status_set_error("Hermes-Task fehlt");
    }
#endif
    ESP_LOGI(TAG, "reporting buttons every second");
    while (true) {
        ESP_LOGI(TAG, "BOOT=%d A=%d B=%d (0=pressed)", gpio_get_level(AIVOX3_BOOT),
                 gpio_get_level(AIVOX3_BUTTON_A), gpio_get_level(AIVOX3_BUTTON_B));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
