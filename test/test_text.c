/* Host unit tests for main/text.c: ./scripts/test-host.sh */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "text.h"

static void check(const char *in, const char *expected, size_t out_size)
{
    char out[64];
    size_t n = text_to_ascii(in, strlen(in), out, out_size);
    if (strcmp(out, expected) || n != strlen(expected)) {
        printf("text_to_ascii(\"%s\") = \"%s\", expected \"%s\"\n", in, out, expected);
        assert(0);
    }
}

int main(void)
{
    check("Wie spät ist es?", "Wie spaet ist es?", 64);
    check("Größe Übermaß", "Groesse Uebermass", 64);
    check("\xe2\x80\x9eHallo\xe2\x80\x9c \xe2\x80\x93 ok", "\"Hallo\" - ok", 64);
    check("caf\xc3\xa9 \xf0\x9f\x98\x80!", "caf? ?!", 64);   /* é and an emoji */
    check("a\tb\nc", "a b c", 64);
    check("Übung", "Ue", 3);                                  /* truncation */
    check("abc", "", 1);
    check("\xc3", "?", 64);                                   /* truncated sequence */
    puts("text tests: all passed");
    return 0;
}
