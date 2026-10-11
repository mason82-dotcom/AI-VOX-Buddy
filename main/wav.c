#include "wav.h"

#include <string.h>

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

void wav_header(uint8_t *p, uint32_t samples)
{
    memcpy(p, "RIFF", 4); put32(p + 4, samples * 2 + 36);
    memcpy(p + 8, "WAVEfmt ", 8); put32(p + 16, 16);
    put16(p + 20, 1); put16(p + 22, 1); put32(p + 24, WAV_OUTPUT_RATE);
    put32(p + 28, WAV_OUTPUT_RATE * 2); put16(p + 32, 2); put16(p + 34, 16);
    memcpy(p + 36, "data", 4); put32(p + 40, samples * 2);
}

bool wav_parse(const uint8_t *wav, size_t len, wav_info_t *info)
{
    if (len < WAV_HEADER_BYTES || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return false;
    bool fmt_ok = false;
    size_t offset = 12;
    while (offset + 8 <= len) {
        uint32_t size = le32(wav + offset + 4);
        size_t data = offset + 8;
        if (size > len - data) return false;
        if (!memcmp(wav + offset, "fmt ", 4) && size >= 16) {
            uint16_t format = le16(wav + data), channels = le16(wav + data + 2);
            uint32_t rate = le32(wav + data + 4);
            /* WAVE_FORMAT_EXTENSIBLE (0xfffe) is accepted for plain 16-bit PCM. */
            fmt_ok = (format == 1 || format == 0xfffe) && channels >= 1 && channels <= 2 &&
                     rate >= 8000 && rate <= 48000 && le16(wav + data + 14) == 16 &&
                     le16(wav + data + 12) == channels * 2;
            info->channels = channels;
            info->rate = rate;
        } else if (!memcmp(wav + offset, "data", 4) && fmt_ok) {
            info->pcm = wav + data;
            info->frames = size / (2U * info->channels);
            return info->frames > 0;
        }
        offset = data + size + (size & 1U);
    }
    return false;
}

static int sample(const wav_info_t *info, size_t frame)
{
    const uint8_t *p = info->pcm + frame * 2U * info->channels;
    int s = (int16_t)le16(p);
    if (info->channels == 2) s = (s + (int16_t)le16(p + 2)) / 2;
    return s;
}

size_t wav_convert(const wav_info_t *info, uint64_t *pos, int16_t *out, size_t out_max)
{
    const uint64_t step = ((uint64_t)info->rate << 32) / WAV_OUTPUT_RATE;
    size_t n = 0;
    while (n < out_max) {
        size_t i = (size_t)(*pos >> 32);
        if (i >= info->frames) break;
        int a = sample(info, i);
        if (i + 1 < info->frames) {
            int frac = (int)((*pos >> 16) & 0xffff);
            a += (int)(((int64_t)(sample(info, i + 1) - a) * frac) >> 16);
        }
        out[n++] = (int16_t)a;
        *pos += step;
    }
    return n;
}

bool hermes_id_valid(const char *id, size_t len)
{
    if (len == 0 || len > 64) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)id[i];
        if (ch <= ' ' || ch >= 0x7f || ch == '"' || ch == '\\' || ch == '+' || ch == '#' || ch == '/')
            return false;
    }
    return true;
}
