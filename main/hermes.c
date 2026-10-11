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
#include "status.h"
#include "wav.h"

#define FRAME_SAMPLES 1600
#define PLAY_CHUNK_SAMPLES 800
/* After push-to-talk release: 300 ms of real audio, then 1 s of silence for VAD. */
#define PTT_TAIL_FRAMES 3
#define PTT_SILENCE_FRAMES 10
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
#if CONFIG_AIVOX3_PTT_HOTWORD
static const char hotword_topic[] = "hermes/hotword/push-to-talk/detected";
#endif

typedef struct {
    uint8_t *wav;
    size_t size;
    char id[65];
} play_job_t;

static play_job_t incoming;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_READY);
        mqtt_ready = false;
        status_set_wifi(false, 0);
        status_set_state(STATUS_WIFI);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_READY);
        status_set_wifi(true, ((ip_event_got_ip_t *)data)->ip_info.ip.addr);
        status_set_state(STATUS_MQTT);
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
            id_len > 64 || event->total_data_len < WAV_HEADER_BYTES ||
            event->total_data_len > MAX_PLAY_BYTES) {
            ESP_LOGW(TAG, "ignoring invalid/oversize playBytes");
            return;
        }
        if (!hermes_id_valid(event->topic + prefix_len, id_len)) {
            ESP_LOGW(TAG, "ignoring playBytes with unsupported request ID");
            return;
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
        status_set_state(playing ? STATUS_PLAYING : STATUS_CONNECTED);
        esp_mqtt_client_subscribe(event->client, play_subscription, 0);
        ESP_LOGI(TAG, "MQTT connected; subscribed to %s", play_subscription);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        mqtt_ready = false;
        release_incoming();
        if (xEventGroupGetBits(wifi_events) & WIFI_READY) status_set_state(STATUS_MQTT);
        ESP_LOGW(TAG, "MQTT disconnected");
    } else if (id == MQTT_EVENT_DATA) receive_audio(event);
    else if (id == MQTT_EVENT_ERROR) ESP_LOGE(TAG, "MQTT error");
}

static bool play_pcm(const wav_info_t *info)
{
    static int16_t out[PLAY_CHUNK_SAMPLES];
    uint64_t pos = 0;
    size_t n;
    while ((n = wav_convert(info, &pos, out, PLAY_CHUNK_SAMPLES)) > 0) {
        size_t sent = 0;
        if (i2s_channel_write(speaker, out, n * 2, &sent, 500) != ESP_OK || sent != n * 2) {
            ESP_LOGE(TAG, "I2S playback failed");
            return false;
        }
    }
    return true;
}

static void playback_task(void *arg)
{
    play_job_t job;
    while (xQueueReceive(play_queue, &job, portMAX_DELAY) == pdTRUE) {
        wav_info_t info;
        if (!wav_parse(job.wav, job.size, &info)) {
            ESP_LOGW(TAG, "playBytes %s: only PCM16 WAV, mono/stereo, 8..48 kHz supported", job.id);
            free(job.wav);
            continue;
        }
        if (info.rate != WAV_OUTPUT_RATE || info.channels != 1)
            ESP_LOGI(TAG, "playBytes %s: converting %" PRIu32 " Hz/%u ch to 16 kHz mono",
                     job.id, info.rate, info.channels);
        playing = true;
        status_set_state(STATUS_PLAYING);
        bool good = play_pcm(&info);
        /* Allow the final DMA buffers to drain before acknowledging playback. */
        if (good) vTaskDelay(pdMS_TO_TICKS(150));
        playing = false;
        if (mqtt_ready) status_set_state(STATUS_CONNECTED);
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
        status_set_error("WLAN/MQTT nicht gesetzt");
        return;
    }
    const char *site = CONFIG_AIVOX3_HERMES_SITE_ID;
    if (!site[0] || strlen(site) > 48) {
        ESP_LOGE(TAG, "site ID must contain 1..48 characters");
        status_set_error("Site-ID ungueltig"); return;
    }
    for (const char *p = site; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            ESP_LOGE(TAG, "site ID contains invalid character");
            status_set_error("Site-ID ungueltig"); return;
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
#if CONFIG_AIVOX3_WIFI_NO_POWER_SAVE
    /* Lower latency, but the constant RF activity can disturb USB on this board. */
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif
    status_set_state(STATUS_WIFI);
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
    if (xTaskCreate(playback_task, "hermes_play", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "playback task creation failed"); return;
    }
    uint8_t *frame = heap_caps_malloc(WAV_HEADER_BYTES + FRAME_SAMPLES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!frame) { ESP_LOGE(TAG, "no audio buffer"); return; }
    uint32_t frames = 0;
#if CONFIG_AIVOX3_PUSH_TO_TALK
    int tail = 0, silence = 0;
#endif
    while (true) {
        size_t got = 0;
        err = i2s_channel_read(rx, frame + WAV_HEADER_BYTES, FRAME_SAMPLES * 2, &got, 500);
        if (err != ESP_OK || !got) { ESP_LOGW(TAG, "microphone read: %s", esp_err_to_name(err)); continue; }
#if CONFIG_AIVOX3_PTT_HOTWORD
        if (status_take_ptt_press() && mqtt_ready) {
            char payload[200];
            int len = snprintf(payload, sizeof(payload),
                               "{\"modelId\":\"push-to-talk\",\"modelVersion\":\"\",\"modelType\":\"personal\","
                               "\"currentSensitivity\":1.0,\"siteId\":\"%s\"}", CONFIG_AIVOX3_HERMES_SITE_ID);
            if (len > 0 && len < sizeof(payload))
                esp_mqtt_client_publish(mqtt, hotword_topic, payload, len, 0, 0);
            ESP_LOGI(TAG, "push-to-talk: published hotword detected");
        }
#endif
        bool open = status_mic_open();
#if CONFIG_AIVOX3_PUSH_TO_TALK
        if (open) {
            tail = PTT_TAIL_FRAMES;
            silence = PTT_SILENCE_FRAMES;
        } else if (tail > 0) {
            --tail;
            open = true;
        } else if (silence > 0) {
            --silence;
            memset(frame + WAV_HEADER_BYTES, 0, got);
            open = true;
        }
#endif
        if (!playing && mqtt_ready && got % 2 == 0 && open) {
            const int16_t *pcm = (const int16_t *)(frame + WAV_HEADER_BYTES);
            int peak = 0;
            for (size_t i = 0; i < got / 2; ++i) {
                int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
                if (v > peak) peak = v;
            }
            status_note_frame(peak);
            wav_header(frame, got / 2);
            if (esp_mqtt_client_publish(mqtt, frame_topic, (char *)frame, got + WAV_HEADER_BYTES, 0, 0) >= 0 &&
                ++frames % 100 == 0)
                ESP_LOGI(TAG, "published %" PRIu32 " audio frames", frames);
        }
    }
}

#else

void hermes_run(i2s_chan_handle_t rx, i2s_chan_handle_t tx)
{
    (void)rx; (void)tx;
}

#endif
