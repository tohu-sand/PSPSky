/* WebP thumbnail decoding on top of the vendored libwebp decoder.
 *
 * libwebp rescales while it decodes, so the scaled RGB888 lands straight in the
 * caller's buffer (is_external_memory) and no full-resolution image is ever
 * held; the PSP's memory limits make that mandatory. No globals: reentrant.
 */

#include "pspsky/webp.h"

#include "pspsky/error.h"

#include "webp/decode.h"

#include <stddef.h>
#include <string.h>

/* Sources wider or taller than this are refused before any decoding starts */
#define WEBP_THUMB_MAX_SRC_DIM 4096

/* Maps a libwebp status onto the shared error codes */
static int webp_thumb_map_status(VP8StatusCode st)
{
    if (st == VP8_STATUS_OK) {
        return PSPSKY_OK;
    }
    if (st == VP8_STATUS_OUT_OF_MEMORY) {
        return PSPSKY_E_NOMEM;
    }
    return PSPSKY_E_PARSE;
}

/* Largest box within max_w x max_h that keeps the aspect ratio; integer only */
static void webp_thumb_fit(int src_w, int src_h, int max_w, int max_h,
                           int *out_w, int *out_h)
{
    int w;
    int h;

    if (src_w <= max_w && src_h <= max_h) {
        *out_w = src_w;
        *out_h = src_h;
        return;
    }

    /* Compare src_w/src_h against max_w/max_h by cross-multiplication */
    if (src_w * max_h >= src_h * max_w) {
        w = max_w;                        /* width is the binding constraint */
        h = (src_h * max_w) / src_w;
    } else {
        h = max_h;
        w = (src_w * max_h) / src_h;
    }

    if (w < 1) { w = 1; }
    if (h < 1) { h = 1; }
    if (w > max_w) { w = max_w; }
    if (h > max_h) { h = max_h; }

    *out_w = w;
    *out_h = h;
}

int pspsky_webp_dimensions(const unsigned char *data, size_t len,
                           int *out_w, int *out_h)
{
    int w = 0;
    int h = 0;

    if (out_w != NULL) { *out_w = 0; }
    if (out_h != NULL) { *out_h = 0; }

    if (data == NULL || len == 0u || out_w == NULL || out_h == NULL) {
        return PSPSKY_E_ARG;
    }
    if (!WebPGetInfo(data, len, &w, &h) || w <= 0 || h <= 0) {
        return PSPSKY_E_PARSE;
    }

    *out_w = w;
    *out_h = h;
    return PSPSKY_OK;
}

int pspsky_webp_thumb_decode(const unsigned char *webp, size_t webp_len,
                             int max_w, int max_h,
                             unsigned char *out_rgb, size_t out_cap,
                             int *out_thumb_w, int *out_thumb_h,
                             int *out_src_w, int *out_src_h)
{
    WebPDecoderConfig config;
    VP8StatusCode st;
    size_t need;
    int src_w;
    int src_h;
    int thumb_w;
    int thumb_h;
    int rc;

    if (out_thumb_w != NULL) { *out_thumb_w = 0; }
    if (out_thumb_h != NULL) { *out_thumb_h = 0; }
    if (out_src_w != NULL)   { *out_src_w = 0; }
    if (out_src_h != NULL)   { *out_src_h = 0; }

    if (webp == NULL || out_rgb == NULL || webp_len == 0u ||
        out_thumb_w == NULL || out_thumb_h == NULL ||
        out_src_w == NULL || out_src_h == NULL) {
        return PSPSKY_E_ARG;
    }
    if (max_w < 1 || max_w > PSPSKY_WEBP_THUMB_MAX_DIM ||
        max_h < 1 || max_h > PSPSKY_WEBP_THUMB_MAX_DIM) {
        return PSPSKY_E_ARG;
    }

    memset(&config, 0, sizeof config);
    if (!WebPInitDecoderConfig(&config)) {
        return PSPSKY_E_PARSE; /* header/library ABI mismatch */
    }

    st = WebPGetFeatures(webp, webp_len, &config.input);
    if (st != VP8_STATUS_OK) {
        return webp_thumb_map_status(st);
    }

    src_w = config.input.width;
    src_h = config.input.height;
    if (config.input.has_animation) {
        return PSPSKY_E_PARSE; /* only single still frames are supported */
    }
    if (src_w < 1 || src_h < 1 ||
        src_w > WEBP_THUMB_MAX_SRC_DIM || src_h > WEBP_THUMB_MAX_SRC_DIM) {
        return PSPSKY_E_PARSE;
    }

    webp_thumb_fit(src_w, src_h, max_w, max_h, &thumb_w, &thumb_h);

    need = (size_t)thumb_w * (size_t)thumb_h * 3u;
    if (need > out_cap) {
        return PSPSKY_E_NOMEM;
    }

    if (thumb_w != src_w || thumb_h != src_h) {
        config.options.use_scaling = 1;
        config.options.scaled_width = thumb_w;
        config.options.scaled_height = thumb_h;
    }
    config.output.colorspace = MODE_RGB;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = (uint8_t *)out_rgb;
    config.output.u.RGBA.stride = thumb_w * 3;
    config.output.u.RGBA.size = need;

    st = WebPDecode(webp, webp_len, &config);
    WebPFreeDecBuffer(&config.output);

    rc = webp_thumb_map_status(st);
    if (rc != PSPSKY_OK) {
        return rc;
    }

    *out_thumb_w = thumb_w;
    *out_thumb_h = thumb_h;
    *out_src_w = src_w;
    *out_src_h = src_h;
    return PSPSKY_OK;
}
