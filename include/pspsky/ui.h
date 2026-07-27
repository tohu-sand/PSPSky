/* PSP user interface: timeline, dialogs, file browser, image preview.
   PSP-only; built on sceGu and intraFont. */
#ifndef PSPSKY_UI_H
#define PSPSKY_UI_H

#include <stddef.h>

#include "pspsky/atproto.h"

/* Action the timeline screen requests from main */
typedef enum {
    PSPSKY_UI_ACT_NONE = 0,
    PSPSKY_UI_ACT_LOAD_OLDER, /* R, or Down at the bottom */
    PSPSKY_UI_ACT_REFRESH,    /* L */
    PSPSKY_UI_ACT_COMPOSE,    /* Triangle */
    PSPSKY_UI_ACT_QUIT        /* START */
} pspsky_ui_action_t;

typedef enum {
    PSPSKY_UI_COMPOSE_CANCEL          = 0, /* Cross */
    PSPSKY_UI_COMPOSE_POST            = 1, /* Circle */
    PSPSKY_UI_COMPOSE_POST_WITH_IMAGE = 2  /* Square */
} pspsky_ui_compose_choice_t;

#define PSPSKY_UI_PICK_PATH_MAX 512

/* Read-only view of the timeline model owned by main */
typedef struct {
    const pspsky_post_t *posts;
    int count;
    int has_more;               /* 1 = a cursor exists */
    long long now_unix;         /* reference for relative times */
    const char *my_handle;      /* shown in the header; may be NULL */
} pspsky_ui_timeline_view_t;

/* GU init plus intraFont (flash0:/font/jpn0.pgf, falling back to ltn0.pgf) */
int  pspsky_ui_init(void);
void pspsky_ui_term(void);

/* Draws one status frame, flips, and returns immediately; call it before a
   blocking network operation */
void pspsky_ui_status(const char *msg_utf8);

/* Blocks until Circle */
void pspsky_ui_message(const char *msg_utf8);

/* Blocks and returns 1 for Circle, 0 for Cross */
int  pspsky_ui_confirm(const char *msg_utf8);

/* Word-wraps the body and blocks until post / post with image / cancel */
pspsky_ui_compose_choice_t pspsky_ui_compose_confirm(const char *msg_utf8);

/* Blocking browser listing directories and .jpg/.jpeg/.png only, rooted at
   "ms0:/". Requires pspsky_ui_init(); never touches the network.
   Still named pick_jpeg for compatibility, though PNG is accepted too. */
int pspsky_ui_pick_jpeg(const char *start_dir, char *out_path, size_t out_cap);

typedef struct {
    const char *path;
    size_t      file_bytes;
    int         src_w;        /* 0 if unknown */
    int         src_h;        /* 0 if unknown */
    const unsigned char *rgb; /* packed RGB888, stride thumb_w*3; NULL = decode failed */
    int         thumb_w;      /* 1..512 when rgb is non-NULL */
    int         thumb_h;
} pspsky_ui_preview_t;

/* Blocking preview; uploads rgb as a GU_PSM_8888 texture, or shows file info
   alone when rgb is NULL. Returns 1 to confirm, 0 to reselect.
   Requires pspsky_ui_init(); never touches the network. */
int pspsky_ui_preview_image(const pspsky_ui_preview_t *pv);

/* Draws one frame and returns the edge-detected action; scroll state is
   internal */
pspsky_ui_action_t pspsky_ui_timeline_frame(const pspsky_ui_timeline_view_t *view);

/* Call after main appends to or clears the posts array */
void pspsky_ui_timeline_invalidate(int reset_scroll);

#endif /* PSPSKY_UI_H */
