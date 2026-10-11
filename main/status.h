#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "es8311.h"

typedef enum {
    STATUS_BOOT,
    STATUS_WIFI,       /* Waiting for Wi-Fi/IP. */
    STATUS_MQTT,       /* Waiting for the broker. */
    STATUS_CONNECTED,  /* MQTT up; streams while the microphone is open. */
    STATUS_PLAYING,    /* Playing playBytes audio. */
    STATUS_ERROR,
} status_state_t;

/*
 * Starts the status task. Button A is push-to-talk (or toggles mute when
 * CONFIG_AIVOX3_PUSH_TO_TALK is off), button B steps the speaker volume, and
 * the RGB LED and (if enabled) the LCD show the state.
 */
void status_start(es8311_handle_t codec, const char *site_id);
void status_set_state(status_state_t state);
/* `detail` must be a string literal; shown on the LCD for STATUS_ERROR. */
void status_set_error(const char *detail);
void status_set_wifi(bool connected, uint32_t ip);
/* True while microphone audio may be streamed (A held, or not muted). */
bool status_mic_open(void);
/* Returns true once per push-to-talk press (for the hotword message). */
bool status_take_ptt_press(void);
/* Hermes dialogue progress (e.g. from Rhasspy) for this site. */
typedef enum {
    DIALOG_NONE,          /* No session seen yet. */
    DIALOG_SESSION,       /* Session started; clears previous texts. */
    DIALOG_LISTENING,     /* ASR listening. */
    DIALOG_HEARD,         /* text = recognized speech. */
    DIALOG_INTENT,        /* text = intent name. */
    DIALOG_NOT_UNDERSTOOD,
    DIALOG_SPEAKING,      /* text = TTS answer. */
    DIALOG_ENDED,         /* text = termination reason if not "nominal". */
} dialog_phase_t;

/* Thread-safe; `text` is UTF-8 (converted to ASCII for the LCD), may be NULL. */
void status_dialog(dialog_phase_t phase, const char *text, size_t len);

/* Requests a speaker volume (0..100), applied by the status task. */
void status_set_volume(int percent);
/* Current speaker volume in percent. */
int status_volume(void);

/* Records one published frame and its absolute peak sample. */
void status_note_frame(int peak);
