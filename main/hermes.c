#include "hermes.h"

#if CONFIG_AIVOX3_HERMES_ENABLE

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define FRAME_SAMPLES 1600
#define MAX_PLAY_BYTES (512 * 1024)
#define WIFI_READY BIT0

static const char *TAG = "hermes";
static EventGroupHandle_t wifi_events;
static QueueHandle_t play_queue;
static esp_mqtt_client_handle_t mqtt;
static i2s_chan_handle_t speaker;
static volatile bool mqtt_ready;
static volatile bool playing;
static char frame_topic[112];
static char play_prefix[112];
static char play_subscription[114];
static char finished_topic[112];

typedef struct {
    uint8_t *wav;
    size_t size;
    char id[65];
} play_job_t;

static play_job_t incoming;

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void wav_header(uint8_t *p, uint32_t samples)
{
    memcpy(p, "RIFF", 4); put32(p + 4, samples * 2 + 36);
    memcpy(p + 8, "WAVEfmt ", 8); put32(p + 16, 16);
    put16(p + 20, 1); put16(p + 22, 1); put32(p + 24, 16000);
    put32(p + 28, 32000); put16(p + 32, 2); put16(p + 34, 16);
    memcpy(p + 36, "data", 4); put32(p + 40, samples * 2);
}

/* Accept PCM16/mono/16 kHz WAV, including extra RIFF chunks before data. */
static bool wav_pcm(const uint8_t *wav, size_t len, const uint8_t **pcm, size_t *bytes)
{
    if (len < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return false;
    bool fmt_ok = false;
    size_t offset = 12;
    while (offset + 8 <= len) {
        uint32_t size = le32(wav + offset + 4);
        size_t data = offset + 8;
        if (size > len - data) return false;
        if (!memcmp(wav + offset, "fmt ", 4) && size >= 16) {
            fmt_ok = le16(wav + data) == 1 && le16(wav + data + 2) == 1 &&
                     le32(wav + data + 4) == 16000 && le16(wav + data + 14) == 16;
        } else if (!memcmp(wav + offset, "data", 4) && fmt_ok) {
            *pcm = wav + data; *bytes = size & ~1U; return true;
        }
        offset = data + size + (size & 1U);
    }
    return false;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_READY);
        mqtt_ready = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_READY);
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static void release_incoming(void)
{
    free(incoming.wav);
    memset(&incoming, 0, sizeof(incoming));
}

static void receive_audio(esp_mqtt_event_handle_t event)
{
    if (event->current_data_offset == 0) {
        release_incoming();
        size_t prefix_len = strlen(play_prefix);
        size_t id_len = event->topic_len > (int)prefix_len ?
                        event->topic_len - prefix_len : 0;
        if (event->topic_len <= (int)prefix_len ||
            memcmp(event->topic, play_prefix, prefix_len) ||
            id_len > 64 || event->total_data_len < 44 ||
            event->total_data_len > MAX_PLAY_BYTES) {
            ESP_LOGW(TAG, "ignoring invalid/oversize playBytes");
            return;
        }
        for (size_t i = 0; i < id_len; ++i) {
            unsigned char ch = event->topic[prefix_len + i];
            if (!isalnum(ch) && ch != '-' && ch != '_') return;
        }
        memcpy(incoming.id, event->topic + prefix_len, id_len);
        incoming.size = event->total_data_len;
        incoming.wav = heap_caps_malloc(incoming.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!incoming.wav) ESP_LOGE(TAG, "no PSRAM for playBytes (%u bytes)", (unsigned)incoming.size);
    }
    if (!incoming.wav || event->current_data_offset < 0 || event->data_len < 0 ||
        (size_t)event->current_data_offset > incoming.size ||
        (size_t)event->data_len > incoming.size - event->current_data_offset) return;
    memcpy(incoming.wav + event->current_data_offset, event->data, event->data_len);
    if ((size_t)event->current_data_offset + event->data_len == incoming.size) {
        if (xQueueSend(play_queue, &incoming, 0) != pdTRUE) {
            ESP_LOGW(TAG, "playback queue full; dropping audio");
            release_incoming();
        } else memset(&incoming, 0, sizeof(incoming));
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = data;
    if (id == MQTT_EVENT_CONNECTED) {
        mqtt_ready = true;
        esp_mqtt_client_subscribe(event->client, play_subscription, 0);
        ESP_LOGI(TAG, "MQTT connected; subscribed to %s", play_subscription);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        mqtt_ready = false;
        release_incoming();
        ESP_LOGW(TAG, "MQTT disconnected");
    } else if (id == MQTT_EVENT_DATA) receive_audio(event);
    else if (id == MQTT_EVENT_ERROR) ESP_LOGE(TAG, "MQTT error");
}

static void playback_task(void *arg)
{
    play_job_t job;
    while (xQueueReceive(play_queue, &job, portMAX_DELAY) == pdTRUE) {
        const uint8_t *pcm;
        size_t bytes;
        if (!wav_pcm(job.wav, job.size, &pcm, &bytes)) {
            ESP_LOGW(TAG, "playBytes %s: only PCM16 mono 16 kHz WAV supported", job.id);
            free(job.wav);
            continue;
        }
        playing = true;
        bool good = true;
        for (size_t offset = 0; offset < bytes;) {
            size_t chunk = bytes - offset > 3200 ? 3200 : bytes - offset;
            size_t sent = 0;
            if (i2s_channel_write(speaker, pcm + offset, chunk, &sent,
                                  500) != ESP_OK || sent != chunk) {
                ESP_LOGE(TAG, "I2S playback failed"); good = false; break;
            }
            offset += chunk;
        }
        /* Allow the final DMA buffers to drain before acknowledging playback. */
        if (good) vTaskDelay(pdMS_TO_TICKS(150));
        playing = false;
        if (good && mqtt_ready) {
            char payload[160];
            int len = snprintf(payload, sizeof(payload), "{\"id\":\"%s\",\"siteId\":\"%s\"}",
                               job.id, CONFIG_AIVOX3_HERMES_SITE_ID);
            if (len > 0 && len < sizeof(payload))
                esp_mqtt_client_publish(mqtt, finished_topic, payload, len, 0, 0);
        }
        free(job.wav);
    }
}

void hermes_run(i2s_chan_handle_t rx, i2s_chan_handle_t tx)
{
    if (!CONFIG_AIVOX3_WIFI_SSID[0] || !CONFIG_AIVOX3_MQTT_URI[0]) {
        ESP_LOGE(TAG, "set Wi-Fi SSID and MQTT URI in menuconfig; satellite not started");
        return;
    }
    const char *site = CONFIG_AIVOX3_HERMES_SITE_ID;
    if (!site[0] || strlen(site) > 48) {
        ESP_LOGE(TAG, "site ID must contain 1..48 characters"); return;
    }
    for (const char *p = site; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            ESP_LOGE(TAG, "site ID contains invalid character"); return;
        }
    snprintf(frame_topic, sizeof(frame_topic), "hermes/audioServer/%s/audioFrame", site);
    snprintf(play_prefix, sizeof(play_prefix), "hermes/audioServer/%s/playBytes/", site);
    snprintf(play_subscription, sizeof(play_subscription), "%s+", play_prefix);
    snprintf(finished_topic, sizeof(finished_topic), "hermes/audioServer/%s/playFinished", site);
    speaker = tx;
    play_queue = xQueueCreate(2, sizeof(play_job_t));
    wifi_events = xEventGroupCreate();
    if (!play_queue || !wifi_events) { ESP_LOGE(TAG, "out of memory"); return; }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS needs repair; refusing to erase existing settings"); return;
    }
    if (err != ESP_OK || esp_netif_init() != ESP_OK ||
        esp_event_loop_create_default() != ESP_OK) {
        ESP_LOGE(TAG, "network initialization failed"); return;
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) { ESP_LOGE(TAG, "Wi-Fi init failed"); return; }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, CONFIG_AIVOX3_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, CONFIG_AIVOX3_WIFI_PASSWORD, sizeof(config.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &config);
    esp_wifi_start();
    ESP_LOGI(TAG, "waiting for Wi-Fi");
    xEventGroupWaitBits(wifi_events, WIFI_READY, pdFALSE, pdTRUE, portMAX_DELAY);
    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = CONFIG_AIVOX3_MQTT_URI,
        .credentials.username = CONFIG_AIVOX3_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_AIVOX3_MQTT_PASSWORD,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
    };
    mqtt = esp_mqtt_client_init(&mqtt_config);
    if (!mqtt) { ESP_LOGE(TAG, "MQTT init failed"); return; }
    esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (esp_mqtt_client_start(mqtt) != ESP_OK) { ESP_LOGE(TAG, "MQTT start failed"); return; }
    xTaskCreate(playback_task, "hermes_play", 6144, NULL, 5, NULL);
    uint8_t *frame = heap_caps_malloc(44 + FRAME_SAMPLES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!frame) { ESP_LOGE(TAG, "no audio buffer"); return; }
    uint32_t frames = 0;
    while (true) {
        size_t got = 0;
        err = i2s_channel_read(rx, frame + 44, FRAME_SAMPLES * 2, &got, 500);
        if (err != ESP_OK || !got) { ESP_LOGW(TAG, "microphone read: %s", esp_err_to_name(err)); continue; }
        if (!playing && mqtt_ready && got % 2 == 0) {
            wav_header(frame, got / 2);
            if (esp_mqtt_client_publish(mqtt, frame_topic, (char *)frame, got + 44, 0, 0) >= 0)
                ++frames;
            if (frames && frames % 100 == 0) ESP_LOGI(TAG, "published %" PRIu32 " audio frames", frames);
        }
    }
}

#else

void hermes_run(i2s_chan_handle_t rx, i2s_chan_handle_t tx)
{
    (void)rx; (void)tx;
}

#endif
