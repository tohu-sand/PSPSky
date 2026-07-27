/* UTF-8 / UCS-2 conversion helpers. All internal strings are UTF-8; UCS-2 is
 * used only at the OSK boundary. */

#include <string.h>

#include "pspsky/utf8.h"
#include "pspsky/error.h"

#define UTF8_REPLACEMENT  0xFFFDu

unsigned int pspsky_utf8_next(const char **p)
{
    const unsigned char *s;
    unsigned int c0, cp;
    int len, i;

    if (p == NULL || *p == NULL) {
        return 0;
    }

    s = (const unsigned char *)(*p);
    c0 = (unsigned int)s[0];

    /* At NUL: return 0 without advancing *p */
    if (c0 == 0u) {
        return 0;
    }

    /* ASCII */
    if (c0 < 0x80u) {
        *p += 1;
        return c0;
    }

    if (c0 >= 0xC2u && c0 <= 0xDFu) {
        len = 2;
        cp = c0 & 0x1Fu;
    } else if (c0 >= 0xE0u && c0 <= 0xEFu) {
        len = 3;
        cp = c0 & 0x0Fu;
    } else if (c0 >= 0xF0u && c0 <= 0xF4u) {
        len = 4;
        cp = c0 & 0x07u;
    } else {
        /* Stray continuation byte (0x80-0xBF), overlong lead (0xC0/0xC1),
           or out of range (0xF5-0xFF) */
        *p += 1;
        return UTF8_REPLACEMENT;
    }

    for (i = 1; i < len; i++) {
        unsigned int cc = (unsigned int)s[i];
        /* This also rejects NUL (0x00) */
        if ((cc & 0xC0u) != 0x80u) {
            *p += 1;
            return UTF8_REPLACEMENT;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }

    /* Reject overlong forms, out-of-range values, and surrogates */
    if (len == 3 && cp < 0x800u) {
        *p += 1;
        return UTF8_REPLACEMENT;
    }
    if (len == 4 && (cp < 0x10000u || cp > 0x10FFFFu)) {
        *p += 1;
        return UTF8_REPLACEMENT;
    }
    if (cp >= 0xD800u && cp <= 0xDFFFu) {
        *p += 1;
        return UTF8_REPLACEMENT;
    }

    *p += len;
    return cp;
}

int pspsky_utf8_to_ucs2(const char *utf8, unsigned short *out, int out_cap)
{
    const char *p;
    int n = 0;

    if (utf8 == NULL || out == NULL || out_cap <= 0) {
        return 0;
    }

    p = utf8;
    while (n < out_cap - 1) {
        unsigned int cp = pspsky_utf8_next(&p);
        if (cp == 0u) {
            break;
        }
        if (cp > 0xFFFFu) {
            cp = UTF8_REPLACEMENT;   /* non-BMP is not representable in UCS-2 */
        }
        out[n++] = (unsigned short)cp;
    }
    out[n] = 0;

    return n;
}

int pspsky_ucs2_to_utf8(const unsigned short *in, char *out, size_t out_cap)
{
    size_t n = 0;
    int i;

    if (in == NULL || out == NULL || out_cap == 0) {
        return PSPSKY_E_ARG;
    }

    for (i = 0; in[i] != 0; i++) {
        unsigned int cp = (unsigned int)in[i];
        size_t need;

        if (cp < 0x80u) {
            need = 1;
        } else if (cp < 0x800u) {
            need = 2;
        } else {
            need = 3;
        }

        /* Stop while still leaving room for the NUL terminator */
        if (n + need > out_cap - 1) {
            break;
        }

        if (need == 1) {
            out[n++] = (char)cp;
        } else if (need == 2) {
            out[n++] = (char)(0xC0u | (cp >> 6));
            out[n++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            out[n++] = (char)(0xE0u | (cp >> 12));
            out[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[n++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    out[n] = '\0';

    return (int)n;
}

size_t pspsky_utf8_truncate(char *s, size_t max_bytes)
{
    const char *p;
    size_t len;

    if (s == NULL) {
        return 0;
    }

    len = strlen(s);
    if (len <= max_bytes) {
        return len;
    }

    /* Walk code point boundaries and cut at the last one within max_bytes */
    p = s;
    for (;;) {
        const char *q = p;
        if (pspsky_utf8_next(&q) == 0u) {
            break;
        }
        if ((size_t)(q - s) > max_bytes) {
            break;
        }
        p = q;
    }

    len = (size_t)(p - s);
    s[len] = '\0';
    return len;
}

int pspsky_utf8_count(const char *s)
{
    const char *p;
    int n = 0;

    if (s == NULL) {
        return 0;
    }

    p = s;
    while (pspsky_utf8_next(&p) != 0u) {
        n++;
    }
    return n;
}
