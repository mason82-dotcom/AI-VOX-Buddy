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
#include "text.h"
#include "ws2812.h"

#define TICK_MS 20
#define INFO_ROWS 9      /* 8 info rows + key hint */
#define HEADER_H 48
#define ROW_Y(i) (52 + (i) * 18)
#define BAR_Y 200
#define BAR_H 10
#define HINT_Y 222
#define LINE_CHARS 28    /* (240 - 16) / 8 */

static const char *TAG = "status";
static const int volume_steps[] = {25, 40, 55, 70, 85, 100};

static es8311_handle_t codec;
static const char *site = "";
static volatile status_state_t state = STATUS_BOOT;
static const char *volatile error_detail;
static volatile bool wifi_up, ptt_pressed;
#if CONFIG_AIVOX3_PUSH_TO_TALK
static volatile bool ptt_held;
#else
static volatile bool muted;
#endif
static volatile uint32_t ip_addr, frames;
static volatile int peak_level;
static volatile int volume = CONFIG_AIVOX3_SPEAKER_VOLUME, requested_volume = -1;

static portMUX_TYPE dialog_lock = portMUX_INITIALIZER_UNLOCKED;
static dialog_phase_t dialog_phase;
static char heard[LINE_CHARS + 1], intent[LINE_CHARS + 1], answer[2 * LINE_CHARS + 1], ended[LINE_CHARS + 1];

void status_dialog(dialog_phase_t phase, const char *text, size_t len)
{
    char ascii[2 * LINE_CHARS + 1] = "";
    if (text) text_to_ascii(text, len, ascii, sizeof(ascii));
    portENTER_CRITICAL(&dialog_lock);
    dialog_phase = phase;
    switch (phase) {
    case DIALOG_SESSION: heard[0] = intent[0] = answer[0] = ended[0] = '\0'; break;
    case DIALOG_HEARD: strlcpy(heard, ascii, sizeof(heard)); break;
    case DIALOG_INTENT: strlcpy(intent, ascii, sizeof(intent)); break;
    case DIALOG_SPEAKING: strlcpy(answer, ascii, sizeof(answer)); break;
    case DIALOG_ENDED: strlcpy(ended, ascii, sizeof(ended)); break;
    default: break;
    }
    portEXIT_CRITICAL(&dialog_lock);
}

static const char *dialog_label(dialog_phase_t p)
{
    switch (p) {
    case DIALOG_SESSION: return "Sitzung";
    case DIALOG_LISTENING: return "hoert zu";
    case DIALOG_HEARD: return "verarbeitet";
    case DIALOG_INTENT: return "erkannt";
    case DIALOG_NOT_UNDERSTOOD: return "nicht verstanden";
    case DIALOG_SPEAKING: return "spricht";
    case DIALOG_ENDED: return "bereit";
    default: return "-";
    }
}

/* Splits `text` at a space so that the first part fits one LCD line. */
static void wrap(const char *text, char *line1, char *line2)
{
    size_t n = strlen(text);
    size_t cut = n <= LINE_CHARS ? n : LINE_CHARS;
    if (n > LINE_CHARS)
        for (size_t i = LINE_CHARS; i > LINE_CHARS / 2; --i)
            if (text[i] == ' ') { cut = i; break; }
    snprintf(line1, LINE_CHARS + 1, "%.*s", (int)cut, text);
    while (text[cut] == ' ') ++cut;
    snprintf(line2, LINE_CHARS + 1, "%s", text + cut);
}

void status_set_state(status_state_t s) { state = s; }
void status_set_error(const char *detail)
{
    if (!error_detail) error_detail = detail;   /* Keep the first, most specific cause. */
    state = STATUS_ERROR;
}
void status_set_wifi(bool connected, uint32_t ip) { wifi_up = connected; ip_addr = connected ? ip : 0; }
#if CONFIG_AIVOX3_PUSH_TO_TALK
bool status_mic_open(void) { return ptt_held; }
#else
bool status_mic_open(void) { return !muted; }
#endif

bool status_take_ptt_press(void)
{
    bool p = ptt_pressed;
    ptt_pressed = false;
    return p;
}
void status_note_frame(int peak) { ++frames; peak_level = peak; }
void status_set_volume(int percent) { requested_volume = percent < 0 ? 0 : percent > 100 ? 100 : percent; }
int status_volume(void) { return volume; }

typedef struct { const char *label; uint8_t r, g, b; uint16_t lcd; bool blink; } look_t;

static look_t look(status_state_t s, bool open)
{
    switch (s) {
    case STATUS_WIFI: return (look_t){"VERBINDE WLAN", 0, 0, 24, LCD_RGB(0, 70, 200), true};
    case STATUS_MQTT: return (look_t){"VERBINDE MQTT", 20, 14, 0, LCD_RGB(200, 140, 0), true};
    case STATUS_CONNECTED:
        if (open) return (look_t){"HOERT ZU", 0, 24, 0, LCD_RGB(0, 150, 60), false};
#if CONFIG_AIVOX3_PUSH_TO_TALK
        return (look_t){"BEREIT", 5, 5, 5, LCD_RGB(70, 70, 80), false};
#else
        return (look_t){"STUMM", 24, 0, 0, LCD_RGB(200, 0, 0), false};
#endif
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

#define VOLUME_STEPS ((int)(sizeof(volume_steps) / sizeof(volume_steps[0])))

static void apply_volume(int percent)
{
    if (es8311_voice_volume_set(codec, percent, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "volume change failed");
        return;
    }
    volume = percent;
    ESP_LOGI(TAG, "speaker volume %d", percent);
}

/* Index of the first step above `percent`, so button B always gets louder first. */
static int next_step(int percent)
{
    for (int i = 0; i < VOLUME_STEPS; ++i)
        if (volume_steps[i] > percent) return i;
    return 0;
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
    status_state_t s = state;
    wifi_ap_record_t ap;
    if (wifi_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        snprintf(text[0], 32, "WLAN %d dBm", ap.rssi);
    else snprintf(text[0], 32, "WLAN getrennt");
    uint32_t ip = ip_addr;
    if (ip) snprintf(text[1], 32, "IP   %" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                     ip & 0xff, ip >> 8 & 0xff, ip >> 16 & 0xff, ip >> 24);
    else snprintf(text[1], 32, "IP   -");
    snprintf(text[2], 32, "MQTT %-3s Vol %d%% %.8s",
             s == STATUS_CONNECTED || s == STATUS_PLAYING ? "ok" : "-", volume, site);

    char h[LINE_CHARS + 1], in[LINE_CHARS + 1], an[2 * LINE_CHARS + 1], en[LINE_CHARS + 1];
    portENTER_CRITICAL(&dialog_lock);
    dialog_phase_t p = dialog_phase;
    memcpy(h, heard, sizeof(h)); memcpy(in, intent, sizeof(in));
    memcpy(an, answer, sizeof(an)); memcpy(en, ended, sizeof(en));
    portEXIT_CRITICAL(&dialog_lock);
    if (s == STATUS_ERROR && error_detail) snprintf(text[3], 32, "%.28s", error_detail);
    else if (p == DIALOG_ENDED && en[0]) snprintf(text[3], 32, "Rhasspy: %.19s", en);
    else snprintf(text[3], 32, "Rhasspy: %s", dialog_label(p));
    snprintf(text[4], 32, "> %.26s", h);
    snprintf(text[5], 32, "Intent: %.20s", p == DIALOG_NOT_UNDERSTOOD ? "?" : in);
    wrap(an, text[6], text[7]);
#if CONFIG_AIVOX3_PUSH_TO_TALK
    snprintf(text[8], 32, "A halten=Sprechen B=Lautst.");
#else
    snprintf(text[8], 32, "A: Stumm  B: Lautst.");
#endif
    for (int i = 0; i < INFO_ROWS; ++i) {
        if (!force && !strcmp(cache[i], text[i])) continue;
        uint16_t fg = i == 8 ? LCD_RGB(150, 150, 150) : i >= 6 ? LCD_RGB(120, 220, 255)
                    : i == 3 ? LCD_RGB(255, 210, 120) : LCD_RGB(230, 230, 230);
        lcd_text_row(i == 8 ? HINT_Y : ROW_Y(i), text[i], 1, fg, 0, false);
        strcpy(cache[i], text[i]);
    }
}

static void status_task(void *arg)
{
    button_t a = {AIVOX3_BUTTON_A, 1, 1}, b = {AIVOX3_BUTTON_B, 1, 1};
    volume = CONFIG_AIVOX3_SPEAKER_VOLUME;
    bool lcd = lcd_ready();
    char cache[INFO_ROWS][32] = {{0}};
    status_state_t shown = (status_state_t)-1;
    bool shown_mute = false, led_on = false;
    int shown_bar = -1;
    for (uint32_t tick = 0;; ++tick) {
        bool a_down = pressed(&a);
#if CONFIG_AIVOX3_PUSH_TO_TALK
        bool held = a.stable == 0 && state == STATUS_CONNECTED;
        if (a_down && held) ptt_pressed = true;
        if (held != ptt_held) ESP_LOGI(TAG, "push-to-talk %s", held ? "pressed" : "released");
        ptt_held = held;
#else
        if (a_down) {
            muted = !muted;
            ESP_LOGI(TAG, "microphone %s", muted ? "muted" : "unmuted");
        }
#endif
        if (pressed(&b)) apply_volume(volume_steps[next_step(volume)]);
        int req = requested_volume;
        if (req >= 0) {
            requested_volume = -1;
            apply_volume(req);
        }
        status_state_t s = state;
        bool m = status_mic_open();
        look_t l = look(s, m);
        bool changed = s != shown || m != shown_mute;
        if (changed) ESP_LOGI(TAG, "state %s", l.label);
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
            if (changed || tick % (200 / TICK_MS) == 0) draw_info(cache, shown == (status_state_t)-1);
            if (tick % (100 / TICK_MS) == 0) {
                int w = s == STATUS_CONNECTED && m ? level_width(peak_level) : 0;
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
