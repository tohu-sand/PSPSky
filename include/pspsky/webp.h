/* WebP thumbnail decoding at reduced scale via vendored libwebp. Portable C99. */
#ifndef PSPSKY_WEBP_H
#define PSPSKY_WEBP_H

#include <stddef.h>

/* Reads width/height from the container header; no decoding */
int pspsky_webp_dimensions(const unsigned char *data, size_t len,
                           int *out_w, int *out_h);

/* Upper bound for the max_w/max_h arguments below */
#define PSPSKY_WEBP_THUMB_MAX_DIM 512

/* Decodes a static WebP scaled to fit max_w x max_h (aspect preserved, never
   upscaled) into packed RGB888, stride thumb_w*3. Alpha is dropped (MODE_RGB).
   Animated input and sources over 4096px per side fail with E_PARSE. */
int pspsky_webp_thumb_decode(const unsigned char *webp, size_t webp_len,
                             int max_w, int max_h,
                             unsigned char *out_rgb, size_t out_cap,
                             int *out_thumb_w, int *out_thumb_h,
                             int *out_src_w, int *out_src_h);

#endif /* PSPSKY_WEBP_H */
