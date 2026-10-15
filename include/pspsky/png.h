#ifndef PSPSKY_PNG_H
#define PSPSKY_PNG_H

#include <stddef.h>

/*
 * Returns a static "image/png" / "image/jpeg" literal, or NULL. Content only:
 * the file extension is never consulted.
 */
const char *pspsky_image_sniff_mime(const unsigned char *data, size_t len);

/* Reads width/height from the leading IHDR chunk; no decoding */
int pspsky_png_dimensions(const unsigned char *data, size_t len,
                          int *out_w, int *out_h);

/* Must match PSPSKY_JPEG_THUMB_MAX_DIM; both decoders share an output buffer. */
#define PSPSKY_PNG_THUMB_MAX_DIM 512
#define PSPSKY_PNG_THUMB_MAX_RGB \
    (PSPSKY_PNG_THUMB_MAX_DIM * PSPSKY_PNG_THUMB_MAX_DIM * 3)
/* At 8 bytes/px (16-bit RGBA), two rows use at most 128 KiB. */
#define PSPSKY_PNG_THUMB_MAX_SRC_W 8192

/*
 * Decode into packed RGB888, stride thumb_w * 3, with at most 512px per side.
 * Uses two row buffers; supports non-interlaced images at depth 8 or 16.
 * Alpha and tRNS are ignored; CRCs are not checked.
 */
int pspsky_png_thumb_decode(const unsigned char *png, size_t png_len,
                            unsigned char *out_rgb, size_t out_cap,
                            int *out_thumb_w, int *out_thumb_h,
                            int *out_src_w, int *out_src_h);

#endif /* PSPSKY_PNG_H */
