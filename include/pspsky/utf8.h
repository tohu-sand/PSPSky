/* UTF-8 / UCS-2 conversion helpers. Portable C99, libc only. */
#ifndef PSPSKY_UTF8_H
#define PSPSKY_UTF8_H

#include <stddef.h>

/* Decodes one code point and advances *p. An invalid byte yields 0xFFFD and
   advances exactly one byte, so callers always make progress. */
unsigned int pspsky_utf8_next(const char **p);

/* Non-BMP code points become 0xFFFD; out_cap counts elements including the NUL */
int pspsky_utf8_to_ucs2(const char *utf8, unsigned short *out, int out_cap);

/* Returns the byte length written, excluding the NUL */
int pspsky_ucs2_to_utf8(const unsigned short *in, char *out, size_t out_cap);

/* Truncates in place on a code point boundary, never leaving a partial sequence */
size_t pspsky_utf8_truncate(char *s, size_t max_bytes);

/* Counts code points; each invalid byte counts as one */
int pspsky_utf8_count(const char *s);

#endif /* PSPSKY_UTF8_H */
