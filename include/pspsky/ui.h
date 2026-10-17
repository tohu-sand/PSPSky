#ifndef PSPSKY_UI_H
#define PSPSKY_UI_H

#include <stddef.h>

#include "pspsky/atproto.h"

typedef enum {
    PSPSKY_UI_ACT_NONE = 0,
    PSPSKY_UI_ACT_LOAD_OLDER,  /* R, or Down at the bottom */
    PSPSKY_UI_ACT_REFRESH,  /* L */
    PSPSKY_UI_ACT_COMPOSE,  /* Triangle */
    PSPSKY_UI_ACT_QUIT  /* START */
} pspsky_ui_action_t;

typedef enum {
    PSPSKY_UI_COMPOSE_CANCEL          = 0,  /* Cross */
    PSPSKY_UI_COMPOSE_POST            = 1,  /* Circle */
    PSPSKY_UI_COMPOSE_POST_WITH_IMAGE = 2,  /* Square: add an image */
    PSPSKY_UI_COMPOSE_REMOVE_IMAGE    = 3  /* Triangle: remove the last image */
} pspsky_ui_compose_choice_t;

#define PSPSKY_UI_PICK_PATH_MAX 512

/*
 * One-row layouts (one or two images) may use the full height; three or four
 * images use two rows of at most GRID_ROW_H. Each row is as tall as its
 * tallest image.
 */
#define PSPSKY_UI_TL_IMG_MAX_W  418  /* timeline content width */
#define PSPSKY_UI_TL_IMG_MAX_H  200
#define PSPSKY_UI_TL_GRID_COL_W 207
#define PSPSKY_UI_TL_GRID_ROW_H 120
#define PSPSKY_UI_TL_GRID_GAP   4

#define PSPSKY_UI_AVATAR_PX    32

/*
 * Main-owned thumbnails, indexed by post and image. tex is drawn directly by
 * the GE, so free it only after pspsky_ui_timeline_invalidate().
 */
typedef struct {
    const unsigned short *tex;  /* from pspsky_ui_tl_image_make(); NULL = marker only */
    short w;  /* within pspsky_ui_tl_image_max() when tex is set */
    short h;
} pspsky_ui_tl_image_t;

/* Decode bounds for each image of a post with count images. */
void   pspsky_ui_tl_image_max(int count, int *max_w, int *max_h);

/* Bytes that pspsky_ui_tl_image_make() allocates for a w x h image. */
size_t pspsky_ui_tl_image_bytes(int w, int h);

/*
 * Converts packed RGB888 (stride w * 3) into an RGB565 texture. The caller
 * releases out->tex with free(). Returns PSPSKY_OK, PSPSKY_E_ARG or
 * PSPSKY_E_NOMEM.
 */
int    pspsky_ui_tl_image_make(pspsky_ui_tl_image_t *out,
                               const unsigned char *rgb, int w, int h);

/*
 * Main-owned avatars parallel the posts array. Posts by the same author may
 * share an RGB buffer.
 */
typedef struct {
    /* Packed RGB888, PSPSKY_UI_AVATAR_PX square; NULL = default avatar. */
    const unsigned char *rgb;
} pspsky_ui_avatar_t;

/* Read-only view of the main-owned timeline. */
typedef struct {
    const pspsky_post_t *posts;
    int count;
    int has_more;  /* 1 = a cursor exists */
    long long now_unix;  /* reference for relative times */
    const char *my_handle;  /* shown in the header; may be NULL */
    const pspsky_ui_tl_image_t (*images)[PSPSKY_MAX_IMAGES];  /* NULL = no thumbnails */
    const pspsky_ui_avatar_t   *avatars;  /* parallel to posts; NULL = default avatars */
} pspsky_ui_timeline_view_t;

/* Loads jpn0.pgf from flash0:/font/, falling back to ltn0.pgf. */
int  pspsky_ui_init(void);
void pspsky_ui_term(void);

/* Draws one frame and returns; call before blocking network operations. */
void pspsky_ui_status(const char *msg_utf8);

/* Blocks until Circle is pressed. */
void pspsky_ui_message(const char *msg_utf8);

/* Blocks; returns 1 for Circle, 0 for Cross. */
int  pspsky_ui_confirm(const char *msg_utf8);

/* Blocks until a compose action is selected; shows up to PSPSKY_MAX_IMAGES. */
pspsky_ui_compose_choice_t pspsky_ui_compose_confirm(
    const char *msg_utf8, const char *const *image_paths, int image_count);

/*
 * Blocking browser for directories and JPEG/PNG files, rooted at "ms0:/".
 * Requires pspsky_ui_init(). Returns PSPSKY_OK, PSPSKY_E_CANCELLED or an error.
 */
int pspsky_ui_pick_jpeg(const char *start_dir, char *out_path, size_t out_cap);

typedef struct {
    const char *path;
    size_t      file_bytes;
    int         src_w;  /* 0 if unknown */
    int         src_h;  /* 0 if unknown */
    const unsigned char *rgb;  /* packed RGB888, stride thumb_w * 3; NULL = no image */
    int         thumb_w;  /* 1..512 when rgb is non-NULL */
    int         thumb_h;
} pspsky_ui_preview_t;

/*
 * Blocking preview; shows only file info when rgb is NULL.
 * Requires pspsky_ui_init(). Returns 1 to confirm, 0 to reselect.
 */
int pspsky_ui_preview_image(const pspsky_ui_preview_t *pv);

/* Draws one frame and returns the edge-detected action; retains scroll state. */
pspsky_ui_action_t pspsky_ui_timeline_frame(const pspsky_ui_timeline_view_t *view);

/*
 * Invalidate when the posts array changes, before freeing image/avatar buffers.
 * Clears cached layout and textures; reset_scroll also resets the scroll offset.
 */
void pspsky_ui_timeline_invalidate(int reset_scroll);

#endif  /* PSPSKY_UI_H */
