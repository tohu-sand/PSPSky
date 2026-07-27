/* JPEG 1/8-scale thumbnail decoding via picojpeg's reduce mode, where each 8x8
 * block yields one pixel from its DC coefficient.
 *
 * NOT REENTRANT: picojpeg keeps a single global context, so only one decode may
 * be in flight at a time. PSPSky calls it from a single thread.
 */

#include "pspsky/jpeg.h"

#include "pspsky/error.h"

#include "picojpeg/picojpeg.h"

#include <stddef.h>
#include <string.h>

/* Input stream handed to picojpeg's read callback */
typedef struct {
    const unsigned char *p;
    size_t len;
    size_t ofs;             /* next byte to return */
} thumb_src_t;

/* Refills picojpeg's buffer. Reporting a read error at 0 bytes remaining is
   what stops truncated data from looping forever. */
static unsigned char thumb_need_bytes(unsigned char *pBuf,
                                      unsigned char buf_size,
                                      unsigned char *pBytes_actually_read,
                                      void *pCallback_data)
{
    thumb_src_t *ctx = (thumb_src_t *)pCallback_data;
    size_t remain;
    size_t n;

    if (ctx == NULL || pBuf == NULL || pBytes_actually_read == NULL) {
        return PJPG_STREAM_READ_ERROR;
    }

    remain = ctx->len - ctx->ofs;
    if (remain == 0) {
        return PJPG_STREAM_READ_ERROR; /* not enough data */
    }

    n = (size_t)buf_size;
    if (n > remain) {
        n = remain;
    }

    memcpy(pBuf, ctx->p + ctx->ofs, n);
    ctx->ofs += n;
    *pBytes_actually_read = (unsigned char)n;
    return 0;
}

/* Pointers already validated non-NULL */
static void thumb_zero_out(int *out_thumb_w, int *out_thumb_h,
                           int *out_src_w, int *out_src_h)
{
    *out_thumb_w = 0;
    *out_thumb_h = 0;
    *out_src_w = 0;
    *out_src_h = 0;
}

int pspsky_jpeg_thumb_decode(const unsigned char *jpeg, size_t jpeg_len,
                             unsigned char *out_rgb, size_t out_cap,
                             int *out_thumb_w, int *out_thumb_h,
                             int *out_src_w, int *out_src_h)
{
    pjpeg_image_info_t info;
    thumb_src_t ctx;
    unsigned char status;
    int thumb_w;
    int thumb_h;
    int row_blocks; /* horizontal blocks per MCU (1 or 2) */
    int col_blocks; /* vertical blocks per MCU (1 or 2) */
    int mcu_x = 0;
    int mcu_y = 0;

    /* 1. Validate arguments */
    if (out_thumb_w != NULL) {
        *out_thumb_w = 0;
    }
    if (out_thumb_h != NULL) {
        *out_thumb_h = 0;
    }
    if (out_src_w != NULL) {
        *out_src_w = 0;
    }
    if (out_src_h != NULL) {
        *out_src_h = 0;
    }

    if (jpeg == NULL || out_rgb == NULL || out_thumb_w == NULL ||
        out_thumb_h == NULL || out_src_w == NULL || out_src_h == NULL ||
        jpeg_len == 0) {
        return PSPSKY_E_ARG;
    }

    /* 2. Init with reduce=1; progressive JPEGs are rejected here */
    ctx.p = jpeg;
    ctx.len = jpeg_len;
    ctx.ofs = 0;

    memset(&info, 0, sizeof info);
    status = pjpeg_decode_init(&info, thumb_need_bytes, &ctx, 1);
    if (status != 0) {
        return PSPSKY_E_PARSE;
    }
    if (info.m_width <= 0 || info.m_height <= 0) {
        return PSPSKY_E_PARSE;
    }

    /* 3. Thumbnail dimensions and output capacity */
    thumb_w = (info.m_width + 7) / 8;
    thumb_h = (info.m_height + 7) / 8;
    if (thumb_w > PSPSKY_JPEG_THUMB_MAX_DIM ||
        thumb_h > PSPSKY_JPEG_THUMB_MAX_DIM) {
        return PSPSKY_E_NOMEM;
    }
    if ((size_t)thumb_w * (size_t)thumb_h * 3u > out_cap) {
        return PSPSKY_E_NOMEM;
    }

    row_blocks = info.m_MCUWidth >> 3;
    col_blocks = info.m_MCUHeight >> 3;

    /* 4. MCU loop, row by row from the top left */
    for (;;) {
        int bx;
        int by;

        status = pjpeg_decode_mcu();
        if (status != 0) {
            if (status != PJPG_NO_MORE_BLOCKS) {
                thumb_zero_out(out_thumb_w, out_thumb_h, out_src_w, out_src_h);
                return PSPSKY_E_PARSE;
            }
            break;
        }

        if (mcu_y >= info.m_MCUSPerCol) {
            /* More MCUs than the decoder declared: corrupt data */
            thumb_zero_out(out_thumb_w, out_thumb_h, out_src_w, out_src_h);
            return PSPSKY_E_PARSE;
        }

        /* 5. In reduce mode only each block's first pixel is valid, and block
              (bx, by) lives at byte offset by*128 + bx*64 */
        for (by = 0; by < col_blocks; by++) {
            for (bx = 0; bx < row_blocks; bx++) {
                size_t src_ofs = (size_t)by * 128u + (size_t)bx * 64u;
                int tx = mcu_x * row_blocks + bx;
                int ty = mcu_y * col_blocks + by;
                unsigned char *dst;

                /* 6. Drop MCU padding */
                if (tx >= thumb_w || ty >= thumb_h) {
                    continue;
                }

                dst = out_rgb + ((size_t)ty * (size_t)thumb_w + (size_t)tx) * 3u;

                if (info.m_scanType == PJPG_GRAYSCALE) {
                    /* Grayscale MCUs are 8x8, one pixel; R=G=B */
                    dst[0] = info.m_pMCUBufR[0];
                    dst[1] = info.m_pMCUBufR[0];
                    dst[2] = info.m_pMCUBufR[0];
                } else {
                    dst[0] = info.m_pMCUBufR[src_ofs];
                    dst[1] = info.m_pMCUBufG[src_ofs];
                    dst[2] = info.m_pMCUBufB[src_ofs];
                }
            }
        }

        mcu_x++;
        if (mcu_x == info.m_MCUSPerRow) {
            mcu_x = 0;
            mcu_y++;
        }
    }

    /* If not every MCU was consumed, parts of the output are unwritten: corrupt data */
    if (mcu_y < info.m_MCUSPerCol) {
        thumb_zero_out(out_thumb_w, out_thumb_h, out_src_w, out_src_h);
        return PSPSKY_E_PARSE;
    }

    /* 7. Success */
    *out_thumb_w = thumb_w;
    *out_thumb_h = thumb_h;
    *out_src_w = info.m_width;
    *out_src_h = info.m_height;
    return PSPSKY_OK;
}
