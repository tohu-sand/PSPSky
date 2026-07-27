/*
 * PSPSky - UI on sceGu + intraFont. Drawing and pad input only, no networking.
 *
 * intraFont constraints: it returns with GU_DEPTH_TEST enabled, so the depth
 * func must be GU_ALWAYS; it skips rebinding its texture while
 * INTRAFONT_ACTIVE, so call intraFontActivate after binding your own; and
 * sprites drawn after it need 32BITF vertices.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <psputils.h>   /* sceKernelDcacheWritebackRange */

#include <intraFont.h>

#include "pspsky/error.h"
#include "pspsky/strings.h"
#include "pspsky/timeutil.h"
#include "pspsky/ui.h"
#include "pspsky/utf8.h"

/* ------------------------------------------------------------------ */
/* Screen and layout constants                                         */
/* ------------------------------------------------------------------ */

#define UI_SCR_W        480
#define UI_SCR_H        272
#define UI_BUF_W        512
#define UI_FRAME_SIZE   (UI_BUF_W * UI_SCR_H * 4)   /* GU_PSM_8888 */

#define UI_HEADER_H     20                          /* header: y=0..19    */
#define UI_LIST_TOP     20                          /* list:   y=20..251  */
#define UI_LIST_BOTTOM  252
#define UI_LIST_H       (UI_LIST_BOTTOM - UI_LIST_TOP)
#define UI_FOOTER_Y     252                         /* footer: y=252..271 */
#define UI_FOOTER_H     (UI_SCR_H - UI_FOOTER_Y)

#define UI_MARGIN_X     10
#define UI_BODY_W       460.0f                      /* body wrap width    */
#define UI_LINE_H       16
#define UI_BASELINE     12                          /* line top to baseline */
/* Timestamp gets its own line: a long handle would overlap it in the header. */
#define UI_TIME_LINE_H  13                          /* timestamp line height */
#define UI_TIME_BASELINE 10                         /* timestamp top to baseline */
#define UI_POST_PAD     6                           /* bottom padding + separator */
#define UI_TL_IMG_GAP   4                           /* body to thumbnail gap */
#define UI_SCROLL_STEP  8                           /* scroll amount per frame */
#define UI_ANALOG_DEAD  48

#define UI_SIZE_HEADER  0.70f
#define UI_SIZE_NAME    0.75f
#define UI_SIZE_BODY    0.80f
#define UI_SIZE_FOOTER  0.60f
#define UI_SIZE_MSG     0.80f

/* GU colours are 0xAABBGGRR */
#define UI_COL_BG       0xFF201510
#define UI_COL_BAR      0xFF3A2A20
#define UI_COL_TEXT     0xFFF0F0F0
#define UI_COL_NAME     0xFFE8E8FF
#define UI_COL_DIM      0xFF9C9C9C
#define UI_COL_SEP      0xFF4A3A30
#define UI_COL_TRACK    0xFF3A2A20
#define UI_COL_KNOB     0xFF9C8C80
#define UI_COL_BOX      0xE0301F18
/* The cursor row needs bar + dark text + ">" to be visible on the PSP LCD. */
#define UI_COL_SEL      0xFFF0A050   /* cursor row bar (bright sky blue)     */
#define UI_COL_SEL_TEXT 0xFF201510   /* cursor row text (dark, matches bg)   */

/* Internal limits */
#define UI_MAX_LAYOUT_POSTS 128   /* main caps the timeline at 100 posts */
#define UI_WRAP_MAX_LINES   64
/* Post header buffers. ui_fit_head_calc() and ui_fit_head_apply() must be
   handed the same cap for a given string, so the sizes are named here rather
   than spelled out at each call site. */
#define UI_NAME_BUF         (PSPSKY_MAX_HANDLE + 8)    /* fitted name        */
#define UI_HSRC_BUF         (PSPSKY_MAX_HANDLE + 8)    /* " @handle" source  */
#define UI_HANDLE_BUF       (PSPSKY_MAX_HANDLE + 16)   /* fitted " @handle"  */
#define UI_MSG_W            400.0f
#define UI_GU_LIST_WORDS    32768 /* 128KB: intraFont allocates via sceGuGetMemory */

/* File browser */
#define UI_PICK_MAX_ENTRIES  200
#define UI_PICK_NAME_MAX     128
#define UI_PICK_ROWS         (UI_LIST_H / UI_LINE_H)  /* 14 rows */
#define UI_PICK_REPEAT_DELAY 20   /* frames held before repeat starts */
#define UI_PICK_REPEAT_RATE  4    /* frames between repeats */

/* Timeline thumbnail textures: 4 slots of 256x128 (power of two), 512KB */
#define UI_TL_TEX_SLOTS   4
#define UI_TL_TEX_W       256
#define UI_TL_TEX_H       128

/* Image preview screen */
#define UI_PREV_MAX_DIM   512     /* max texture side (power of two)    */
#define UI_PREV_MIN_DIM   16      /* min texture side (power of two)    */
#define UI_PREV_AREA_Y    24      /* top of the image area              */
#define UI_PREV_AREA_W    440     /* max width of the image area        */
#define UI_PREV_AREA_H    164     /* max height of the image area       */
#define UI_PREV_INFO_Y1   196     /* top of info line 1 (path)          */
#define UI_PREV_INFO_Y2   212     /* top of info line 2 (size, dims)    */
#define UI_PREV_INFO_W    460.0f  /* max width of an info line          */

/* Footer and hint texts come from the language table. */
#define UI_FOOTER_TEXT      pspsky_tr(PSPSKY_STR_UI_FOOTER_TIMELINE)
#define UI_PICK_FOOTER_TEXT pspsky_tr(PSPSKY_STR_UI_FOOTER_PICK)
#define UI_COMPOSE_HINT     pspsky_tr(PSPSKY_STR_UI_HINT_COMPOSE)
#define UI_EMBED_MARK       pspsky_tr(PSPSKY_STR_UI_EMBED_MARK)
#define UI_PICK_CURSOR      ">"
#define UI_PREV_TITLE       pspsky_tr(PSPSKY_STR_UI_PREV_TITLE)
#define UI_PREV_FOOTER_TEXT pspsky_tr(PSPSKY_STR_UI_FOOTER_PREV)
#define UI_PREV_NO_IMAGE    pspsky_tr(PSPSKY_STR_UI_PREV_NO_IMAGE)
#define UI_PREV_NO_IMAGE_SUB pspsky_tr(PSPSKY_STR_UI_PREV_NO_IMAGE_SUB)
#define UI_IMG_MARK_N_FMT   pspsky_tr(PSPSKY_STR_UI_IMG_MARK_N)

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

static unsigned int __attribute__((aligned(16))) s_gu_list[UI_GU_LIST_WORDS];

static intraFont   *s_font        = NULL;
static int          s_inited      = 0;

static unsigned int s_prev_buttons = 0;

static int          s_scroll       = 0;   /* scroll offset in pixels */
static int          s_layout_valid = 0;
static int          s_layout_count = 0;
static int          s_content_h    = 0;
static int          s_post_top[UI_MAX_LAYOUT_POSTS + 1];

/* Wrap result (byte offset and length); shared, not reentrant. */
static int  s_wrap_start[UI_WRAP_MAX_LINES];
static int  s_wrap_len[UI_WRAP_MAX_LINES];
static char s_line_buf[PSPSKY_MAX_TEXT + 1];

/* Per-post layout cache, filled by ui_rebuild_layout() and read by
   ui_draw_post(). It exists so the timeline draw path never calls
   intraFontMeasureText: on hardware, re-wrapping every visible post one code
   point at a time on every frame is what made deep scrolling crawl. Valid
   exactly while s_layout_valid is set, for the first s_layout_count posts, so
   it is invalidated by pspsky_ui_timeline_invalidate() along with s_post_top.
   Same semantics as s_wrap_start/s_wrap_len: byte offset into post->text and
   byte length, both well under a short (PSPSKY_MAX_TEXT is 1024). 32KB. */
static short s_pw_start[UI_MAX_LAYOUT_POSTS][UI_WRAP_MAX_LINES];
static short s_pw_len[UI_MAX_LAYOUT_POSTS][UI_WRAP_MAX_LINES];

/* A ui_fit_head() result kept as "copy this many bytes of the source, then
   maybe an ellipsis" instead of as the fitted string. Rebuilding from it is a
   memcpy, and it costs 4 bytes per string instead of a 264-byte buffer. */
typedef struct {
    short cut;   /* bytes of the source kept */
    short ell;   /* 1 = the ellipsis follows */
} ui_fit_cache_t;

typedef struct {
    ui_fit_cache_t name;       /* display name (or handle), truncated  */
    ui_fit_cache_t handle;     /* " @handle", truncated                */
    short          lines;      /* wrapped body line count              */
    short          has_handle; /* 1 = the @handle is drawn at all      */
    float          handle_x;   /* pen x for the @handle                */
} ui_post_cache_t;

static ui_post_cache_t s_post_cache[UI_MAX_LAYOUT_POSTS];

/* About 26KB, so kept off the stack. */
typedef struct {
    char name[UI_PICK_NAME_MAX];
    int  is_dir;
} ui_pick_entry_t;

static ui_pick_entry_t s_pick[UI_PICK_MAX_ENTRIES];
static int             s_pick_count  = 0;
static int             s_pick_parent = 0;  /* 1 = entry 0 is a synthetic ".." */
static char            s_pick_cur[PSPSKY_UI_PICK_PATH_MAX];
static char            s_pick_path[PSPSKY_UI_PICK_PATH_MAX]; /* build scratch */

/* Preview texture, 512x512 GU_PSM_8888 = 1MB; valid region is the top-left. */
static unsigned int __attribute__((aligned(64)))
    s_prev_tex[UI_PREV_MAX_DIM * UI_PREV_MAX_DIM];

/* Timeline thumbnail textures, 512KB, filled on a slot miss only. s_tl_tex_post
   maps a slot to the post index it holds (-1 = empty); those indices and the
   rgb buffers behind them belong to main, so every slot is dropped by
   pspsky_ui_timeline_invalidate() before main is free to release them. */
static unsigned int __attribute__((aligned(16)))
    s_tl_tex[UI_TL_TEX_SLOTS][UI_TL_TEX_W * UI_TL_TEX_H];
static int s_tl_tex_post[UI_TL_TEX_SLOTS] = { -1, -1, -1, -1 };
static int s_tl_tex_next = 0;   /* round robin */

typedef struct {
    unsigned int color;
    short x, y, z;
} ui_vertex_t;

/* Textured sprite vertex; UVs are texels. Float, see ui_prev_draw_image(). */
typedef struct {
    float u, v;
    float x, y, z;
} ui_tex_vertex_f_t;

/* ------------------------------------------------------------------ */
/* Drawing primitives                                                  */
/* ------------------------------------------------------------------ */

static void ui_draw_rect(int x, int y, int w, int h, unsigned int color)
{
    ui_vertex_t *v;

    if (w <= 0 || h <= 0) {
        return;
    }
    v = (ui_vertex_t *)sceGuGetMemory(2 * sizeof(ui_vertex_t));
    if (!v) {
        return;
    }
    v[0].color = color;
    v[0].x = (short)x;
    v[0].y = (short)y;
    v[0].z = 0;
    v[1].color = color;
    v[1].x = (short)(x + w);
    v[1].y = (short)(y + h);
    v[1].z = 0;

    /* intraFontPrint returns with GU_DEPTH_TEST enabled. */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

static float ui_measure(float size, const char *s)
{
    if (!s_font || !s || !*s) {
        return 0.0f;
    }
    intraFontSetStyle(s_font, size, UI_COL_TEXT, 0, 0.0f, 0);
    return intraFontMeasureText(s_font, s);
}

static float ui_text(float size, unsigned int color, float x, float y, const char *s)
{
    if (!s_font || !s || !*s) {
        return x;
    }
    intraFontSetStyle(s_font, size, color, 0, 0.0f, 0);
    return intraFontPrint(s_font, x, y, s);
}

static void ui_frame_begin(void)
{
    sceGuStart(GU_DIRECT, s_gu_list);
    sceGuClearColor(UI_COL_BG);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
}

static void ui_frame_end(void)
{
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

/* ------------------------------------------------------------------ */
/* Word wrap (per code point)                                          */
/* ------------------------------------------------------------------ */

static void ui_wrap_emit(int *n, int start, int len)
{
    if (*n < UI_WRAP_MAX_LINES) {
        s_wrap_start[*n] = start;
        s_wrap_len[*n]   = (len > 0) ? len : 0;
        (*n)++;
    }
}

/* Wraps into s_wrap_start[]/s_wrap_len[] and returns the line count. */
static int ui_wrap(const char *text, float size, float max_w)
{
    const char *base = text;
    const char *p    = text;
    int   nlines     = 0;
    int   line_start = 0;
    float line_w     = 0.0f;
    int   sp_char    = -1;   /* offset of the last space seen */
    int   sp_next    = -1;   /* offset just past that space   */
    char  cbuf[8];

    if (!s_font || !text) {
        return 0;
    }
    intraFontSetStyle(s_font, size, UI_COL_TEXT, 0, 0.0f, 0);

    while (nlines < UI_WRAP_MAX_LINES) {
        const char  *cp_start = p;
        unsigned int cp       = pspsky_utf8_next(&p);
        int          cp_bytes;
        float        cw;

        if (cp == 0) {
            if ((int)(p - base) > line_start) {
                ui_wrap_emit(&nlines, line_start, (int)(p - base) - line_start);
            }
            break;
        }
        cp_bytes = (int)(p - cp_start);
        if (cp_bytes <= 0 || cp_bytes >= (int)sizeof(cbuf)) {
            continue;
        }
        if (cp == '\r') {
            continue;
        }
        if (cp == '\n') {
            ui_wrap_emit(&nlines, line_start, (int)(cp_start - base) - line_start);
            line_start = (int)(p - base);
            line_w     = 0.0f;
            sp_char    = -1;
            sp_next    = -1;
            continue;
        }

        memcpy(cbuf, cp_start, (size_t)cp_bytes);
        cbuf[cp_bytes] = '\0';
        cw = intraFontMeasureText(s_font, cbuf);

        if (line_w + cw > max_w && (int)(cp_start - base) > line_start) {
            if (sp_char > line_start) {
                /* Break at the space, dropping the space itself. */
                ui_wrap_emit(&nlines, line_start, sp_char - line_start);
                line_start = sp_next;
            } else {
                ui_wrap_emit(&nlines, line_start, (int)(cp_start - base) - line_start);
                line_start = (int)(cp_start - base);
            }
            p       = base + line_start;
            line_w  = 0.0f;
            sp_char = -1;
            sp_next = -1;
            continue;   /* reprocess this character on the next line */
        }

        line_w += cw;
        if (cp == ' ' || cp == '\t' || cp == 0x3000) {
            sp_char = (int)(cp_start - base);
            sp_next = (int)(p - base);
        }
    }
    return nlines;
}

/* Copies text[start .. start+len) into a NUL-terminated buffer. */
static const char *ui_copy_line(const char *text, int start, int len)
{
    if (len > (int)sizeof(s_line_buf) - 1) {
        len = (int)sizeof(s_line_buf) - 1;
    }
    if (len > 0) {
        memcpy(s_line_buf, text + start, (size_t)len);
    }
    s_line_buf[(len > 0) ? len : 0] = '\0';
    return s_line_buf;
}

/* Copies wrapped line i into a NUL-terminated buffer. */
static const char *ui_wrap_line(const char *text, int i)
{
    return ui_copy_line(text, s_wrap_start[i], s_wrap_len[i]);
}

/* Same, from the per-post cache rather than the shared wrap result. */
static const char *ui_cached_line(const char *text, int idx, int i)
{
    return ui_copy_line(text, s_pw_start[idx][i], s_pw_len[idx][i]);
}

/* ------------------------------------------------------------------ */
/* Head-first truncation                                               */
/* ------------------------------------------------------------------ */

static const char ui_ell[] = "…";   /* sizeof is 4, NUL included */

/* Works out how src has to be cut to fit into max_w px when the tail is
   dropped and an ellipsis appended. This is the measuring half of the old
   ui_fit_head(); it runs at layout time only, and ui_fit_head_apply() rebuilds
   the string from the result without touching the font. */
static void ui_fit_head_calc(const char *src, float size, float max_w,
                             size_t cap, ui_fit_cache_t *out)
{
    const char *p    = src;
    size_t      used = 0;
    float       w    = 0.0f;
    float       avail;
    char        cbuf[8];

    out->cut = 0;
    out->ell = 0;
    if (cap == 0) {
        return;
    }
    if (!s_font || src == NULL || src[0] == '\0' || max_w <= 0.0f) {
        return;
    }

    intraFontSetStyle(s_font, size, UI_COL_TEXT, 0, 0.0f, 0);
    if (strlen(src) < cap && intraFontMeasureText(s_font, src) <= max_w) {
        out->cut = (short)strlen(src);
        return;   /* fits as-is */
    }

    avail = max_w - intraFontMeasureText(s_font, ui_ell);
    if (avail < 0.0f) {
        /* The ellipsis alone does not fit: emit nothing. */
        return;
    }
    for (;;) {
        const char *cp_start = p;
        int         cp_bytes;
        float       cw;

        if (pspsky_utf8_next(&p) == 0) {
            break;
        }
        cp_bytes = (int)(p - cp_start);
        if (cp_bytes <= 0 || cp_bytes >= (int)sizeof cbuf) {
            break;
        }
        if (used + (size_t)cp_bytes + sizeof ui_ell > cap) {
            break;   /* always leave room for the ellipsis and the NUL */
        }
        memcpy(cbuf, cp_start, (size_t)cp_bytes);
        cbuf[cp_bytes] = '\0';
        cw = intraFontMeasureText(s_font, cbuf);
        if (w + cw > avail) {
            break;
        }
        used += (size_t)cp_bytes;
        w    += cw;
    }
    out->cut = (short)used;
    out->ell = (used + sizeof ui_ell <= cap) ? 1 : 0;
}

/* Rebuilds the string ui_fit_head_calc() described. Pure memcpy: no measuring,
   so this is what the per-frame draw path calls. */
static const char *ui_fit_head_apply(const char *src, const ui_fit_cache_t *fc,
                                     char *buf, size_t cap)
{
    size_t used;

    if (buf == NULL || cap == 0) {
        return "";
    }
    buf[0] = '\0';
    used = (fc->cut > 0) ? (size_t)fc->cut : 0;
    if (src == NULL || used >= cap) {
        return buf;   /* used >= cap cannot happen: calc() used the same cap */
    }
    if (used > 0) {
        memcpy(buf, src, used);
    }
    if (fc->ell && used + sizeof ui_ell <= cap) {
        memcpy(buf + used, ui_ell, sizeof ui_ell);   /* copy including the NUL */
    } else {
        buf[used] = '\0';
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* Timeline thumbnails                                                 */
/* ------------------------------------------------------------------ */

/* The thumbnail of post idx, or NULL when it can only be marked as text.
   Sizes are checked here because a slot holds UI_TL_TEX_W x UI_TL_TEX_H at
   most, and both the layout and the draw path branch on this result. */
static const pspsky_ui_tl_image_t *ui_tl_image(const pspsky_ui_timeline_view_t *view,
                                               int idx)
{
    const pspsky_ui_tl_image_t *img;

    if (view->images == NULL) {
        return NULL;
    }
    img = &view->images[idx];
    if (img->rgb == NULL ||
        img->w < 1 || img->w > PSPSKY_UI_TL_IMG_MAX_W ||
        img->h < 1 || img->h > PSPSKY_UI_TL_IMG_MAX_H) {
        return NULL;
    }
    return img;
}

/* Height taken under the body by the thumbnail or the marker line. Shared so
   that ui_layout_post() and ui_draw_post() cannot drift apart. */
static int ui_post_extra_h(const pspsky_post_t *post,
                           const pspsky_ui_tl_image_t *img)
{
    if (img != NULL) {
        int h = UI_TL_IMG_GAP + img->h;

        if (post->img_count >= 2) {
            h += UI_LINE_H;   /* the "[images xN]" line under the thumbnail */
        }
        return h;
    }
    if (post->img_count >= 1) {
        return UI_LINE_H;     /* marker line instead of the thumbnail */
    }
    if (post->has_embed) {
        return UI_LINE_H;     /* embed of some other kind */
    }
    return 0;
}

/* Marker text for a post with img_count >= 1 and no usable thumbnail. */
static const char *ui_img_mark(const pspsky_post_t *post, char *buf, size_t cap)
{
    if (post->img_count < 2) {
        return UI_EMBED_MARK;
    }
    snprintf(buf, cap, UI_IMG_MARK_N_FMT, post->img_count);
    return buf;
}

/* Returns the slot holding post idx, uploading it on a miss. */
static const unsigned int *ui_tl_tex_bind(int idx, const pspsky_ui_tl_image_t *img)
{
    unsigned int *tex;
    int           slot, x, y;

    for (slot = 0; slot < UI_TL_TEX_SLOTS; slot++) {
        if (s_tl_tex_post[slot] == idx) {
            return s_tl_tex[slot];
        }
    }
    slot          = s_tl_tex_next;
    s_tl_tex_next = (s_tl_tex_next + 1) % UI_TL_TEX_SLOTS;
    tex           = s_tl_tex[slot];

    for (y = 0; y < img->h; y++) {
        const unsigned char *src = img->rgb + (size_t)y * (size_t)img->w * 3u;
        unsigned int        *dst = tex + (size_t)y * (size_t)UI_TL_TEX_W;

        for (x = 0; x < img->w; x++) {
            dst[x] = 0xFF000000u |
                     ((unsigned int)src[x * 3 + 2] << 16) |
                     ((unsigned int)src[x * 3 + 1] << 8) |
                     (unsigned int)src[x * 3 + 0];
        }
    }
    sceKernelDcacheWritebackRange(tex,
                                  (unsigned int)UI_TL_TEX_W * 4u *
                                  (unsigned int)img->h);
    s_tl_tex_post[slot] = idx;
    return tex;
}

/* Draws the thumbnail at 1:1; the list scissor clips it. */
static void ui_tl_draw_image(int idx, const pspsky_ui_tl_image_t *img,
                             int x, int y)
{
    const unsigned int *tex;
    ui_tex_vertex_f_t  *v;

    tex = ui_tl_tex_bind(idx, img);
    v   = (ui_tex_vertex_f_t *)sceGuGetMemory(2 * sizeof(ui_tex_vertex_f_t));
    if (!v) {
        return;
    }
    v[0].u = 0.0f;
    v[0].v = 0.0f;
    v[0].x = (float)x;
    v[0].y = (float)y;
    v[0].z = 0.0f;
    v[1].u = (float)img->w;
    v[1].v = (float)img->h;
    v[1].x = (float)(x + img->w);
    v[1].y = (float)(y + img->h);
    v[1].z = 0.0f;

    /* intraFontPrint returns with GU_DEPTH_TEST enabled. */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, UI_TL_TEX_W, UI_TL_TEX_H, UI_TL_TEX_W, tex);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);   /* 1:1, so no filtering needed */
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    /* GU_TCC_RGB takes alpha from the vertex colour; these carry none. */
    sceGuColor(0xFFFFFFFF);
    /* Must be float (32BITF), as in ui_prev_draw_image(). */
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   2, 0, v);

    /* intraFontPrint skips rebinding its texture while INTRAFONT_ACTIVE, so
       later text would sample the thumbnail bound above. */
    if (s_font) {
        intraFontActivate(s_font);
    }
}

/* ------------------------------------------------------------------ */
/* Timeline layout                                                     */
/* ------------------------------------------------------------------ */

/* Wraps the body and fits the header of post idx into the layout cache, so
   that ui_draw_post() needs no measuring, and returns the post height.
   Must stay in step with how ui_draw_post() accumulates h. */
static int ui_layout_post(int idx, const pspsky_post_t *post,
                          const pspsky_ui_tl_image_t *img)
{
    ui_post_cache_t *c = &s_post_cache[idx];
    char             namebuf[UI_NAME_BUF];
    const char      *name;
    float            w, avail;
    int              lines, i, h;

    lines = ui_wrap(post->text, UI_SIZE_BODY, UI_BODY_W);
    if (lines > UI_WRAP_MAX_LINES) {
        lines = UI_WRAP_MAX_LINES;
    }
    for (i = 0; i < lines; i++) {
        s_pw_start[idx][i] = (short)s_wrap_start[i];
        s_pw_len[idx][i]   = (short)s_wrap_len[i];
    }
    c->lines = (short)lines;

    /* Header line: name, then @handle in whatever width is left. */
    name = (post->display_name[0] != '\0') ? post->display_name : post->handle;
    ui_fit_head_calc(name, UI_SIZE_NAME, UI_BODY_W, UI_NAME_BUF, &c->name);
    w = ui_measure(UI_SIZE_NAME,
                   ui_fit_head_apply(name, &c->name, namebuf, UI_NAME_BUF));
    avail = UI_BODY_W - w;

    c->handle_x   = (float)UI_MARGIN_X + w;
    c->has_handle = 0;
    c->handle.cut = 0;
    c->handle.ell = 0;
    if (post->handle[0] != '\0' && avail > 0.0f) {
        char hsrc[UI_HSRC_BUF];

        snprintf(hsrc, sizeof hsrc, " @%s", post->handle);
        ui_fit_head_calc(hsrc, UI_SIZE_NAME, avail, UI_HANDLE_BUF, &c->handle);
        c->has_handle = 1;
    }

    h = UI_LINE_H + UI_TIME_LINE_H + lines * UI_LINE_H + UI_POST_PAD;
    h += ui_post_extra_h(post, img);
    return h;
}

static int ui_clamp_count(int count)
{
    if (count < 0) {
        return 0;
    }
    if (count > UI_MAX_LAYOUT_POSTS) {
        return UI_MAX_LAYOUT_POSTS;
    }
    return count;
}

static void ui_rebuild_layout(const pspsky_ui_timeline_view_t *view)
{
    int n = ui_clamp_count(view->count);
    int y = 0;
    int i;

    for (i = 0; i < n; i++) {
        s_post_top[i] = y;
        y += ui_layout_post(i, &view->posts[i], ui_tl_image(view, i));
    }
    s_post_top[n]  = y;
    s_content_h    = y;
    s_layout_count = n;
    s_layout_valid = 1;
}

static int ui_max_scroll(void)
{
    int m = s_content_h - UI_LIST_H;

    return (m > 0) ? m : 0;
}

/* ------------------------------------------------------------------ */
/* Drawing a single post                                               */
/* ------------------------------------------------------------------ */

/* idx indexes the layout cache built by ui_layout_post(); it must be < the
   s_layout_count that the current s_layout_valid covers. */
static void ui_draw_post(const pspsky_post_t *post, int idx, int top,
                         long long now_unix, const pspsky_ui_tl_image_t *img)
{
    const ui_post_cache_t *c = &s_post_cache[idx];
    char        hsrc[UI_HSRC_BUF];
    char        namebuf[UI_NAME_BUF];     /* display name or handle + ellipsis */
    char        handlebuf[UI_HANDLE_BUF];
    char        markbuf[32];
    char        tbuf[24];
    const char *name;
    int         body_top, lines, i, h, below;

    name = (post->display_name[0] != '\0') ? post->display_name : post->handle;

    /* Header line: name, then @handle in whatever width is left. Both were
       fitted in ui_layout_post(), so rebuilding them here only copies bytes. */
    ui_text(UI_SIZE_NAME, UI_COL_NAME, (float)UI_MARGIN_X,
            (float)(top + UI_BASELINE),
            ui_fit_head_apply(name, &c->name, namebuf, UI_NAME_BUF));

    if (c->has_handle) {
        snprintf(hsrc, sizeof hsrc, " @%s", post->handle);
        ui_text(UI_SIZE_NAME, UI_COL_DIM, c->handle_x,
                (float)(top + UI_BASELINE),
                ui_fit_head_apply(hsrc, &c->handle, handlebuf, UI_HANDLE_BUF));
    }

    /* Timestamp line: right-aligned, dimmed, tighter than a body line. */
    tbuf[0] = '\0';
    if (pspsky_format_relative(post->created_at_unix, now_unix,
                               tbuf, sizeof(tbuf)) != PSPSKY_OK) {
        tbuf[0] = '\0';
    }
    if (tbuf[0] != '\0') {
        float tw = ui_measure(UI_SIZE_FOOTER, tbuf);

        ui_text(UI_SIZE_FOOTER, UI_COL_DIM,
                (float)(UI_SCR_W - UI_MARGIN_X) - tw,
                (float)(top + UI_LINE_H + UI_TIME_BASELINE), tbuf);
    }

    /* Body, wrapped to UI_BODY_W by ui_layout_post(). */
    body_top = top + UI_LINE_H + UI_TIME_LINE_H;
    lines    = c->lines;
    for (i = 0; i < lines; i++) {
        int ly = body_top + i * UI_LINE_H;

        if (ly + UI_LINE_H < UI_LIST_TOP || ly > UI_LIST_BOTTOM) {
            continue;   /* skip lines that are off screen */
        }
        ui_text(UI_SIZE_BODY, UI_COL_TEXT,
                (float)UI_MARGIN_X, (float)(ly + UI_BASELINE),
                ui_cached_line(post->text, idx, i));
    }

    h     = UI_LINE_H + UI_TIME_LINE_H + lines * UI_LINE_H + UI_POST_PAD;
    below = body_top + lines * UI_LINE_H;

    /* Same three cases as ui_post_extra_h(), which supplies the height. */
    if (img != NULL) {
        int iy = below + UI_TL_IMG_GAP;

        if (iy + img->h >= UI_LIST_TOP && iy <= UI_LIST_BOTTOM) {
            ui_tl_draw_image(idx, img, UI_MARGIN_X, iy);
        }
        if (post->img_count >= 2) {
            int my = iy + img->h;

            if (my + UI_LINE_H >= UI_LIST_TOP && my <= UI_LIST_BOTTOM) {
                ui_text(UI_SIZE_NAME, UI_COL_DIM,
                        (float)UI_MARGIN_X, (float)(my + UI_BASELINE),
                        ui_img_mark(post, markbuf, sizeof markbuf));
            }
        }
    } else if (post->img_count >= 1) {
        if (below + UI_LINE_H >= UI_LIST_TOP && below <= UI_LIST_BOTTOM) {
            ui_text(UI_SIZE_NAME, UI_COL_DIM,
                    (float)UI_MARGIN_X, (float)(below + UI_BASELINE),
                    ui_img_mark(post, markbuf, sizeof markbuf));
        }
    } else if (post->has_embed) {
        /* Presence only; the embed contents are never inspected. */
        if (below + UI_LINE_H >= UI_LIST_TOP && below <= UI_LIST_BOTTOM) {
            ui_text(UI_SIZE_NAME, UI_COL_DIM,
                    (float)UI_MARGIN_X, (float)(below + UI_BASELINE),
                    UI_EMBED_MARK);
        }
    }
    h += ui_post_extra_h(post, img);

    /* separator line */
    ui_draw_rect(UI_MARGIN_X, top + h - 3, UI_SCR_W - UI_MARGIN_X * 2, 1, UI_COL_SEP);
}

/* ------------------------------------------------------------------ */
/* Header / footer / scrollbar                                         */
/* ------------------------------------------------------------------ */

static void ui_draw_header(const char *my_handle)
{
    ui_draw_rect(0, 0, UI_SCR_W, UI_HEADER_H, UI_COL_BAR);
    ui_text(UI_SIZE_HEADER, UI_COL_TEXT, 6.0f, 14.0f, "PSPSky");

    if (my_handle && my_handle[0] != '\0') {
        /* Right-aligned, so it needs a width. The handle almost never changes,
           so it is measured once and remembered rather than every frame. */
        static char  cached[PSPSKY_MAX_HANDLE + 8];
        static float cached_w;
        char  buf[PSPSKY_MAX_HANDLE + 8];

        if (my_handle[0] == '@') {
            snprintf(buf, sizeof(buf), "%s", my_handle);
        } else {
            snprintf(buf, sizeof(buf), "@%s", my_handle);
        }
        if (strcmp(buf, cached) != 0) {
            cached_w = ui_measure(UI_SIZE_HEADER, buf);
            memcpy(cached, buf, strlen(buf) + 1);
        }
        ui_text(UI_SIZE_HEADER, UI_COL_DIM,
                (float)UI_SCR_W - 6.0f - cached_w, 14.0f, buf);
    }
}

static void ui_draw_footer(void)
{
    ui_draw_rect(0, UI_FOOTER_Y, UI_SCR_W, UI_FOOTER_H, UI_COL_BAR);
    ui_text(UI_SIZE_FOOTER, UI_COL_TEXT, 6.0f, (float)(UI_FOOTER_Y + 14),
            UI_FOOTER_TEXT);
}

static void ui_draw_scrollbar(void)
{
    int max_scroll = ui_max_scroll();
    int knob_h, knob_y;

    if (max_scroll <= 0) {
        return;
    }
    knob_h = UI_LIST_H * UI_LIST_H / s_content_h;
    if (knob_h < 12) {
        knob_h = 12;
    }
    knob_y = UI_LIST_TOP + (UI_LIST_H - knob_h) * s_scroll / max_scroll;

    ui_draw_rect(UI_SCR_W - 6, UI_LIST_TOP, 4, UI_LIST_H, UI_COL_TRACK);
    ui_draw_rect(UI_SCR_W - 6, knob_y, 4, knob_h, UI_COL_KNOB);
}

/* ------------------------------------------------------------------ */
/* Centred message box                                                 */
/* ------------------------------------------------------------------ */

static void ui_draw_message(const char *msg_utf8, const char *hint_utf8)
{
    const char *msg = (msg_utf8 != NULL) ? msg_utf8 : "";
    int   lines = ui_wrap(msg, UI_SIZE_MSG, UI_MSG_W);
    int   total = lines * UI_LINE_H + ((hint_utf8 != NULL) ? (UI_LINE_H + 8) : 0);
    int   top   = (UI_SCR_H - total) / 2;
    int   i;
    int   box_y = top - 12;
    int   box_h = total + 24;

    if (box_y < 0) {
        box_y = 0;
    }
    if (box_y + box_h > UI_SCR_H) {
        box_h = UI_SCR_H - box_y;
    }
    ui_draw_rect(20, box_y, UI_SCR_W - 40, box_h, UI_COL_BOX);

    for (i = 0; i < lines; i++) {
        const char *line = ui_wrap_line(msg, i);
        float       w    = ui_measure(UI_SIZE_MSG, line);

        ui_text(UI_SIZE_MSG, UI_COL_TEXT,
                ((float)UI_SCR_W - w) * 0.5f,
                (float)(top + i * UI_LINE_H + UI_BASELINE), line);
    }
    if (hint_utf8 != NULL) {
        float w = ui_measure(UI_SIZE_MSG, hint_utf8);

        ui_text(UI_SIZE_MSG, UI_COL_DIM,
                ((float)UI_SCR_W - w) * 0.5f,
                (float)(top + lines * UI_LINE_H + 8 + UI_BASELINE), hint_utf8);
    }
}

/* ------------------------------------------------------------------ */
/* File browser - building the listing                                 */
/* ------------------------------------------------------------------ */

/* Does name end with suffix (ASCII), ignoring case? */
static int ui_pick_has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name);
    size_t m = strlen(suffix);
    size_t i;

    if (n < m) {
        return 0;
    }
    for (i = 0; i < m; i++) {
        char a = name[n - m + i];
        char b = suffix[i];

        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static int ui_pick_is_image(const char *name)
{
    return (ui_pick_has_suffix(name, ".jpg") ||
            ui_pick_has_suffix(name, ".jpeg") ||
            ui_pick_has_suffix(name, ".png"));
}

/* Directories first, then strcmp ascending within each kind. */
static int ui_pick_cmp(const void *a, const void *b)
{
    const ui_pick_entry_t *x = (const ui_pick_entry_t *)a;
    const ui_pick_entry_t *y = (const ui_pick_entry_t *)b;

    if (x->is_dir != y->is_dir) {
        return (y->is_dir - x->is_dir);
    }
    return strcmp(x->name, y->name);
}

/* 1 if s_pick_cur is a drive root such as "ms0:/". */
static int ui_pick_at_root(void)
{
    size_t n = strlen(s_pick_cur);
    size_t i = 0;

    while (i < n && s_pick_cur[i] != '/') {
        i++;
    }
    return (i + 1 >= n);
}

/* Sets the current directory, always with a trailing '/'. */
static int ui_pick_set_dir(const char *src)
{
    size_t n;

    if (src == NULL || src[0] == '\0') {
        return -1;
    }
    n = strlen(src);
    if (n + 2 > sizeof s_pick_cur) {   /* room for trailing '/' and NUL */
        return -1;
    }
    memcpy(s_pick_cur, src, n + 1);
    if (s_pick_cur[n - 1] != '/') {
        s_pick_cur[n]     = '/';
        s_pick_cur[n + 1] = '\0';
    }
    return 0;
}

/* Parent of s_pick_cur into buf, stopping at the root. */
static void ui_pick_parent_path(char *buf, size_t cap)
{
    size_t n    = strlen(s_pick_cur);
    size_t root = 0;

    while (root < n && s_pick_cur[root] != '/') {
        root++;
    }
    if (root < n) {
        root++;   /* keep the '/' immediately after the drive */
    }
    if (n > root && s_pick_cur[n - 1] == '/') {
        n--;
    }
    while (n > root && s_pick_cur[n - 1] != '/') {
        n--;
    }
    if (n < root) {
        n = root;
    }
    if (n + 1 > cap) {
        n = cap - 1;
    }
    memcpy(buf, s_pick_cur, n);
    buf[n] = '\0';
}

/* Joins s_pick_cur + name into buf; non-zero if it would not fit. */
static int ui_pick_join(char *buf, size_t cap, const char *name, int as_dir)
{
    size_t dlen = strlen(s_pick_cur);
    size_t nlen = strlen(name);
    size_t need = dlen + nlen + (as_dir ? 1u : 0u) + 1u;

    if (need > cap || need > (size_t)PSPSKY_UI_PICK_PATH_MAX) {
        return -1;
    }
    memcpy(buf, s_pick_cur, dlen);
    memcpy(buf + dlen, name, nlen);
    if (as_dir) {
        buf[dlen + nlen] = '/';
    }
    buf[need - 1] = '\0';
    return 0;
}

/* Rebuilds s_pick[] from s_pick_cur. */
static int ui_pick_load(void)
{
    SceUID      fd;
    SceIoDirent de;
    int         need_parent = !ui_pick_at_root();
    int         limit       = UI_PICK_MAX_ENTRIES - (need_parent ? 1 : 0);
    int         n           = 0;

    fd = sceIoDopen(s_pick_cur);
    if (fd < 0) {
        return -1;
    }
    while (n < limit) {
        int is_dir;

        memset(&de, 0, sizeof(de));
        if (sceIoDread(fd, &de) <= 0) {
            break;   /* 0 = end of directory, negative = error */
        }
        de.d_name[sizeof(de.d_name) - 1] = '\0';
        if (de.d_name[0] == '\0' ||
            strcmp(de.d_name, ".") == 0 || strcmp(de.d_name, "..") == 0) {
            continue;
        }
        if (strlen(de.d_name) > (size_t)(UI_PICK_NAME_MAX - 1)) {
            continue;   /* skip names that are too long */
        }
        is_dir = ((de.d_stat.st_mode & FIO_S_IFMT) == FIO_S_IFDIR);
        if (!is_dir && !ui_pick_is_image(de.d_name)) {
            continue;
        }
        snprintf(s_pick[n].name, sizeof s_pick[n].name, "%s", de.d_name);
        s_pick[n].is_dir = is_dir;
        n++;
    }
    sceIoDclose(fd);

    if (n > 1) {
        qsort(s_pick, (size_t)n, sizeof s_pick[0], ui_pick_cmp);
    }
    if (need_parent) {
        memmove(&s_pick[1], &s_pick[0], (size_t)n * sizeof s_pick[0]);
        snprintf(s_pick[0].name, sizeof s_pick[0].name, "..");
        s_pick[0].is_dir = 1;
        n++;
    }
    s_pick_count  = n;
    s_pick_parent = need_parent;
    return 0;
}

/* Moves to dir; on failure the current directory is kept. */
static int ui_pick_enter(const char *dir)
{
    static char prev[PSPSKY_UI_PICK_PATH_MAX];

    snprintf(prev, sizeof prev, "%s", s_pick_cur);
    if (ui_pick_set_dir(dir) != 0 || ui_pick_load() != 0) {
        snprintf(s_pick_cur, sizeof s_pick_cur, "%s", prev);
        ui_pick_load();
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* File browser - drawing                                              */
/* ------------------------------------------------------------------ */

/* Fits src into max_w px, dropping the head and prefixing an ellipsis. */
static const char *ui_fit_tail(const char *src, float size, float max_w,
                               char *buf, size_t cap)
{
    const char *p = src;

    snprintf(buf, cap, "%s", src);
    if (ui_measure(size, buf) <= max_w) {
        return buf;
    }
    for (;;) {
        if (pspsky_utf8_next(&p) == 0) {
            break;
        }
        snprintf(buf, cap, "…%s", p);
        if (ui_measure(size, buf) <= max_w) {
            break;
        }
    }
    return buf;
}

/* Measuring is per character, so this is cached until the directory changes. */
static const char *ui_pick_header_text(void)
{
    static char src[PSPSKY_UI_PICK_PATH_MAX];
    static char buf[PSPSKY_UI_PICK_PATH_MAX + 8];

    if (src[0] != '\0' && strcmp(src, s_pick_cur) == 0) {
        return buf;
    }
    snprintf(src, sizeof src, "%s", s_pick_cur);
    return ui_fit_tail(s_pick_cur, UI_SIZE_HEADER, (float)(UI_SCR_W - 12),
                       buf, sizeof buf);
}

static void ui_pick_draw(int cursor, int top)
{
    int i;

    ui_frame_begin();

    ui_draw_rect(0, 0, UI_SCR_W, UI_HEADER_H, UI_COL_BAR);
    ui_text(UI_SIZE_HEADER, UI_COL_TEXT, 6.0f, 14.0f, ui_pick_header_text());

    for (i = 0; i < UI_PICK_ROWS && (top + i) < s_pick_count; i++) {
        const ui_pick_entry_t *e   = &s_pick[top + i];
        int   y                    = UI_LIST_TOP + i * UI_LINE_H;
        int   is_cursor            = ((top + i) == cursor);
        unsigned int col;
        char  namebuf[UI_PICK_NAME_MAX + 2];

        /* Bar, dark text and ">" together. */
        if (is_cursor) {
            ui_draw_rect(UI_MARGIN_X - 6, y,
                         UI_SCR_W - (UI_MARGIN_X - 6) * 2, UI_LINE_H,
                         UI_COL_SEL);
            ui_text(UI_SIZE_BODY, UI_COL_SEL_TEXT, (float)UI_MARGIN_X,
                    (float)(y + UI_BASELINE), UI_PICK_CURSOR);
            col = UI_COL_SEL_TEXT;
        } else {
            col = e->is_dir ? UI_COL_NAME : UI_COL_TEXT;
        }

        /* Indent by the marker width so all rows line up. */
        if (e->is_dir) {
            snprintf(namebuf, sizeof namebuf, "%s/", e->name);
            ui_text(UI_SIZE_BODY, col, (float)(UI_MARGIN_X + 12),
                    (float)(y + UI_BASELINE), namebuf);
        } else {
            ui_text(UI_SIZE_BODY, col, (float)(UI_MARGIN_X + 12),
                    (float)(y + UI_BASELINE), e->name);
        }
    }

    /* Directory with no entries other than "..". */
    if (s_pick_count - (s_pick_parent ? 1 : 0) <= 0) {
        int y = UI_LIST_TOP + (s_pick_parent ? 1 : 0) * UI_LINE_H;

        ui_text(UI_SIZE_BODY, UI_COL_DIM, (float)UI_MARGIN_X,
                (float)(y + UI_BASELINE),
                pspsky_tr(PSPSKY_STR_UI_PICK_EMPTY));
    }

    ui_draw_rect(0, UI_FOOTER_Y, UI_SCR_W, UI_FOOTER_H, UI_COL_BAR);
    ui_text(UI_SIZE_FOOTER, UI_COL_TEXT, 6.0f, (float)(UI_FOOTER_Y + 14),
            UI_PICK_FOOTER_TEXT);

    ui_frame_end();
}

/* ------------------------------------------------------------------ */
/* Image preview - texture upload and drawing                          */
/* ------------------------------------------------------------------ */

/* PSP texture sides must be powers of two. */
static int ui_prev_pow2(int v)
{
    int p = UI_PREV_MIN_DIM;

    while (p < v && p < UI_PREV_MAX_DIM) {
        p <<= 1;
    }
    return p;
}

/* RGB888 to 0xAABBGGRR into s_prev_tex; call once, before the loop. */
static void ui_prev_upload(const pspsky_ui_preview_t *pv, int potw, int poth)
{
    int x, y;

    for (y = 0; y < pv->thumb_h; y++) {
        const unsigned char *src = pv->rgb + (size_t)y * (size_t)pv->thumb_w * 3u;
        unsigned int        *dst = s_prev_tex + (size_t)y * (size_t)potw;

        for (x = 0; x < pv->thumb_w; x++) {
            dst[x] = 0xFF000000u |
                     ((unsigned int)src[x * 3 + 2] << 16) |
                     ((unsigned int)src[x * 3 + 1] << 8) |
                     (unsigned int)src[x * 3 + 0];
        }
        /* Duplicate the edge so GU_LINEAR does not sample padding. */
        if (potw > pv->thumb_w) {
            dst[pv->thumb_w] = dst[pv->thumb_w - 1];
        }
    }
    if (poth > pv->thumb_h) {   /* same for the bottom row */
        memcpy(s_prev_tex + (size_t)pv->thumb_h * (size_t)potw,
               s_prev_tex + (size_t)(pv->thumb_h - 1) * (size_t)potw,
               (size_t)potw * sizeof s_prev_tex[0]);
    }

    sceKernelDcacheWritebackRange(s_prev_tex,
                                  (unsigned int)poth * (unsigned int)potw * 4u);
}

/* Centred and aspect-preserving; intraFont clobbers the texture state, so it
   is re-applied every frame. */
static void ui_prev_draw_image(const pspsky_ui_preview_t *pv, int potw, int poth)
{
    ui_tex_vertex_f_t *v;
    float sx, sy, scale;
    int   dw, dh, dx, dy;

    sx    = (float)UI_PREV_AREA_W / (float)pv->thumb_w;
    sy    = (float)UI_PREV_AREA_H / (float)pv->thumb_h;
    scale = (sx < sy) ? sx : sy;

    dw = (int)((float)pv->thumb_w * scale);
    dh = (int)((float)pv->thumb_h * scale);
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }
    if (dw > UI_PREV_AREA_W) {
        dw = UI_PREV_AREA_W;
    }
    if (dh > UI_PREV_AREA_H) {
        dh = UI_PREV_AREA_H;
    }
    dx = (UI_SCR_W - dw) / 2;
    dy = UI_PREV_AREA_Y + (UI_PREV_AREA_H - dh) / 2;

    /* Drawn first: ui_draw_rect disables texturing. */
    ui_draw_rect(dx - 1, dy - 1, dw + 2, dh + 2, UI_COL_SEP);

    /* Must be float (32BITF): 16-bit textured sprites drawn after
       intraFontPrint break GE rendering on hardware. intraFont uses 32BITF
       itself, and matching it avoids the conflict. */
    v = (ui_tex_vertex_f_t *)sceGuGetMemory(2 * sizeof(ui_tex_vertex_f_t));
    if (!v) {
        return;
    }
    v[0].u = 0.0f;
    v[0].v = 0.0f;
    v[0].x = (float)dx;
    v[0].y = (float)dy;
    v[0].z = 0.0f;
    v[1].u = (float)pv->thumb_w;
    v[1].v = (float)pv->thumb_h;
    v[1].x = (float)(dx + dw);
    v[1].y = (float)(dy + dh);
    v[1].z = 0.0f;

    /* intraFontPrint returns with GU_DEPTH_TEST enabled. */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, potw, poth, potw, s_prev_tex);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    /* GU_TCC_RGB takes alpha from the vertex colour; these carry none. */
    sceGuColor(0xFFFFFFFF);
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   2, 0, v);

    /* intraFontPrint skips rebinding its texture while INTRAFONT_ACTIVE, so
       later text would sample the image bound above. */
    if (s_font) {
        intraFontActivate(s_font);
    }
}

/* has_img=0 selects the fallback display. */
static void ui_prev_draw(const pspsky_ui_preview_t *pv, int has_img,
                         int potw, int poth)
{
    static char  pathbuf[PSPSKY_UI_PICK_PATH_MAX + 8];
    char         info[64];
    unsigned int kb = (unsigned int)((pv->file_bytes + 1023u) / 1024u);
    float        w;

    ui_frame_begin();

    ui_draw_rect(0, 0, UI_SCR_W, UI_HEADER_H, UI_COL_BAR);
    ui_text(UI_SIZE_HEADER, UI_COL_TEXT, 6.0f, 14.0f, UI_PREV_TITLE);

    if (has_img) {
        ui_prev_draw_image(pv, potw, poth);
    } else {
        w = ui_measure(UI_SIZE_MSG, UI_PREV_NO_IMAGE);
        ui_text(UI_SIZE_MSG, UI_COL_TEXT, ((float)UI_SCR_W - w) * 0.5f,
                (float)(UI_PREV_AREA_Y + UI_PREV_AREA_H / 2 - 4),
                UI_PREV_NO_IMAGE);
        w = ui_measure(UI_SIZE_NAME, UI_PREV_NO_IMAGE_SUB);
        ui_text(UI_SIZE_NAME, UI_COL_DIM, ((float)UI_SCR_W - w) * 0.5f,
                (float)(UI_PREV_AREA_Y + UI_PREV_AREA_H / 2 + 18),
                UI_PREV_NO_IMAGE_SUB);
    }

    /* Path, tail-first when too wide. */
    ui_text(UI_SIZE_NAME, UI_COL_TEXT, (float)UI_MARGIN_X,
            (float)(UI_PREV_INFO_Y1 + UI_BASELINE),
            ui_fit_tail(pv->path, UI_SIZE_NAME, UI_PREV_INFO_W,
                        pathbuf, sizeof pathbuf));

    /* Size (KB, rounded up) and source dimensions. */
    if (pv->src_w > 0 && pv->src_h > 0) {
        snprintf(info, sizeof info, "%u KB   %d x %d px",
                 kb, pv->src_w, pv->src_h);
    } else {
        snprintf(info, sizeof info, "%u KB   %s", kb,
                 pspsky_tr(PSPSKY_STR_UI_PREV_DIM_UNKNOWN));
    }
    ui_text(UI_SIZE_NAME, UI_COL_DIM, (float)UI_MARGIN_X,
            (float)(UI_PREV_INFO_Y2 + UI_BASELINE), info);

    ui_draw_rect(0, UI_FOOTER_Y, UI_SCR_W, UI_FOOTER_H, UI_COL_BAR);
    ui_text(UI_SIZE_FOOTER, UI_COL_TEXT, 6.0f, (float)(UI_FOOTER_Y + 14),
            UI_PREV_FOOTER_TEXT);

    ui_frame_end();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int pspsky_ui_init(void)
{
    int i;

    if (s_inited) {
        return PSPSKY_OK;
    }

    sceGuInit();
    sceGuStart(GU_DIRECT, s_gu_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, UI_BUF_W);
    sceGuDispBuffer(UI_SCR_W, UI_SCR_H, (void *)UI_FRAME_SIZE, UI_BUF_W);
    sceGuDepthBuffer((void *)(UI_FRAME_SIZE * 2), UI_BUF_W);
    sceGuOffset(2048 - (UI_SCR_W / 2), 2048 - (UI_SCR_H / 2));
    sceGuViewport(2048, 2048, UI_SCR_W, UI_SCR_H);
    sceGuDepthRange(65535, 0);
    /* intraFontPrint re-enables GU_DEPTH_TEST; with the GE default (NEVER)
       everything except text would then be discarded. */
    sceGuDepthFunc(GU_ALWAYS);
    sceGuScissor(0, 0, UI_SCR_W, UI_SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    if (!intraFontInit()) {
        printf("pspsky_ui_init: intraFontInit failed\n");
        sceGuTerm();
        return PSPSKY_E_IO;
    }
    /* CACHE_LARGE is a 512x512 glyph cache instead of 256x256. The medium one
       holds about 100 glyphs, so a screen full of kanji thrashes it and every
       frame re-rasterises glyphs it just evicted. */
    s_font = intraFontLoad("flash0:/font/jpn0.pgf", INTRAFONT_CACHE_LARGE);
    if (!s_font) {
        printf("pspsky_ui_init: jpn0.pgf load failed, falling back to ltn0.pgf\n");
        s_font = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_LARGE);
    }
    if (!s_font) {
        printf("pspsky_ui_init: no usable font\n");
        intraFontShutdown();
        sceGuTerm();
        return PSPSKY_E_IO;
    }
    intraFontSetEncoding(s_font, INTRAFONT_STRING_UTF8);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    for (i = 0; i < UI_TL_TEX_SLOTS; i++) {
        s_tl_tex_post[i] = -1;
    }
    s_tl_tex_next  = 0;
    s_prev_buttons = 0;
    s_scroll       = 0;
    s_content_h    = 0;
    s_layout_valid = 0;
    s_layout_count = 0;
    s_inited       = 1;
    return PSPSKY_OK;
}

void pspsky_ui_term(void)
{
    if (!s_inited) {
        return;
    }
    if (s_font) {
        intraFontUnload(s_font);
        s_font = NULL;
    }
    intraFontShutdown();
    sceGuTerm();
    s_inited = 0;
}

void pspsky_ui_status(const char *msg_utf8)
{
    if (!s_inited) {
        return;
    }
    ui_frame_begin();
    ui_draw_message(msg_utf8, NULL);
    ui_frame_end();
}

void pspsky_ui_message(const char *msg_utf8)
{
    SceCtrlData pad;
    int         first = 1;

    if (!s_inited) {
        return;
    }
    for (;;) {
        unsigned int pressed;

        ui_frame_begin();
        ui_draw_message(msg_utf8, pspsky_tr(PSPSKY_STR_UI_HINT_OK));
        ui_frame_end();

        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        if (first) {
            /* Re-baseline so a button held on entry is not read as a press. */
            s_prev_buttons = pad.Buttons;
            first = 0;
            continue;
        }
        pressed        = pad.Buttons & ~s_prev_buttons;
        s_prev_buttons = pad.Buttons;
        if (pressed & PSP_CTRL_CIRCLE) {
            return;
        }
    }
}

int pspsky_ui_confirm(const char *msg_utf8)
{
    SceCtrlData pad;
    int         first = 1;

    if (!s_inited) {
        return 0;
    }
    for (;;) {
        unsigned int pressed;

        ui_frame_begin();
        ui_draw_message(msg_utf8, pspsky_tr(PSPSKY_STR_UI_HINT_YES_NO));
        ui_frame_end();

        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        if (first) {
            s_prev_buttons = pad.Buttons;
            first = 0;
            continue;
        }
        pressed        = pad.Buttons & ~s_prev_buttons;
        s_prev_buttons = pad.Buttons;
        if (pressed & PSP_CTRL_CIRCLE) {
            return 1;
        }
        if (pressed & PSP_CTRL_CROSS) {
            return 0;
        }
    }
}

pspsky_ui_compose_choice_t pspsky_ui_compose_confirm(const char *msg_utf8)
{
    SceCtrlData pad;
    int         first = 1;

    if (!s_inited) {
        return PSPSKY_UI_COMPOSE_CANCEL;
    }
    for (;;) {
        unsigned int pressed;

        ui_frame_begin();
        ui_draw_message(msg_utf8, UI_COMPOSE_HINT);
        ui_frame_end();

        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        if (first) {
            /* Re-baseline so a button held on entry is not read as a press. */
            s_prev_buttons = pad.Buttons;
            first = 0;
            continue;
        }
        pressed        = pad.Buttons & ~s_prev_buttons;
        s_prev_buttons = pad.Buttons;
        if (pressed & PSP_CTRL_CIRCLE) {
            return PSPSKY_UI_COMPOSE_POST;
        }
        if (pressed & PSP_CTRL_SQUARE) {
            return PSPSKY_UI_COMPOSE_POST_WITH_IMAGE;
        }
        if (pressed & PSP_CTRL_CROSS) {
            return PSPSKY_UI_COMPOSE_CANCEL;
        }
    }
}

int pspsky_ui_pick_jpeg(const char *start_dir, char *out_path, size_t out_cap)
{
    SceCtrlData  pad;
    unsigned int hold_dir    = 0;   /* direction key currently repeating */
    int          hold_frames = 0;
    int          cursor      = 0;
    int          top         = 0;
    int          first       = 1;

    if (out_path == NULL || out_cap == 0) {
        return PSPSKY_E_ARG;
    }
    out_path[0] = '\0';
    if (!s_inited) {
        return PSPSKY_E_ARG;   /* only callable after pspsky_ui_init() */
    }

    /* Fall back to "ms0:/" if start_dir cannot be opened. */
    if (start_dir == NULL || start_dir[0] == '\0' ||
        ui_pick_set_dir(start_dir) != 0 || ui_pick_load() != 0) {
        if (ui_pick_set_dir("ms0:/") != 0 || ui_pick_load() != 0) {
            return PSPSKY_E_IO;
        }
    }

    for (;;) {
        unsigned int buttons, pressed;
        int          step = 0;

        /* Keep the window following the cursor. */
        if (cursor < top) {
            top = cursor;
        }
        if (cursor >= top + UI_PICK_ROWS) {
            top = cursor - UI_PICK_ROWS + 1;
        }
        if (top > s_pick_count - UI_PICK_ROWS) {
            top = s_pick_count - UI_PICK_ROWS;
        }
        if (top < 0) {
            top = 0;
        }

        ui_pick_draw(cursor, top);

        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        buttons = pad.Buttons;
        if (first) {
            s_prev_buttons = buttons;
            first = 0;
            continue;
        }
        pressed        = buttons & ~s_prev_buttons;
        s_prev_buttons = buttons;

        /* Held: repeat every UI_PICK_REPEAT_RATE frames after the delay. */
        if (pressed & PSP_CTRL_UP) {
            step        = -1;
            hold_dir    = PSP_CTRL_UP;
            hold_frames = 0;
        } else if (pressed & PSP_CTRL_DOWN) {
            step        = 1;
            hold_dir    = PSP_CTRL_DOWN;
            hold_frames = 0;
        } else if (hold_dir != 0 && (buttons & hold_dir) != 0) {
            hold_frames++;
            if (hold_frames >= UI_PICK_REPEAT_DELAY &&
                ((hold_frames - UI_PICK_REPEAT_DELAY) % UI_PICK_REPEAT_RATE) == 0) {
                step = (hold_dir == PSP_CTRL_UP) ? -1 : 1;
            }
        } else {
            hold_dir    = 0;
            hold_frames = 0;
        }

        if (step != 0 && s_pick_count > 0) {
            cursor += step;
            if (cursor < 0) {
                cursor = 0;
            }
            if (cursor >= s_pick_count) {
                cursor = s_pick_count - 1;
            }
        }

        /* --- Circle: confirm (one action per frame) ----------------- */
        if ((pressed & PSP_CTRL_CIRCLE) && s_pick_count > 0 &&
            cursor >= 0 && cursor < s_pick_count) {
            const ui_pick_entry_t *e = &s_pick[cursor];

            if (e->is_dir) {
                if (strcmp(e->name, "..") == 0) {
                    ui_pick_parent_path(s_pick_path, sizeof s_pick_path);
                } else if (ui_pick_join(s_pick_path, sizeof s_pick_path,
                                        e->name, 1) != 0) {
                    pspsky_ui_message(pspsky_tr(PSPSKY_STR_UI_MSG_PATH_TOO_LONG));
                    first = 1;
                    continue;
                }
                if (ui_pick_enter(s_pick_path) != 0) {
                    pspsky_ui_message(pspsky_tr(PSPSKY_STR_UI_MSG_DIR_OPEN_FAIL));
                } else {
                    cursor = 0;   /* reset cursor and window to the top */
                    top    = 0;
                }
                first       = 1;
                hold_dir    = 0;
                hold_frames = 0;
                continue;
            }

            /* File: return the full path. */
            if (ui_pick_join(s_pick_path, sizeof s_pick_path, e->name, 0) != 0 ||
                strlen(s_pick_path) + 1 > out_cap) {
                pspsky_ui_message(pspsky_tr(PSPSKY_STR_UI_MSG_PATH_TOO_LONG));
                first = 1;
                continue;
            }
            snprintf(out_path, out_cap, "%s", s_pick_path);
            return PSPSKY_OK;
        }

        /* --- Cross: cancel the browser entirely -------------------- */
        if (pressed & PSP_CTRL_CROSS) {
            out_path[0] = '\0';
            return PSPSKY_E_CANCELLED;
        }
    }
}

int pspsky_ui_preview_image(const pspsky_ui_preview_t *pv)
{
    SceCtrlData pad;
    int         first   = 1;
    int         has_img = 0;
    int         potw    = UI_PREV_MIN_DIM;
    int         poth    = UI_PREV_MIN_DIM;

    if (!s_inited || pv == NULL || pv->path == NULL) {
        return 0;
    }

    /* Built once; out-of-range dimensions fall back to no image. */
    if (pv->rgb != NULL &&
        pv->thumb_w >= 1 && pv->thumb_w <= UI_PREV_MAX_DIM &&
        pv->thumb_h >= 1 && pv->thumb_h <= UI_PREV_MAX_DIM) {
        potw    = ui_prev_pow2(pv->thumb_w);
        poth    = ui_prev_pow2(pv->thumb_h);
        has_img = 1;
        ui_prev_upload(pv, potw, poth);
    }

    for (;;) {
        unsigned int pressed;

        ui_prev_draw(pv, has_img, potw, poth);

        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        if (first) {
            /* Re-baseline so a button held on entry is not read as a press. */
            s_prev_buttons = pad.Buttons;
            first = 0;
            continue;
        }
        pressed        = pad.Buttons & ~s_prev_buttons;
        s_prev_buttons = pad.Buttons;
        if (pressed & PSP_CTRL_CIRCLE) {
            return 1;   /* Circle: post with this image */
        }
        if (pressed & PSP_CTRL_CROSS) {
            return 0;   /* Cross: pick a different one */
        }
    }
}

void pspsky_ui_timeline_invalidate(int reset_scroll)
{
    int i;

    s_layout_valid = 0;
    /* The posts array is about to change and main may free the rgb buffers, so
       no slot may go on claiming to hold a given post index. */
    for (i = 0; i < UI_TL_TEX_SLOTS; i++) {
        s_tl_tex_post[i] = -1;
    }
    if (reset_scroll) {
        s_scroll = 0;
    }
}

pspsky_ui_action_t pspsky_ui_timeline_frame(const pspsky_ui_timeline_view_t *view)
{
    pspsky_ui_action_t act = PSPSKY_UI_ACT_NONE;
    SceCtrlData        pad;
    unsigned int       buttons, pressed;
    int                max_scroll, at_bottom;
    int                n, i;

    if (!s_inited || !view) {
        return PSPSKY_UI_ACT_NONE;
    }

    n = ui_clamp_count(view->count);
    if (!s_layout_valid || s_layout_count != n) {
        ui_rebuild_layout(view);
    }

    /* --- input ----------------------------------------------------- */
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(&pad, 1);
    buttons        = pad.Buttons;
    pressed        = buttons & ~s_prev_buttons;
    s_prev_buttons = buttons;

    max_scroll = ui_max_scroll();
    if (buttons & PSP_CTRL_UP) {
        s_scroll -= UI_SCROLL_STEP;
    }
    if (buttons & PSP_CTRL_DOWN) {
        s_scroll += UI_SCROLL_STEP;
    }
    if (pad.Ly < (128 - UI_ANALOG_DEAD)) {
        s_scroll -= UI_SCROLL_STEP;
    } else if (pad.Ly > (128 + UI_ANALOG_DEAD)) {
        s_scroll += UI_SCROLL_STEP;
    }
    if (s_scroll > max_scroll) {
        s_scroll = max_scroll;
    }
    if (s_scroll < 0) {
        s_scroll = 0;
    }
    at_bottom = (s_scroll >= max_scroll);

    /* --- action selection (one action per frame) ------------------- */
    if (((pressed & PSP_CTRL_RTRIGGER) ||
         (at_bottom && (pressed & PSP_CTRL_DOWN))) && view->has_more) {
        act = PSPSKY_UI_ACT_LOAD_OLDER;
    } else if (pressed & PSP_CTRL_LTRIGGER) {
        act = PSPSKY_UI_ACT_REFRESH;
    } else if (pressed & PSP_CTRL_TRIANGLE) {
        act = PSPSKY_UI_ACT_COMPOSE;
    } else if (pressed & PSP_CTRL_START) {
        act = PSPSKY_UI_ACT_QUIT;
    }

    /* --- drawing --------------------------------------------------- */
    ui_frame_begin();

    if (n == 0 || view->posts == NULL) {
        const char *empty = pspsky_tr(PSPSKY_STR_UI_TL_EMPTY);
        float       w     = ui_measure(UI_SIZE_MSG, empty);

        ui_text(UI_SIZE_MSG, UI_COL_DIM, ((float)UI_SCR_W - w) * 0.5f,
                (float)(UI_LIST_TOP + UI_LIST_H / 2), empty);
    } else {
        for (i = 0; i < n; i++) {
            int top    = UI_LIST_TOP + s_post_top[i] - s_scroll;
            int bottom = UI_LIST_TOP + s_post_top[i + 1] - s_scroll;

            if (bottom < UI_LIST_TOP) {
                continue;
            }
            if (top > UI_LIST_BOTTOM) {
                break;
            }
            /* intraFont may change the scissor. */
            sceGuScissor(0, UI_LIST_TOP, UI_SCR_W, UI_LIST_H);
            ui_draw_post(&view->posts[i], i, top, view->now_unix,
                         ui_tl_image(view, i));
        }
        sceGuScissor(0, 0, UI_SCR_W, UI_SCR_H);
        ui_draw_scrollbar();
    }

    sceGuScissor(0, 0, UI_SCR_W, UI_SCR_H);
    ui_draw_header(view->my_handle);
    ui_draw_footer();

    ui_frame_end();

    return act;
}
