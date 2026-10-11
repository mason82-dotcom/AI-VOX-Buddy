#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure C helpers without ESP-IDF dependencies; unit-tested on the host. */

#define WAV_HEADER_BYTES 44
#define WAV_OUTPUT_RATE 16000

typedef struct {
    const uint8_t *pcm;   /* Interleaved little-endian PCM16 samples. */
    size_t frames;        /* Samples per channel. */
    uint32_t rate;        /* 8000..48000 Hz. */
    uint16_t channels;    /* 1 or 2. */
} wav_info_t;

/* Writes a 44-byte PCM16/mono/16-kHz header for `samples` samples. */
void wav_header(uint8_t *p, uint32_t samples);

/* Accepts PCM16 WAV with 1..2 channels and 8..48 kHz, skipping extra chunks. */
bool wav_parse(const uint8_t *wav, size_t len, wav_info_t *info);

/*
 * Converts to mono 16 kHz by channel averaging and linear interpolation.
 * `pos` is the 32.32 fixed-point read position in input frames; start at 0
 * and pass it back on each call. Returns the number of samples written;
 * 0 means the input is exhausted.
 */
size_t wav_convert(const wav_info_t *info, uint64_t *pos, int16_t *out, size_t out_max);

/* True for 1..64 characters that need no JSON escaping and are MQTT-safe. */
bool hermes_id_valid(const char *id, size_t len);
