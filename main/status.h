#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "es8311.h"

typedef enum {
    STATUS_BOOT,
    STATUS_WIFI,       /* Waiting for Wi-Fi/IP. */
    STATUS_MQTT,       /* Waiting for the broker. */
    STATUS_LISTENING,  /* Streaming microphone audio (or muted). */
    STATUS_PLAYING,    /* Playing playBytes audio. */
    STATUS_ERROR,
} status_state_t;

/*
 * Starts the status task: button A toggles microphone mute, button B steps
 * the speaker volume, the RGB LED and (if enabled) the LCD show the state.
 */
void status_start(es8311_handle_t codec, const char *site_id);
void status_set_state(status_state_t state);
/* `detail` must be a string literal; shown on the LCD for STATUS_ERROR. */
void status_set_error(const char *detail);
void status_set_wifi(bool connected, uint32_t ip);
bool status_mic_muted(void);
/* Records one published frame and its absolute peak sample. */
void status_note_frame(int peak);
