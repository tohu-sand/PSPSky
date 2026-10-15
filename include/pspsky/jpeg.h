#ifndef PSPSKY_JPEG_H
#define PSPSKY_JPEG_H

#include <stddef.h>

/*
 * Reads width/height from the SOF0/SOF1/SOF2 segment; no decoding, and EXIF
 * Orientation is ignored
 */
int pspsky_jpeg_dimensions(const unsigned char *data, size_t len,
                           int *out_w, int *out_h);

/* Covers source images up to 4096px at 1/8 reduction */
#define PSPSKY_JPEG_THUMB_MAX_DIM 512
#define PSPSKY_JPEG_THUMB_MAX_RGB \
    (PSPSKY_JPEG_THUMB_MAX_DIM * PSPSKY_JPEG_THUMB_MAX_DIM * 3)

/*
 * Decodes baseline or extended sequential JPEG (not progressive) at 1/8 scale
 * via picojpeg's reduce mode into packed RGB888, stride thumb_w*3. MCU padding
 * at the right and bottom edges is not written; grayscale gives R=G=B.
 * Not reentrant: picojpeg uses one global decoder context.
 */
int pspsky_jpeg_thumb_decode(const unsigned char *jpeg, size_t jpeg_len,
                             unsigned char *out_rgb, size_t out_cap,
                             int *out_thumb_w, int *out_thumb_h,
                             int *out_src_w, int *out_src_h);

#endif /* PSPSKY_JPEG_H */
