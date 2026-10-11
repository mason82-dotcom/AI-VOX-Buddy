/* Host unit tests for main/wav.c: ./scripts/test-host.sh */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wav.h"

static size_t make_wav(uint8_t *buf, uint16_t format, uint16_t channels, uint32_t rate,
                       const int16_t *samples, size_t count, bool extra_chunk)
{
    size_t p = 0;
    memcpy(buf, "RIFF", 4); memcpy(buf + 8, "WAVE", 4); p = 12;
    if (extra_chunk) {   /* Odd-sized LIST chunk with pad byte. */
        memcpy(buf + p, "LIST", 4); buf[p + 4] = 3; memset(buf + p + 5, 0, 3); p += 8 + 4;
    }
    uint8_t fmt[24] = "fmt ";
    fmt[4] = 16; fmt[8] = format & 0xff; fmt[9] = format >> 8; fmt[10] = channels;
    fmt[12] = rate & 0xff; fmt[13] = rate >> 8; fmt[14] = rate >> 16;
    uint32_t bytes_per_sec = rate * channels * 2;
    fmt[16] = bytes_per_sec & 0xff; fmt[17] = bytes_per_sec >> 8; fmt[18] = bytes_per_sec >> 16;
    fmt[20] = channels * 2; fmt[22] = 16;
    memcpy(buf + p, fmt, 24); p += 24;
    memcpy(buf + p, "data", 4);
    uint32_t data = count * 2;
    buf[p + 4] = data & 0xff; buf[p + 5] = data >> 8; buf[p + 6] = data >> 16; buf[p + 7] = 0;
    p += 8;
    for (size_t i = 0; i < count; ++i) { buf[p++] = samples[i] & 0xff; buf[p++] = (uint16_t)samples[i] >> 8; }
    uint32_t riff = p - 8;
    buf[4] = riff & 0xff; buf[5] = riff >> 8; buf[6] = riff >> 16; buf[7] = 0;
    return p;
}

static uint8_t wav[200000];
static int16_t in[96000], out[64000];

static void test_header_roundtrip(void)
{
    uint8_t h[WAV_HEADER_BYTES + 4] = {0};
    wav_header(h, 2);
    h[44] = 0x34; h[45] = 0x12; h[46] = 0xff; h[47] = 0xff;
    wav_info_t info;
    assert(wav_parse(h, sizeof(h), &info));
    assert(info.rate == 16000 && info.channels == 1 && info.frames == 2);
    uint64_t pos = 0;
    assert(wav_convert(&info, &pos, out, 10) == 2);
    assert(out[0] == 0x1234 && out[1] == -1);
}

static void test_passthrough_and_chunks(void)
{
    for (int i = 0; i < 1000; ++i) in[i] = (int16_t)(i * 37 - 18000);
    size_t len = make_wav(wav, 1, 1, 16000, in, 1000, true);
    wav_info_t info;
    assert(wav_parse(wav, len, &info));
    uint64_t pos = 0;
    size_t total = 0, n;
    while ((n = wav_convert(&info, &pos, out + total, 7)) > 0) total += n;
    assert(total == 1000);
    assert(!memcmp(in, out, 2000));
}

static void test_resample_22050_stereo(void)
{
    /* One second of stereo: left ramps, right is its negation plus 100. */
    for (int i = 0; i < 22050; ++i) { in[2 * i] = (int16_t)(i - 11025); in[2 * i + 1] = (int16_t)(11025 - i + 100); }
    size_t len = make_wav(wav, 0xfffe, 2, 22050, in, 44100, false);
    wav_info_t info;
    assert(wav_parse(wav, len, &info));
    assert(info.channels == 2 && info.rate == 22050 && info.frames == 22050);
    uint64_t pos = 0;
    size_t n = wav_convert(&info, &pos, out, 64000);
    assert(n >= 15999 && n <= 16001);
    for (size_t i = 0; i < n; ++i) assert(out[i] == 50);
}

static void test_resample_8000_interpolates(void)
{
    for (int i = 0; i < 8000; ++i) in[i] = (int16_t)(i * 4);
    size_t len = make_wav(wav, 1, 1, 8000, in, 8000, false);
    wav_info_t info;
    assert(wav_parse(wav, len, &info));
    uint64_t pos = 0;
    size_t n = wav_convert(&info, &pos, out, 64000);
    assert(n == 16000);
    for (size_t i = 0; i + 2 < n; ++i) assert(out[i] == (int16_t)(i * 2));
}

static void test_rejects(void)
{
    wav_info_t info;
    int16_t s[4] = {0};
    size_t len = make_wav(wav, 1, 1, 16000, s, 4, false);
    assert(!wav_parse(wav, 43, &info));
    assert(!wav_parse(wav, len - 1, &info));          /* truncated data chunk */
    make_wav(wav, 3, 1, 16000, s, 4, false);           /* IEEE float */
    assert(!wav_parse(wav, len, &info));
    make_wav(wav, 1, 3, 16000, s, 4, false);           /* three channels */
    assert(!wav_parse(wav, len, &info));
    make_wav(wav, 1, 1, 96000, s, 4, false);           /* rate too high */
    assert(!wav_parse(wav, len, &info));
    len = make_wav(wav, 1, 1, 16000, s, 0, false);     /* empty data */
    assert(!wav_parse(wav, len, &info));
    memcpy(wav, "RIFX", 4);
    assert(!wav_parse(wav, len, &info));
}

static void test_ids(void)
{
    assert(hermes_id_valid("3f2a-uuid_1.x:y", 15));
    assert(!hermes_id_valid("", 0));
    assert(!hermes_id_valid("a\"b", 3));
    assert(!hermes_id_valid("a\\b", 3));
    assert(!hermes_id_valid("a b", 3));
    assert(!hermes_id_valid("a/b", 3));
    char long_id[65]; memset(long_id, 'a', 65);
    assert(hermes_id_valid(long_id, 64));
    assert(!hermes_id_valid(long_id, 65));
}

int main(void)
{
    test_header_roundtrip();
    test_passthrough_and_chunks();
    test_resample_22050_stereo();
    test_resample_8000_interpolates();
    test_rejects();
    test_ids();
    puts("wav tests: all passed");
    return 0;
}
