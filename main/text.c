#include "text.h"

#include <string.h>

typedef struct { const char *utf8; const char *ascii; } translit_t;

static const translit_t map[] = {
    {"\xc3\xa4", "ae"}, {"\xc3\xb6", "oe"}, {"\xc3\xbc", "ue"},
    {"\xc3\x84", "Ae"}, {"\xc3\x96", "Oe"}, {"\xc3\x9c", "Ue"}, {"\xc3\x9f", "ss"},
    {"\xe2\x80\x9e", "\""}, {"\xe2\x80\x9c", "\""}, {"\xe2\x80\x9d", "\""},
    {"\xe2\x80\x98", "'"}, {"\xe2\x80\x99", "'"}, {"\xe2\x80\x93", "-"}, {"\xe2\x80\x94", "-"},
};

size_t text_to_ascii(const char *utf8, size_t len, char *out, size_t out_size)
{
    if (!out_size) return 0;
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < out_size;) {
        unsigned char c = (unsigned char)utf8[i];
        if (c < 0x80) {
            out[o++] = c < ' ' || c == 0x7f ? ' ' : (char)c;
            ++i;
            continue;
        }
        const char *repl = "?";
        size_t used = 1;
        for (size_t m = 0; m < sizeof(map) / sizeof(map[0]); ++m) {
            size_t n = strlen(map[m].utf8);
            if (n <= len - i && !memcmp(utf8 + i, map[m].utf8, n)) { repl = map[m].ascii; used = n; break; }
        }
        if (used == 1)   /* Unknown sequence: skip its continuation bytes. */
            while (i + used < len && ((unsigned char)utf8[i + used] & 0xc0) == 0x80) ++used;
        for (const char *r = repl; *r && o + 1 < out_size; ++r) out[o++] = *r;
        i += used;
    }
    out[o] = '\0';
    return o;
}
