#include <stddef.h>

#include "pspsky/error.h"
#include "pspsky/jpeg.h"

#define JPEG_MARK_TEM  0x01  /* has no length field */
#define JPEG_MARK_RST0 0xD0  /* 0xD0..0xD7: have no length field */
#define JPEG_MARK_RST7 0xD7
#define JPEG_MARK_SOI  0xD8
#define JPEG_MARK_EOI  0xD9
#define JPEG_MARK_SOS  0xDA
#define JPEG_MARK_SOF0 0xC0
#define JPEG_MARK_SOF1 0xC1
#define JPEG_MARK_SOF2 0xC2

/* The caller must provide at least two bytes. */
static unsigned int be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

int pspsky_jpeg_dimensions(const unsigned char *data, size_t len,
                           int *out_w, int *out_h)
{
    size_t pos;

    if (data == NULL || out_w == NULL || out_h == NULL) {
        return PSPSKY_E_ARG;
    }

    if (len < 4 || data[0] != 0xFF || data[1] != JPEG_MARK_SOI) {
        return PSPSKY_E_PARSE;
    }

    pos = 2;
    for (;;) {
        unsigned int marker;
        unsigned int seglen;

        if (pos >= len) {
            return PSPSKY_E_PARSE;
        }
        if (data[pos] != 0xFF) {
            return PSPSKY_E_PARSE;
        }
        /* JPEG allows repeated 0xFF fill bytes. */
        while (data[pos] == 0xFF) {
            pos++;
            if (pos >= len) {
                return PSPSKY_E_PARSE;
            }
        }
        marker = (unsigned int)data[pos];
        pos++;

        /* Markers without a length field */
        if (marker == JPEG_MARK_TEM || marker == JPEG_MARK_SOI ||
            (marker >= JPEG_MARK_RST0 && marker <= JPEG_MARK_RST7)) {
            continue;
        }

        /* Reaching scan data or the end before any SOF means no dimensions */
        if (marker == JPEG_MARK_EOI || marker == JPEG_MARK_SOS) {
            return PSPSKY_E_PARSE;
        }

        if (pos + 2 > len) {
            return PSPSKY_E_PARSE;
        }
        seglen = be16(&data[pos]);  /* includes the two-byte length field */
        if (seglen < 2 || pos + (size_t)seglen > len) {
            return PSPSKY_E_PARSE;
        }

        if (marker == JPEG_MARK_SOF0 || marker == JPEG_MARK_SOF1 ||
            marker == JPEG_MARK_SOF2) {
            unsigned int h;
            unsigned int w;

            /* The payload needs one precision byte and two bytes per dimension. */
            if ((size_t)(seglen - 2) < 5) {
                return PSPSKY_E_PARSE;
            }
            h = be16(&data[pos + 3]);  /* data[pos+2] is the precision byte */
            w = be16(&data[pos + 5]);
            if (w == 0 || h == 0) {
                return PSPSKY_E_PARSE;
            }
            *out_w = (int)w;
            *out_h = (int)h;
            return PSPSKY_OK;
        }

        pos += (size_t)seglen;
    }
}
