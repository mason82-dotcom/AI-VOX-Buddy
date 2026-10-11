#include "status.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "ws2812.h"

#define TICK_MS 20
#define INFO_ROWS 7
#define HEADER_H 48
#define BAR_Y 204
#define BAR_H 12

static const char *TAG = "status";
static const int volume_steps[] = {25, 40, 55, 70, 85, 100};

static es8311_handle_t codec;
static const char *site = "";
static volatile status_state_t state = STATUS_BOOT;
static const char *volatile error_detail;
static volatile bool wifi_up, muted;
static volatile uint32_t ip_addr, frames;
static volatile int peak_level;
static int volume;

void status_set_state(status_state_t s) { state = s; }
void status_set_error(const char *detail)
{
    if (!error_detail) error_detail = detail;   /* Keep the first, most specific cause. */
    state = STATUS_ERROR;
}
void status_set_wifi(bool connected, uint32_t ip) { wifi_up = connected; ip_addr = connected ? ip : 0; }
bool status_mic_muted(void) { return muted; }
void status_note_frame(int peak) { ++frames; peak_level = peak; }

typedef struct { const char *label; uint8_t r, g, b; uint16_t lcd; bool blink; } look_t;

static look_t look(status_state_t s, bool mute)
{
    switch (s) {
    case STATUS_WIFI: return (look_t){"VERBINDE WLAN", 0, 0, 24, LCD_RGB(0, 70, 200), true};
    case STATUS_MQTT: return (look_t){"VERBINDE MQTT", 20, 14, 0, LCD_RGB(200, 140, 0), true};
    case STATUS_LISTENING:
        return mute ? (look_t){"STUMM", 24, 0, 0, LCD_RGB(200, 0, 0), false}
                    : (look_t){"HOERT ZU", 0, 16, 0, LCD_RGB(0, 150, 60), false};
    case STATUS_PLAYING: return (look_t){"SPRICHT", 0, 14, 18, LCD_RGB(0, 150, 170), false};
    case STATUS_ERROR: return (look_t){"FEHLER", 32, 0, 0, LCD_RGB(160, 0, 0), true};
    default: return (look_t){"START", 8, 8, 8, LCD_RGB(90, 90, 90), false};
    }
}

/* Debounced press detection: true once per press after two stable samples. */
typedef struct { gpio_num_t pin; int stable, last; } button_t;

static bool pressed(button_t *b)
{
    int level = gpio_get_level(b->pin);
    bool edge = false;
    if (level == b->last && level != b->stable) {
        edge = level == 0;
        b->stable = level;
    }
    b->last = level;
    return edge;
}

static void set_volume(int index)
{
    volume = volume_steps[index];
    if (es8311_voice_volume_set(codec, volume, NULL) != ESP_OK) ESP_LOGW(TAG, "volume change failed");
    else ESP_LOGI(TAG, "speaker volume %d", volume);
}

static int level_width(int peak)
{
    if (peak <= 0) return 0;
    float db = 20.0f * log10f((float)peak / 32768.0f);   /* -60..0 dBFS */
    int w = (int)((db + 60.0f) * (LCD_WIDTH - 16) / 60.0f);
    return w < 0 ? 0 : w > LCD_WIDTH - 16 ? LCD_WIDTH - 16 : w;
}

static void draw_info(char cache[INFO_ROWS][32], bool force)
{
    char text[INFO_ROWS][32];
    wifi_ap_record_t ap;
    if (wifi_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        snprintf(text[0], 32, "WLAN    %d dBm", ap.rssi);
    else snprintf(text[0], 32, "WLAN    getrennt");
    uint32_t ip = ip_addr;
    if (ip) snprintf(text[1], 32, "IP      %" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                     ip & 0xff, ip >> 8 & 0xff, ip >> 16 & 0xff, ip >> 24);
    else snprintf(text[1], 32, "IP      -");
    status_state_t s = state;
    snprintf(text[2], 32, "MQTT    %s", s == STATUS_LISTENING || s == STATUS_PLAYING ? "verbunden" : "-");
    snprintf(text[3], 32, "Site    %.20s", site);
    snprintf(text[4], 32, "Volume  %d %%", volume);
    snprintf(text[5], 32, "Frames  %" PRIu32, frames);
    if (s == STATUS_ERROR && error_detail) snprintf(text[6], 32, "%.28s", error_detail);
    else snprintf(text[6], 32, "A: Stumm  B: Lautst.");
    for (int i = 0; i < INFO_ROWS; ++i) {
        if (!force && !strcmp(cache[i], text[i])) continue;
        lcd_text_row(HEADER_H + 8 + i * 20, text[i], 1,
                     i == 6 ? LCD_RGB(150, 150, 150) : LCD_RGB(230, 230, 230), 0, false);
        strcpy(cache[i], text[i]);
    }
}

static void status_task(void *arg)
{
    button_t a = {AIVOX3_BUTTON_A, 1, 1}, b = {AIVOX3_BUTTON_B, 1, 1};
    int step = 0;
    while (step + 1 < (int)(sizeof(volume_steps) / sizeof(volume_steps[0])) &&
           volume_steps[step] < CONFIG_AIVOX3_SPEAKER_VOLUME) ++step;
    volume = CONFIG_AIVOX3_SPEAKER_VOLUME;
    bool lcd = lcd_ready();
    char cache[INFO_ROWS][32] = {{0}};
    status_state_t shown = (status_state_t)-1;
    bool shown_mute = false, led_on = false;
    int shown_bar = -1;
    for (uint32_t tick = 0;; ++tick) {
        if (pressed(&a)) {
            muted = !muted;
            ESP_LOGI(TAG, "microphone %s", muted ? "muted" : "unmuted");
        }
        if (pressed(&b)) {
            step = (step + 1) % (int)(sizeof(volume_steps) / sizeof(volume_steps[0]));
            set_volume(step);
        }
        status_state_t s = state;
        bool m = muted;
        look_t l = look(s, m);
        bool changed = s != shown || m != shown_mute;
        /* Blink at 1 Hz; refresh steady colors once per second. */
        bool want_on = !l.blink || (tick * TICK_MS / 500) % 2 == 0;
        if (changed || want_on != led_on || tick % (1000 / TICK_MS) == 0) {
            ws2812_set(want_on ? l.r : 0, want_on ? l.g : 0, want_on ? l.b : 0);
            led_on = want_on;
        }
        if (lcd) {
            if (changed) {
                if (shown == (status_state_t)-1) lcd_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
                lcd_fill(0, 0, LCD_WIDTH, 8, l.lcd);
                lcd_text_row(8, l.label, 2, LCD_RGB(255, 255, 255), l.lcd, true);
                lcd_fill(0, 40, LCD_WIDTH, 8, l.lcd);
            }
            if (changed || tick % (500 / TICK_MS) == 0) draw_info(cache, shown == (status_state_t)-1);
            if (tick % (100 / TICK_MS) == 0) {
                int w = s == STATUS_LISTENING && !m ? level_width(peak_level) : 0;
                if (w != shown_bar) {
                    if (w > 0) lcd_fill(8, BAR_Y, w, BAR_H, w > LCD_WIDTH - 40 ? LCD_RGB(230, 60, 0) : LCD_RGB(0, 200, 80));
                    if (w < LCD_WIDTH - 16) lcd_fill(8 + w, BAR_Y, LCD_WIDTH - 16 - w, BAR_H, LCD_RGB(40, 40, 40));
                    shown_bar = w;
                }
            }
        }
        shown = s;
        shown_mute = m;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

void status_start(es8311_handle_t c, const char *site_id)
{
    codec = c;
    site = site_id;
    if (ws2812_init() != ESP_OK) ESP_LOGW(TAG, "RGB LED unavailable");
    if (xTaskCreate(status_task, "status", 4096, NULL, 3, NULL) != pdPASS)
        ESP_LOGE(TAG, "status task creation failed");
}
