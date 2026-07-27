/* Attachment MIME sniffing and PNG dimension probing, by validating the
 * signature and IHDR only. Deliberately zlib-free: png_thumb.c is the only
 * core file that may depend on zlib.
 */

#include "pspsky/png.h"

#include "pspsky/error.h"

#include <stddef.h>
#include <string.h>

/* PNG signature */
static const unsigned char png_signature[8] = {
    0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au
};

/* Read a big-endian 32-bit value */
static unsigned long png_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) |
           ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) |
           (unsigned long)p[3];
}

const char *pspsky_image_sniff_mime(const unsigned char *data, size_t len)
{
    if (data == NULL) {
        return NULL;
    }

    /* Check PNG first, then JPEG */
    if (len >= 8u && memcmp(data, png_signature, 8u) == 0) {
        return "image/png";
    }
    if (len >= 3u && data[0] == 0xFFu && data[1] == 0xD8u && data[2] == 0xFFu) {
        return "image/jpeg";
    }
    return NULL;
}

int pspsky_png_dimensions(const unsigned char *data, size_t len,
                          int *out_w, int *out_h)
{
    unsigned long w;
    unsigned long h;

    if (out_w != NULL) {
        *out_w = 0;
    }
    if (out_h != NULL) {
        *out_h = 0;
    }

    if (data == NULL || out_w == NULL || out_h == NULL || len == 0u) {
        return PSPSKY_E_ARG;
    }

    /* Needs signature 8 + IHDR length 4 + type 4 + width 4 + height 4 = 24 bytes */
    if (len < 24u) {
        return PSPSKY_E_PARSE;
    }
    if (memcmp(data, png_signature, 8u) != 0) {
        return PSPSKY_E_PARSE;
    }
    /* The first chunk must be an IHDR of length 13 */
    if (png_be32(data + 8) != 13uL) {
        return PSPSKY_E_PARSE;
    }
    if (memcmp(data + 12, "IHDR", 4u) != 0) {
        return PSPSKY_E_PARSE;
    }

    w = png_be32(data + 16);
    h = png_be32(data + 20);
    /* 0 and values >= 2^31 (top bit set) are invalid */
    if (w == 0uL || h == 0uL || w > 0x7FFFFFFFuL || h > 0x7FFFFFFFuL) {
        return PSPSKY_E_PARSE;
    }

    *out_w = (int)w;
    *out_h = (int)h;
    return PSPSKY_OK;
}
