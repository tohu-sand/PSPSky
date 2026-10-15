/*
 * Keep only two decoded rows (up to 128 KiB) plus the output in memory.
 * A full-resolution buffer exceeds PSP memory limits.
 */

#include "pspsky/png.h"

#include "pspsky/error.h"

/* Keep zlib input pointers const. */
#define ZLIB_CONST 1
#include <zlib.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PNG_MAX_PLTE_ENTRIES 256

typedef struct {
    int w;
    int h;
    int depth;  /* 8 or 16 */
    int color;  /* 0/2/3/4/6 */
    int n;  /* decimation factor, >= 1 */
    int thumb_w;
    int thumb_h;
    size_t bpp;  /* channels * depth/8 */
    size_t row_bytes;  /* 1 + w * bpp; the leading byte is the filter type */
    unsigned char *prev;  /* previous row, already unfiltered */
    unsigned char *cur;
    size_t filled;  /* bytes accumulated in cur */
    int y;  /* next row to complete */
    unsigned char *out;
    unsigned char plte[PNG_MAX_PLTE_ENTRIES * 3];
    int plte_entries;
    int have_plte;
} png_dec_t;

static unsigned long png_thumb_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) |
           ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) |
           (unsigned long)p[3];
}

static int png_paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p - a;
    int pb = p - b;
    int pc = p - c;

    if (pa < 0) { pa = -pa; }
    if (pb < 0) { pb = -pb; }
    if (pc < 0) { pc = -pc; }

    if (pa <= pb && pa <= pc) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

static int png_channels_of(int color)
{
    switch (color) {
    case 0: return 1;  /* gray */
    case 2: return 3;  /* RGB */
    case 3: return 1;  /* palette (index) */
    case 4: return 2;  /* gray + alpha */
    case 6: return 4;  /* RGB + alpha */
    default: return 0;
    }
}

/* Unfilters cur in place, against the already-unfiltered prev */
static int png_unfilter_row(png_dec_t *d)
{
    unsigned char *cur = d->cur;
    const unsigned char *prev = d->prev;
    size_t bpp = d->bpp;
    size_t rb = d->row_bytes;
    size_t i;

    switch (cur[0]) {
    case 0:  /* None */
        break;
    case 1:  /* Sub */
        for (i = 1u + bpp; i < rb; i++) {
            cur[i] = (unsigned char)(cur[i] + cur[i - bpp]);
        }
        break;
    case 2:  /* Up */
        for (i = 1u; i < rb; i++) {
            cur[i] = (unsigned char)(cur[i] + prev[i]);
        }
        break;
    case 3:  /* Average */
        for (i = 1u; i < rb; i++) {
            int a = (i > bpp) ? (int)cur[i - bpp] : 0;
            cur[i] = (unsigned char)(cur[i] + ((a + (int)prev[i]) / 2));
        }
        break;
    case 4:  /* Paeth */
        for (i = 1u; i < rb; i++) {
            int a = (i > bpp) ? (int)cur[i - bpp] : 0;
            int b = (int)prev[i];
            int c = (i > bpp) ? (int)prev[i - bpp] : 0;
            cur[i] = (unsigned char)(cur[i] + png_paeth(a, b, c));
        }
        break;
    default:
        return PSPSKY_E_PARSE;
    }
    return PSPSKY_OK;
}

/* Point-samples the unfiltered cur row into thumbnail row ty */
static int png_sample_row(png_dec_t *d, int ty)
{
    const unsigned char *cur = d->cur;
    unsigned char *dst = d->out + (size_t)ty * (size_t)d->thumb_w * 3u;
    int tx;

    for (tx = 0; tx < d->thumb_w; tx++) {
        size_t o = 1u + (size_t)(tx * d->n) * d->bpp;
        unsigned char r;
        unsigned char g;
        unsigned char b;

        switch (d->color) {
        case 0:  /* gray; use the high byte for 16-bit samples */
        case 4:  /* gray + alpha (alpha ignored) */
            r = cur[o];
            g = r;
            b = r;
            break;
        case 2:  /* RGB */
        case 6:  /* RGB + alpha (alpha ignored) */
            if (d->depth == 16) {
                r = cur[o];
                g = cur[o + 2u];
                b = cur[o + 4u];
            } else {
                r = cur[o];
                g = cur[o + 1u];
                b = cur[o + 2u];
            }
            break;
        case 3: {  /* palette */
            int idx = (int)cur[o];
            if (idx >= d->plte_entries) {
                return PSPSKY_E_PARSE;
            }
            r = d->plte[idx * 3 + 0];
            g = d->plte[idx * 3 + 1];
            b = d->plte[idx * 3 + 2];
            break;
        }
        default:
            return PSPSKY_E_PARSE;
        }

        dst[tx * 3 + 0] = r;
        dst[tx * 3 + 1] = g;
        dst[tx * 3 + 2] = b;
    }
    return PSPSKY_OK;
}

static int png_row_complete(png_dec_t *d)
{
    unsigned char *tmp;
    int rc = png_unfilter_row(d);

    if (rc != PSPSKY_OK) {
        return rc;
    }
    if ((d->y % d->n) == 0) {
        rc = png_sample_row(d, d->y / d->n);
        if (rc != PSPSKY_OK) {
            return rc;
        }
    }

    tmp = d->prev;
    d->prev = d->cur;
    d->cur = tmp;
    d->filled = 0u;
    d->y++;
    return PSPSKY_OK;
}

/* Feeds one IDAT chunk to inflate, processing every row it completes */
static int png_feed_idat(png_dec_t *d, z_stream *strm,
                         const unsigned char *data, size_t len)
{
    strm->next_in = (const Bytef *)data;
    strm->avail_in = (uInt)len;

    while (d->y < d->h) {
        size_t before = d->filled;
        int zr;

        strm->next_out = (Bytef *)(d->cur + d->filled);
        strm->avail_out = (uInt)(d->row_bytes - d->filled);

        zr = inflate(strm, Z_NO_FLUSH);
        d->filled = d->row_bytes - (size_t)strm->avail_out;

        if (zr != Z_OK && zr != Z_STREAM_END && zr != Z_BUF_ERROR) {
            return (zr == Z_MEM_ERROR) ? PSPSKY_E_NOMEM : PSPSKY_E_PARSE;
        }

        if (d->filled == d->row_bytes) {
            int rc = png_row_complete(d);
            if (rc != PSPSKY_OK) {
                return rc;
            }
            continue;
        }

        if (zr == Z_STREAM_END) {
            return PSPSKY_E_PARSE;
        }
        if (strm->avail_in == 0u) {
            return PSPSKY_OK;  /* wait for the next IDAT chunk */
        }
        if (d->filled == before) {
            return PSPSKY_E_PARSE;  /* reject a stalled stream */
        }
    }
    return PSPSKY_OK;
}

int pspsky_png_thumb_decode(const unsigned char *png, size_t png_len,
                            unsigned char *out_rgb, size_t out_cap,
                            int *out_thumb_w, int *out_thumb_h,
                            int *out_src_w, int *out_src_h)
{
    png_dec_t dec;
    z_stream strm;
    size_t ofs;
    size_t maxdim;
    size_t nn;
    int rc;
    int compression;
    int filter_method;
    int interlace;
    int channels;

    if (out_thumb_w != NULL) { *out_thumb_w = 0; }
    if (out_thumb_h != NULL) { *out_thumb_h = 0; }
    if (out_src_w != NULL)   { *out_src_w = 0; }
    if (out_src_h != NULL)   { *out_src_h = 0; }

    if (png == NULL || out_rgb == NULL || png_len == 0u ||
        out_thumb_w == NULL || out_thumb_h == NULL ||
        out_src_w == NULL || out_src_h == NULL) {
        return PSPSKY_E_ARG;
    }

    memset(&dec, 0, sizeof dec);
    dec.out = out_rgb;

    if (pspsky_png_dimensions(png, png_len, &dec.w, &dec.h) != PSPSKY_OK) {
        return PSPSKY_E_PARSE;
    }
    /* IHDR data runs to offset 28 */
    if (png_len < 29u) {
        return PSPSKY_E_PARSE;
    }
    dec.depth     = (int)png[24];
    dec.color     = (int)png[25];
    compression   = (int)png[26];
    filter_method = (int)png[27];
    interlace     = (int)png[28];

    if (compression != 0 || filter_method != 0 || interlace != 0) {
        return PSPSKY_E_PARSE;
    }

    if (dec.depth == 8) {
        if (dec.color != 0 && dec.color != 2 && dec.color != 3 &&
            dec.color != 4 && dec.color != 6) {
            return PSPSKY_E_PARSE;
        }
    } else if (dec.depth == 16) {
        if (dec.color != 0 && dec.color != 2 &&
            dec.color != 4 && dec.color != 6) {
            return PSPSKY_E_PARSE;
        }
    } else {
        return PSPSKY_E_PARSE;
    }

    if (dec.w > PSPSKY_PNG_THUMB_MAX_SRC_W) {
        return PSPSKY_E_NOMEM;
    }
    maxdim = (dec.w > dec.h) ? (size_t)dec.w : (size_t)dec.h;
    nn = (maxdim + (size_t)(PSPSKY_PNG_THUMB_MAX_DIM - 1)) /
         (size_t)PSPSKY_PNG_THUMB_MAX_DIM;
    if (nn < 1u) {
        nn = 1u;
    }
    dec.n = (int)nn;
    dec.thumb_w = (int)(((size_t)dec.w + nn - 1u) / nn);
    dec.thumb_h = (int)(((size_t)dec.h + nn - 1u) / nn);
    if ((size_t)dec.thumb_w * (size_t)dec.thumb_h * 3u > out_cap) {
        return PSPSKY_E_NOMEM;
    }

    channels = png_channels_of(dec.color);
    dec.bpp = (size_t)channels * (size_t)(dec.depth / 8);
    dec.row_bytes = 1u + (size_t)dec.w * dec.bpp;

    dec.prev = (unsigned char *)malloc(dec.row_bytes);
    dec.cur  = (unsigned char *)malloc(dec.row_bytes);
    if (dec.prev == NULL || dec.cur == NULL) {
        free(dec.prev);
        free(dec.cur);
        return PSPSKY_E_NOMEM;
    }
    /* The row before row 0 is all zeroes, per the PNG specification */
    memset(dec.prev, 0, dec.row_bytes);

    memset(&strm, 0, sizeof strm);
    if (inflateInit(&strm) != Z_OK) {
        free(dec.prev);
        free(dec.cur);
        return PSPSKY_E_NOMEM;
    }

    rc = PSPSKY_OK;
    ofs = 8u;
    while (dec.y < dec.h && ofs < png_len) {
        size_t remain = png_len - ofs;
        const unsigned char *ctype;
        const unsigned char *cdata;
        unsigned long clen;

        /* length 4 + type 4 + CRC 4 */
        if (remain < 12u) {
            rc = PSPSKY_E_PARSE;
            break;
        }
        clen = png_thumb_be32(png + ofs);
        ctype = png + ofs + 4u;
        cdata = png + ofs + 8u;
        if ((size_t)clen > remain - 12u) {
            rc = PSPSKY_E_PARSE;
            break;
        }

        if (memcmp(ctype, "PLTE", 4u) == 0) {
            if (dec.color == 3) {
                if (clen == 0uL || (clen % 3uL) != 0uL || clen > 768uL) {
                    rc = PSPSKY_E_PARSE;
                    break;
                }
                memcpy(dec.plte, cdata, (size_t)clen);
                dec.plte_entries = (int)(clen / 3uL);
                dec.have_plte = 1;
            }
        } else if (memcmp(ctype, "IDAT", 4u) == 0) {
            if (dec.color == 3 && !dec.have_plte) {
                rc = PSPSKY_E_PARSE;
                break;
            }
            rc = png_feed_idat(&dec, &strm, cdata, (size_t)clen);
            if (rc != PSPSKY_OK) {
                break;
            }
        } else if (memcmp(ctype, "IEND", 4u) == 0) {
            break;
        }

        ofs += 12u + (size_t)clen;
    }
    if (rc == PSPSKY_OK && dec.y < dec.h) {
        rc = PSPSKY_E_PARSE;
    }

    inflateEnd(&strm);
    free(dec.prev);
    free(dec.cur);

    if (rc != PSPSKY_OK) {
        *out_thumb_w = 0;
        *out_thumb_h = 0;
        *out_src_w = 0;
        *out_src_h = 0;
        return rc;
    }

    *out_thumb_w = dec.thumb_w;
    *out_thumb_h = dec.thumb_h;
    *out_src_w = dec.w;
    *out_src_h = dec.h;
    return PSPSKY_OK;
}
