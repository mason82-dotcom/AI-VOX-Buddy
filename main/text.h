#pragma once

#include <stddef.h>

/*
 * Converts UTF-8 to printable ASCII for the 8x16 LCD font: German umlauts
 * become ae/oe/ue/ss, typographic quotes become ASCII quotes, other non-ASCII
 * characters become '?', control characters become spaces. Always
 * NUL-terminates `out` (if out_size > 0). Returns the output length.
 */
size_t text_to_ascii(const char *utf8, size_t len, char *out, size_t out_size);
