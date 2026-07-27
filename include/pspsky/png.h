/* Image MIME sniffing, PNG dimensions, and streaming PNG thumbnail decoding.
   Portable C99; the thumbnail decoder additionally needs zlib. */
#ifndef PSPSKY_PNG_H
#define PSPSKY_PNG_H

#include <stddef.h>

/* Returns a static "image/png" / "image/jpeg" literal, or NULL. Content only:
   the file extension is never consulted. */
const char *pspsky_image_sniff_mime(const unsigned char *data, size_t len);

/* Reads width/height from the leading IHDR chunk; no decoding */
int pspsky_png_dimensions(const unsigned char *data, size_t len,
                          int *out_w, int *out_h);

/* Must equal PSPSKY_JPEG_THUMB_MAX_DIM: main.c reuses one output buffer for
   both decoders */
#define PSPSKY_PNG_THUMB_MAX_DIM 512
#define PSPSKY_PNG_THUMB_MAX_RGB \
    (PSPSKY_PNG_THUMB_MAX_DIM * PSPSKY_PNG_THUMB_MAX_DIM * 3)
/* A row costs up to 8 bytes/px (16-bit RGBA); two rows must stay within ~128KB */
#define PSPSKY_PNG_THUMB_MAX_SRC_W 8192

/* Streams inflate row by row into packed RGB888, stride thumb_w*3, decimating
   by n = ceil(max(src_w, src_h) / 512). Never allocates a full-resolution
   buffer, which is what keeps large PNGs within PSP memory limits.
   Non-interlaced, depth 8 or 16; alpha and tRNS ignored, CRCs unchecked. */
int pspsky_png_thumb_decode(const unsigned char *png, size_t png_len,
                            unsigned char *out_rgb, size_t out_cap,
                            int *out_thumb_w, int *out_thumb_h,
                            int *out_src_w, int *out_src_h);

#endif /* PSPSKY_PNG_H */
