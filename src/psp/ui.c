#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <psputils.h>

#include <intraFont.h>

#include "pspsky/error.h"
#include "pspsky/strings.h"
#include "pspsky/timeutil.h"
#include "pspsky/ui.h"
#include "pspsky/utf8.h"

#include "ui_atlas.h"

#define UI_SCR_W        480
#define UI_SCR_H        272
#define UI_BUF_W        512
#define UI_FRAME_SIZE   (UI_BUF_W * UI_SCR_H * 4)  /* GU_PSM_8888 */

/*
 * Timeline content extends under the sliding bars; padding keeps posts clear
 * when the bars are visible.
 */
#define UI_HEADER_H     24  /* bottom border included */
#define UI_FOOTER_H     22  /* top border included */
#define UI_FOOTER_Y     (UI_SCR_H - UI_FOOTER_H)
#define UI_PAD_TOP      UI_HEADER_H
#define UI_PAD_BOTTOM   UI_FOOTER_H

#define UI_AVATAR_X     10
#define UI_AVATAR_PX    PSPSKY_UI_AVATAR_PX
#define UI_CONTENT_X    (UI_AVATAR_X + UI_AVATAR_PX + 8)
#define UI_CONTENT_W    (UI_SCR_W - UI_CONTENT_X - 12)
#define UI_POST_PAD_T   7
#define UI_POST_PAD_B   8
#define UI_META_H       17  /* name, handle, relative time */
#define UI_META_BASE    13  /* baseline offset */
#define UI_CTX_H        15  /* repost/reply line */
#define UI_CTX_BASE     11
#define UI_CTX_ICON     12
#define UI_LINE_H       16
#define UI_BASELINE     12
#define UI_DOT_GAP      4  /* gap on each side of the time separator */
#define UI_DOT_PX       3
#define UI_TL_IMG_GAP   5
#define UI_IMG_RADIUS   6
#define UI_CHIP_H       20  /* fallback image marker */
#define UI_END_H        36  /* load-more row */

#define UI_SCROLL_STEP  8
#define UI_SCROLL_FAST  16
#define UI_SCROLL_HOLD  30  /* frames before fast scrolling */
#define UI_ANALOG_DEAD  48  /* dead zone for stick drift */
#define UI_ANALOG_MAX   18  /* pixels per frame at full tilt */

#define UI_SIZE_TAB     0.75f
#define UI_SIZE_NAME    0.75f
#define UI_SIZE_BODY    0.80f
#define UI_SIZE_SMALL   0.65f
#define UI_SIZE_KEY     0.55f
#define UI_SIZE_MSG     0.80f

/* GU colors use 0xAABBGGRR. */
#define UI_RGB(r, g, b) \
    (0xFF000000u | ((unsigned int)(b) << 16) | ((unsigned int)(g) << 8) | \
     (unsigned int)(r))

/* Bluesky dim theme: inverted subdued palette from @bsky.app/alf. */
#define UI_COL_BG       UI_RGB(0x15, 0x1D, 0x28)  /* contrast_0 */
#define UI_COL_CARD     UI_RGB(0x1C, 0x27, 0x36)  /* contrast_25 */
#define UI_COL_BORDER   UI_RGB(0x2C, 0x3A, 0x4E)  /* contrast_100 */
#define UI_COL_KEYCAP   UI_RGB(0x39, 0x49, 0x60)  /* contrast_200 */
#define UI_COL_KNOB     UI_RGB(0x48, 0x5B, 0x75)  /* contrast_300 */
#define UI_COL_TEXT     UI_RGB(0xFF, 0xFF, 0xFF)  /* contrast_1000 */
#define UI_COL_MED      UI_RGB(0xAB, 0xB8, 0xC9)  /* text_contrast_medium */
#define UI_COL_PRIMARY  UI_RGB(0x0F, 0x73, 0xFF)  /* primary_500 */
#define UI_COL_LINK     UI_RGB(0x4D, 0x97, 0xFF)  /* text_link (primary_600) */
#define UI_COL_AVATAR   UI_RGB(0x00, 0x70, 0xFF)
/* Combine a bright bar, dark text and ">" for contrast on the PSP LCD. */
#define UI_COL_SEL      UI_COL_LINK
#define UI_COL_SEL_TEXT UI_COL_BG

#define UI_MAX_LAYOUT_POSTS 128  /* must cover the timeline limit in main */
#define UI_WRAP_MAX_LINES   64
/* ui_fit_head_calc() and ui_fit_head_apply() must use the same capacity. */
#define UI_NAME_BUF         (PSPSKY_MAX_HANDLE + 8)
#define UI_HSRC_BUF         (PSPSKY_MAX_HANDLE + 8)  /* source with leading " @" */
#define UI_HANDLE_BUF       (PSPSKY_MAX_HANDLE + 16)
#define UI_CTX_BUF          (PSPSKY_MAX_DISPLAYNAME + 64)
#define UI_GU_LIST_WORDS    32768  /* 128 KiB; also holds intraFont vertices */

#define UI_MSG_W            396.0f
#define UI_DLG_PAD          16
#define UI_DLG_MIN_W        220
#define UI_DLG_MAX_W        440
#define UI_DLG_RADIUS       10
#define UI_DLG_MAX_LINES    11  /* fits within the screen */
#define UI_BTN_H            20
#define UI_BAR_BTN_H        18
#define UI_BTN_GAP          8

#define UI_PICK_MAX_ENTRIES  200
#define UI_PICK_NAME_MAX     128
#define UI_PICK_ROW_H        18
#define UI_PICK_TOP          (UI_HEADER_H + 2)
#define UI_PICK_ROWS         ((UI_FOOTER_Y - UI_PICK_TOP) / UI_PICK_ROW_H)
#define UI_PICK_NAME_X       40
#define UI_PICK_NAME_W       (UI_SCR_W - UI_PICK_NAME_X - 12)
#define UI_PICK_FIT_BUF      (UI_PICK_NAME_MAX + 4)  /* name + UTF-8 ellipsis + NUL */
#define UI_PICK_REPEAT_DELAY 20  /* frames before repeat */
#define UI_PICK_REPEAT_RATE  4  /* frames between repeats */

#if PSPSKY_UI_TL_IMG_MAX_W > UI_CONTENT_W || \
    2 * PSPSKY_UI_TL_GRID_COL_W + PSPSKY_UI_TL_GRID_GAP > UI_CONTENT_W || \
    PSPSKY_UI_TL_GRID_ROW_H > PSPSKY_UI_TL_IMG_MAX_H
#error "timeline image bounds exceed the content column"
#endif

/* Inverted corner coverage, one quadrant per corner. */
#define UI_CORNER_TEX     16
#if 2 * UI_IMG_RADIUS > UI_CORNER_TEX
#error "UI_CORNER_TEX must hold four UI_IMG_RADIUS quadrants"
#endif

/* Sixteen avatars cover one screen and use 64 KiB. */
#define UI_AV_SLOTS       16

#define UI_TEX_MAX_DIM    512  /* power-of-two texture limits */
#define UI_TEX_MIN_DIM    16
#define UI_PREV_AREA_Y    32
#define UI_PREV_AREA_W    440
#define UI_PREV_AREA_H    170
#define UI_PREV_INFO_Y1   210  /* path */
#define UI_PREV_INFO_Y2   228  /* file size and dimensions */
#define UI_PREV_INFO_W    460.0f
#define UI_PREV_RADIUS    8

#define UI_PICK_CURSOR      ">"
#define UI_PREV_TITLE       pspsky_tr(PSPSKY_STR_UI_PREV_TITLE)
#define UI_PREV_NO_IMAGE    pspsky_tr(PSPSKY_STR_UI_PREV_NO_IMAGE)
#define UI_PREV_NO_IMAGE_SUB pspsky_tr(PSPSKY_STR_UI_PREV_NO_IMAGE_SUB)
#define UI_EMBED_MARK       pspsky_tr(PSPSKY_STR_UI_EMBED_MARK)
#define UI_IMG_MARK_N_FMT   pspsky_tr(PSPSKY_STR_UI_IMG_MARK_N)

static unsigned int __attribute__((aligned(16))) s_gu_list[UI_GU_LIST_WORDS];

static intraFont   *s_font        = NULL;
static int          s_inited      = 0;

static unsigned int s_prev_buttons = 0;

/*
 * Track texture bindings because intraFont skips rebinding while
 * INTRAFONT_ACTIVE. Reset each frame.
 */
enum { UI_TEX_NONE = 0, UI_TEX_FONT, UI_TEX_ATLAS, UI_TEX_OTHER };
static int          s_tex          = UI_TEX_NONE;
static unsigned int s_frame        = 0;

static int          s_scroll       = 0;  /* scroll offset in pixels */
static int          s_shell        = 0;  /* 0 = bars visible, UI_HEADER_H = hidden */
static int          s_hold         = 0;
static int          s_vis_top      = 0;  /* visible list bounds for the current frame */
static int          s_vis_bottom   = UI_SCR_H;
static int          s_layout_valid = 0;
static int          s_layout_count = 0;
static int          s_layout_more  = 0;  /* has_more used to build the layout */
static int          s_content_h    = 0;  /* posts and end row, excluding padding */
static int          s_post_top[UI_MAX_LAYOUT_POSTS + 1];
static float        s_digit_w      = 0.0f;  /* space reserved for an extra time digit */

/* Shared UTF-8 byte offsets and lengths; not reentrant. */
static int  s_wrap_start[UI_WRAP_MAX_LINES];
static int  s_wrap_len[UI_WRAP_MAX_LINES];
static char s_line_buf[PSPSKY_MAX_TEXT + 1];

/*
 * Cache wrapped byte offsets and lengths (32 KiB) to avoid measuring every
 * code point each frame. Valid for s_layout_count posts while s_layout_valid
 * is set. PSPSKY_MAX_TEXT must fit in a short.
 */
static short s_pw_start[UI_MAX_LAYOUT_POSTS][UI_WRAP_MAX_LINES];
static short s_pw_len[UI_MAX_LAYOUT_POSTS][UI_WRAP_MAX_LINES];

/* Store truncation offsets to limit per-post memory use. */
typedef struct {
    short cut;  /* source bytes retained */
    short ell;  /* append an ellipsis */
} ui_fit_cache_t;

typedef struct {
    ui_fit_cache_t name;
    ui_fit_cache_t handle;
    ui_fit_cache_t repost;
    ui_fit_cache_t reply;
    short         lines;
    short         has_handle;
    float         handle_x;
    float         time_x;  /* start of the time separator */
    float         time_room;  /* width reserved for relative time */
} ui_post_cache_t;

static ui_post_cache_t s_post_cache[UI_MAX_LAYOUT_POSTS];

typedef struct {
    char           name[UI_PICK_NAME_MAX];
    int            is_dir;
    ui_fit_cache_t fit;
} ui_pick_entry_t;

/* Keep the directory listing (~27 KiB) off the stack. */
static ui_pick_entry_t s_pick[UI_PICK_MAX_ENTRIES];
static int             s_pick_count  = 0;
static int             s_pick_parent = 0;  /* entry 0 is a synthetic ".." */
static char            s_pick_cur[PSPSKY_UI_PICK_PATH_MAX];
static char            s_pick_path[PSPSKY_UI_PICK_PATH_MAX];

/* 1 MiB preview texture; image data occupies the top-left region. */
static unsigned int __attribute__((aligned(64)))
    s_prev_tex[UI_TEX_MAX_DIM * UI_TEX_MAX_DIM];

/* LRU cache keyed by main-owned RGB buffers; invalidated with thumbnails. */
static unsigned int __attribute__((aligned(16)))
    s_av_tex[UI_AV_SLOTS][UI_AVATAR_PX * UI_AVATAR_PX];
static const unsigned char *s_av_key[UI_AV_SLOTS];  /* NULL = empty */
static unsigned int         s_av_used[UI_AV_SLOTS];  /* s_frame of the last draw */

/* 64 KiB atlas: white RGB with mask coverage in alpha for GU_TFX_MODULATE. */
static unsigned int __attribute__((aligned(16)))
    s_atlas[UI_ATLAS_W * UI_ATLAS_H];

/*
 * RGB565 thumbnails have no alpha, so their corners are rounded by drawing
 * the surrounding color through this mask (white RGB, 255 - coverage alpha).
 */
static unsigned int __attribute__((aligned(16)))
    s_corner_tex[UI_CORNER_TEX * UI_CORNER_TEX];

/* 4x4 ordered dither for RGB565 quantization. */
static const unsigned char k_ui_dither[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

typedef struct {
    unsigned int color;
    short x, y, z;
} ui_vertex_t;

/*
 * UVs are texels. Textured sprites after intraFont require 32BITF vertices
 * on hardware.
 */
typedef struct {
    float u, v;
    float x, y, z;
} ui_tex_vertex_f_t;

typedef struct {
    float        u, v;
    unsigned int color;
    float        x, y, z;
} ui_tc_vertex_t;

/* Source coordinates are texels; destination coordinates are pixels. */
typedef struct {
    float u0, v0, u1, v1;
    float x0, y0, x1, y1;
} ui_quad_t;

typedef struct {
    int             glyph;  /* UI_ICON_TRIANGLE / CIRCLE / CROSS / SQUARE */
    pspsky_str_id_t label;
    int             primary;  /* filled with the primary color */
} ui_button_t;

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

    /* intraFont re-enables depth testing. */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}

static void ui_atlas_bind(void)
{
    /* intraFont re-enables depth testing. */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    if (s_tex == UI_TEX_ATLAS) {
        return;
    }
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, UI_ATLAS_W, UI_ATLAS_H, UI_ATLAS_W, s_atlas);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    s_tex = UI_TEX_ATLAS;
}

/* Draws with the currently bound texture. */
static void ui_quads_draw(const ui_quad_t *q, int n, unsigned int color)
{
    ui_tc_vertex_t *v;
    int             i;

    if (n <= 0) {
        return;
    }
    v = (ui_tc_vertex_t *)sceGuGetMemory((unsigned int)n * 2u * sizeof *v);
    if (!v) {
        return;
    }
    for (i = 0; i < n; i++) {
        v[2 * i].u         = q[i].u0;
        v[2 * i].v         = q[i].v0;
        v[2 * i].color     = color;
        v[2 * i].x         = q[i].x0;
        v[2 * i].y         = q[i].y0;
        v[2 * i].z         = 0.0f;
        v[2 * i + 1].u     = q[i].u1;
        v[2 * i + 1].v     = q[i].v1;
        v[2 * i + 1].color = color;
        v[2 * i + 1].x     = q[i].x1;
        v[2 * i + 1].y     = q[i].y1;
        v[2 * i + 1].z     = 0.0f;
    }
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF |
                   GU_TRANSFORM_2D,
                   n * 2, 0, v);
}

static void ui_atlas_draw(const ui_quad_t *q, int n, unsigned int color)
{
    ui_atlas_bind();
    ui_quads_draw(q, n, color);
}

static void ui_icon_scaled(int icon, float x, float y, float w, float h,
                           unsigned int color)
{
    const unsigned char *r = k_ui_icon_rect[icon];
    ui_quad_t q;

    q.u0 = (float)r[0];
    q.v0 = (float)r[1];
    q.u1 = (float)(r[0] + r[2]);
    q.v1 = (float)(r[1] + r[3]);
    q.x0 = x;
    q.y0 = y;
    q.x1 = x + w;
    q.y1 = y + h;
    ui_atlas_draw(&q, 1, color);
}

static void ui_icon(int icon, int x, int y, unsigned int color)
{
    const unsigned char *r = k_ui_icon_rect[icon];

    ui_icon_scaled(icon, (float)x, (float)y, (float)r[2], (float)r[3], color);
}

/* Splits the texel rect c into quadrants placed at the corners of x, y, w, h. */
static void ui_corner_quads(const unsigned char *c, int x, int y, int w, int h,
                            int r, ui_quad_t q[4])
{
    float     u0 = (float)c[0];
    float     v0 = (float)c[1];
    float     um = (float)c[0] + (float)c[2] * 0.5f;
    float     vm = (float)c[1] + (float)c[3] * 0.5f;
    float     u1 = (float)(c[0] + c[2]);
    float     v1 = (float)(c[1] + c[3]);
    int       i;

    for (i = 0; i < 4; i++) {
        int right  = i & 1;
        int bottom = i >> 1;
        int dx     = right ? x + w - r : x;
        int dy     = bottom ? y + h - r : y;

        q[i].u0 = right ? um : u0;
        q[i].u1 = right ? u1 : um;
        q[i].v0 = bottom ? vm : v0;
        q[i].v1 = bottom ? v1 : vm;
        q[i].x0 = (float)dx;
        q[i].y0 = (float)dy;
        q[i].x1 = (float)(dx + r);
        q[i].y1 = (float)(dy + r);
    }
}

static void ui_corners(int icon, int x, int y, int w, int h, int r,
                       unsigned int color)
{
    ui_quad_t q[4];

    ui_corner_quads(k_ui_icon_rect[icon], x, y, w, h, r, q);
    ui_atlas_draw(q, 4, color);
}

/* Paints color outside the rounded corners of x, y, w, h. */
static void ui_mask_corners(int x, int y, int w, int h, unsigned int color)
{
    static const unsigned char rect[4] = {
        0, 0, 2 * UI_IMG_RADIUS, 2 * UI_IMG_RADIUS
    };
    ui_quad_t q[4];

    if (w < 2 * UI_IMG_RADIUS || h < 2 * UI_IMG_RADIUS) {
        return;
    }
    ui_corner_quads(rect, x, y, w, h, UI_IMG_RADIUS, q);
    /* intraFont re-enables depth testing. */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, UI_CORNER_TEX, UI_CORNER_TEX, UI_CORNER_TEX, s_corner_tex);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);  /* unscaled texture */
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    s_tex = UI_TEX_OTHER;
    ui_quads_draw(q, 4, color);
}

/* Keep pieces disjoint so translucent colors blend evenly. */
static void ui_fill_rrect(int x, int y, int w, int h, int r, unsigned int color)
{
    if (r > h / 2) {
        r = h / 2;
    }
    if (r > w / 2) {
        r = w / 2;
    }
    if (r <= 0) {
        ui_draw_rect(x, y, w, h, color);
        return;
    }
    ui_draw_rect(x + r, y, w - 2 * r, h, color);
    ui_draw_rect(x, y + r, r, h - 2 * r, color);
    ui_draw_rect(x + w - r, y + r, r, h - 2 * r, color);
    ui_corners((r <= 8) ? UI_ICON_CIRCLE16 : UI_ICON_CIRCLE32,
               x, y, w, h, r, color);
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
    if (s_tex != UI_TEX_FONT) {
        intraFontActivate(s_font);
        s_tex = UI_TEX_FONT;
    }
    intraFontSetStyle(s_font, size, color, 0, 0.0f, 0);
    return intraFontPrint(s_font, x, y, s);
}

/* jpn0.pgf has no bold face; the offset pass adds one pixel to the width. */
static float ui_text_bold(float size, unsigned int color, float x, float y,
                          const char *s)
{
    ui_text(size, color, x + 1.0f, y, s);
    return ui_text(size, color, x, y, s) + 1.0f;
}

static float ui_measure_bold(float size, const char *s)
{
    return (s != NULL && *s != '\0') ? ui_measure(size, s) + 1.0f : 0.0f;
}

static void ui_frame_begin(void)
{
    sceGuStart(GU_DIRECT, s_gu_list);
    sceGuClearColor(UI_COL_BG);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    s_tex = UI_TEX_NONE;
    /* Main may reuse freed thumbnail memory for new textures between frames. */
    sceGuTexFlush();
    s_frame++;
}

static void ui_frame_end(void)
{
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

static void ui_wrap_emit(int *n, int start, int len)
{
    if (*n < UI_WRAP_MAX_LINES) {
        s_wrap_start[*n] = start;
        s_wrap_len[*n]   = (len > 0) ? len : 0;
        (*n)++;
    }
}

/* Writes byte ranges to the shared wrap buffers; returns the line count. */
static int ui_wrap(const char *text, float size, float max_w)
{
    const char *base = text;
    const char *p    = text;
    int   nlines     = 0;
    int   line_start = 0;
    float line_w     = 0.0f;
    int   sp_char    = -1;  /* byte offset of the last space */
    int   sp_next    = -1;  /* byte offset after that space */
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
            continue;
        }

        line_w += cw;
        if (cp == ' ' || cp == '\t' || cp == 0x3000) {
            sp_char = (int)(cp_start - base);
            sp_next = (int)(p - base);
        }
    }
    return nlines;
}

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

static const char *ui_wrap_line(const char *text, int i)
{
    return ui_copy_line(text, s_wrap_start[i], s_wrap_len[i]);
}

static const char ui_ell[] = "…";  /* UTF-8: three bytes plus NUL */

/*
 * Measure truncation at layout time. ui_fit_head_apply() reconstructs the
 * string without font calls; both functions must use the same capacity.
 */
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
        return;
    }

    avail = max_w - intraFontMeasureText(s_font, ui_ell);
    if (avail < 0.0f) {
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
            break;
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

/* Reconstruct a cached truncation without measuring text. */
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
        return buf;
    }
    if (used > 0) {
        memcpy(buf, src, used);
    }
    if (fc->ell && used + sizeof ui_ell <= cap) {
        memcpy(buf + used, ui_ell, sizeof ui_ell);
    } else {
        buf[used] = '\0';
    }
    return buf;
}

/* glyph < 0 selects a labeled key cap. Returns the x coordinate after it. */
static float ui_keycap(float x, int cy, const char *key, int glyph)
{
    float w;

    x = (float)(int)(x + 0.5f);  /* whole pixels keep the masks sharp */
    if (glyph >= 0) {
        ui_icon_scaled(UI_ICON_CIRCLE16, x, (float)(cy - 7), 14.0f, 14.0f,
                       UI_COL_KEYCAP);
        ui_icon(glyph, (int)x + 2, cy - 5, UI_COL_TEXT);
        return x + 14.0f;
    }
    w = (float)(int)(ui_measure(UI_SIZE_KEY, key) + 8.5f);
    ui_fill_rrect((int)x, cy - 6, (int)w, 13, 3, UI_COL_KEYCAP);
    ui_text(UI_SIZE_KEY, UI_COL_TEXT, x + 4.0f, (float)(cy + 4), key);
    return x + w;
}

static float ui_hint_w(const char *key, int glyph, pspsky_str_id_t label)
{
    float cap = (glyph >= 0) ? 14.0f
                             : (float)(int)(ui_measure(UI_SIZE_KEY, key) + 8.5f);

    return cap + 4.0f + ui_measure(UI_SIZE_SMALL, pspsky_tr(label));
}

/* Returns the x coordinate after the label. */
static float ui_hint(float x, int cy, const char *key, int glyph,
                     pspsky_str_id_t label)
{
    x = ui_keycap(x, cy, key, glyph) + 4.0f;
    return ui_text(UI_SIZE_SMALL, UI_COL_MED, x, (float)(cy + 5),
                   pspsky_tr(label));
}

static float ui_button_w(const ui_button_t *b)
{
    return (float)(int)(10.0f + 10.0f + 5.0f +
                        ui_measure_bold(UI_SIZE_SMALL, pspsky_tr(b->label)) +
                        12.5f);
}

static void ui_draw_button(float x, int y, int h, const ui_button_t *b,
                           unsigned int bg)
{
    int w  = (int)ui_button_w(b);
    int cy = y + h / 2;

    x = (float)(int)(x + 0.5f);  /* whole pixels keep the masks sharp */
    ui_fill_rrect((int)x, y, w, h, h / 2, b->primary ? UI_COL_PRIMARY : bg);
    ui_icon(b->glyph, (int)x + 10, cy - 5, UI_COL_TEXT);
    ui_text_bold(UI_SIZE_SMALL, UI_COL_TEXT, x + 25.0f, (float)(cy + 5),
                 pspsky_tr(b->label));
}

static void ui_draw_header_bar(int y)
{
    ui_draw_rect(0, y, UI_SCR_W, UI_HEADER_H - 1, UI_COL_BG);
    ui_draw_rect(0, y + UI_HEADER_H - 1, UI_SCR_W, 1, UI_COL_BORDER);
}

static void ui_draw_footer_bar(int y)
{
    ui_draw_rect(0, y, UI_SCR_W, 1, UI_COL_BORDER);
    ui_draw_rect(0, y + 1, UI_SCR_W, UI_FOOTER_H - 1, UI_COL_BG);
}

static void ui_draw_footer(int y, const ui_button_t *main_btn,
                           const char *key, int glyph, pspsky_str_id_t label)
{
    int cy = y + 1 + (UI_FOOTER_H - 1) / 2;

    ui_draw_footer_bar(y);
    if (label != PSPSKY_STR_COUNT) {
        ui_hint(10.0f, cy, key, glyph, label);
    }
    if (main_btn != NULL) {
        ui_draw_button((float)UI_SCR_W - 10.0f - ui_button_w(main_btn),
                       cy - UI_BAR_BTN_H / 2, UI_BAR_BTN_H, main_btn,
                       UI_COL_BORDER);
    }
}

static int ui_image_count(const pspsky_post_t *post)
{
    if (post->img_count < 0) {
        return 0;
    }
    return (post->img_count > PSPSKY_MAX_IMAGES) ?
           PSPSKY_MAX_IMAGES : post->img_count;
}

/* GE buffer widths for 16-bit texels must be multiples of eight. */
static int ui_tl_stride(int w)
{
    return (w + 7) & ~7;
}

/* PSP texture sides must be powers of two. */
static int ui_tex_pow2(int v)
{
    int p = UI_TEX_MIN_DIM;

    while (p < v && p < UI_TEX_MAX_DIM) {
        p <<= 1;
    }
    return p;
}

/* Layout and drawing must use the same image validity checks. */
static const pspsky_ui_tl_image_t *ui_tl_image(const pspsky_ui_tl_image_t *images,
                                              int image, int count)
{
    const pspsky_ui_tl_image_t *img;
    int max_w, max_h;

    if (images == NULL || image < 0 || image >= count) {
        return NULL;
    }
    pspsky_ui_tl_image_max(count, &max_w, &max_h);
    img = &images[image];
    if (img->tex == NULL ||
        img->w < 1 || img->w > max_w ||
        img->h < 1 || img->h > max_h) {
        return NULL;
    }
    return img;
}

/*
 * Keep original image indices. NULL means all images are unavailable and the
 * count marker should be shown.
 */
static const pspsky_ui_tl_image_t *ui_tl_images(const pspsky_ui_timeline_view_t *view,
                                               int idx)
{
    int count = ui_image_count(&view->posts[idx]);
    int i;

    if (view->images != NULL) {
        for (i = 0; i < count; i++) {
            if (ui_tl_image(view->images[idx], i, count) != NULL) {
                return view->images[idx];
            }
        }
    }
    return NULL;
}

static const pspsky_ui_avatar_t *ui_avatar(const pspsky_ui_timeline_view_t *view,
                                           int idx)
{
    return (view->avatars != NULL) ? &view->avatars[idx] : NULL;
}

/* A row with no available image falls back to the decode bound. */
static int ui_tl_row_h(const pspsky_ui_tl_image_t *images, int count, int row)
{
    int h = 0;
    int max_w, max_h, i;

    for (i = 2 * row; i < count && i < 2 * row + 2; i++) {
        const pspsky_ui_tl_image_t *img = ui_tl_image(images, i, count);

        if (img != NULL && img->h > h) {
            h = img->h;
        }
    }
    if (h == 0) {
        pspsky_ui_tl_image_max(count, &max_w, &max_h);
        h = max_h;
    }
    return h;
}

/* Must match the attachment layout in ui_draw_post(). */
static int ui_post_extra_h(const pspsky_post_t *post,
                           const pspsky_ui_tl_image_t *images)
{
    if (images != NULL) {
        int count = ui_image_count(post);
        int h     = UI_TL_IMG_GAP - PSPSKY_UI_TL_GRID_GAP;
        int row;

        if (count == 1) {
            return UI_TL_IMG_GAP + images[0].h;
        }
        for (row = 0; 2 * row < count; row++) {
            h += PSPSKY_UI_TL_GRID_GAP + ui_tl_row_h(images, count, row);
        }
        return h;
    }
    if (post->img_count >= 1) {
        return UI_TL_IMG_GAP + UI_CHIP_H;
    }
    /* Link cards and quoted records get no marker of their own. */
    return 0;
}

static const char *ui_img_mark(const pspsky_post_t *post, char *buf, size_t cap)
{
    if (post->img_count < 2) {
        return UI_EMBED_MARK;
    }
    snprintf(buf, cap, UI_IMG_MARK_N_FMT, post->img_count);
    return buf;
}

static void ui_draw_tex(int psm, const void *tex, int tw, int th, int tbw,
                        int w, int h, int x, int y)
{
    ui_tex_vertex_f_t *v;

    v = (ui_tex_vertex_f_t *)sceGuGetMemory(2 * sizeof(ui_tex_vertex_f_t));
    if (!v) {
        return;
    }
    v[0].u = 0.0f;
    v[0].v = 0.0f;
    v[0].x = (float)x;
    v[0].y = (float)y;
    v[0].z = 0.0f;
    v[1].u = (float)w;
    v[1].v = (float)h;
    v[1].x = (float)(x + w);
    v[1].y = (float)(y + h);
    v[1].z = 0.0f;

    /* intraFont re-enables depth testing. */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(psm, 0, 0, 0);
    sceGuTexImage(0, tw, th, tbw, tex);
    /* RGB565 has no alpha channel; take it from the opaque color. */
    sceGuTexFunc(GU_TFX_REPLACE,
                 (psm == GU_PSM_5650) ? GU_TCC_RGB : GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);  /* unscaled texture */
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuColor(0xFFFFFFFF);
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   2, 0, v);
    s_tex = UI_TEX_OTHER;
}

/* bg is the color around the image, used to round its corners. */
static void ui_draw_tl_image(const pspsky_ui_tl_image_t *img, int x, int y,
                             unsigned int bg)
{
    ui_draw_tex(GU_PSM_5650, img->tex, ui_tex_pow2(img->w),
                ui_tex_pow2(img->h), ui_tl_stride(img->w),
                img->w, img->h, x, y);
    ui_mask_corners(x, y, img->w, img->h, bg);
}

static const unsigned int *ui_av_tex_bind(const unsigned char *rgb)
{
    const unsigned char *c = k_ui_icon_rect[UI_ICON_CIRCLE32];
    unsigned int        *tex;
    int                  slot, best = -1, x, y;

    for (slot = 0; slot < UI_AV_SLOTS; slot++) {
        if (s_av_key[slot] == rgb) {
            s_av_used[slot] = s_frame;
            return s_av_tex[slot];
        }
    }
    for (slot = 0; slot < UI_AV_SLOTS; slot++) {
        if (s_av_key[slot] == NULL) {
            best = slot;
            break;
        }
        if (best < 0 || s_av_used[slot] < s_av_used[best]) {
            best = slot;
        }
    }

    tex = s_av_tex[best];
    for (y = 0; y < UI_AVATAR_PX; y++) {
        const unsigned char *src  = rgb + (size_t)y * UI_AVATAR_PX * 3u;
        const unsigned int  *mask = s_atlas + (size_t)(c[1] + y) * UI_ATLAS_W + c[0];
        unsigned int        *dst  = tex + (size_t)y * UI_AVATAR_PX;

        for (x = 0; x < UI_AVATAR_PX; x++) {
            dst[x] = (mask[x] & 0xFF000000u) |
                     ((unsigned int)src[x * 3 + 2] << 16) |
                     ((unsigned int)src[x * 3 + 1] << 8) |
                     (unsigned int)src[x * 3 + 0];
        }
    }
    sceKernelDcacheWritebackRange(tex, sizeof s_av_tex[0]);
    sceGuTexFlush();
    s_av_key[best]  = rgb;
    s_av_used[best] = s_frame;
    return tex;
}

static void ui_draw_avatar(const pspsky_ui_avatar_t *av, int x, int y)
{
    if (av == NULL || av->rgb == NULL) {
        ui_icon(UI_ICON_CIRCLE32, x, y, UI_COL_AVATAR);
        ui_icon(UI_ICON_AVATAR32, x, y, UI_COL_TEXT);
        return;
    }
    ui_draw_tex(GU_PSM_8888, ui_av_tex_bind(av->rgb), UI_AVATAR_PX,
                UI_AVATAR_PX, UI_AVATAR_PX, UI_AVATAR_PX, UI_AVATAR_PX, x, y);
}

static void ui_draw_img_chip(const pspsky_post_t *post, int x, int y)
{
    char        buf[32];
    const char *label = ui_img_mark(post, buf, sizeof buf);
    int         w     = 8 + 12 + 5 + (int)ui_measure(UI_SIZE_SMALL, label) + 9;

    ui_fill_rrect(x, y, w, UI_CHIP_H, 6, UI_COL_BORDER);
    ui_fill_rrect(x + 1, y + 1, w - 2, UI_CHIP_H - 2, 5, UI_COL_BG);
    ui_icon(UI_ICON_PICTURE, x + 8, y + 4, UI_COL_MED);
    ui_text(UI_SIZE_SMALL, UI_COL_MED, (float)(x + 25), (float)(y + 14), label);
}

/* An unavailable attachment keeps its original cell and image icon. */
static void ui_draw_img_placeholder(int x, int y, int w, int h)
{
    ui_fill_rrect(x, y, w, h, UI_IMG_RADIUS, UI_COL_CARD);
    if (w >= 12 && h >= 12) {
        ui_icon(UI_ICON_PICTURE, x + (w - 12) / 2, y + (h - 12) / 2, UI_COL_MED);
    }
}

/*
 * Letterboxes img on a card. An image reaching the card's corner squares is
 * masked again with the background outside the card's rounded corners.
 */
static void ui_draw_tl_cell(const pspsky_ui_tl_image_t *img, int x, int y, int h)
{
    const int w = PSPSKY_UI_TL_GRID_COL_W;
    int       dx, dy;

    if (img == NULL) {
        ui_draw_img_placeholder(x, y, w, h);
        return;
    }
    if (img->w == w && img->h == h) {
        ui_draw_tl_image(img, x, y, UI_COL_BG);
        return;
    }
    dx = (w - img->w) / 2;
    dy = (h - img->h) / 2;
    ui_fill_rrect(x, y, w, h, UI_IMG_RADIUS, UI_COL_CARD);
    ui_draw_tl_image(img, x + dx, y + dy, UI_COL_CARD);
    if (dx < UI_IMG_RADIUS && dy < UI_IMG_RADIUS) {
        ui_mask_corners(x, y, w, h, UI_COL_BG);
    }
}

static const char *ui_repost_text(const pspsky_post_t *post, char *buf, size_t cap)
{
    switch (post->repost) {
    case PSPSKY_REPOST_BY:
        snprintf(buf, cap, pspsky_tr(PSPSKY_STR_UI_REPOSTED_BY), post->repost_by);
        return buf;
    case PSPSKY_REPOST_BY_SELF:
        return pspsky_tr(PSPSKY_STR_UI_REPOSTED_BY_YOU);
    default:
        return NULL;
    }
}

static const char *ui_reply_text(const pspsky_post_t *post, char *buf, size_t cap)
{
    switch (post->reply) {
    case PSPSKY_REPLY_TO:
        snprintf(buf, cap, pspsky_tr(PSPSKY_STR_UI_REPLIED_TO), post->reply_to);
        return buf;
    case PSPSKY_REPLY_TO_SELF:
        return pspsky_tr(PSPSKY_STR_UI_REPLIED_TO_YOU);
    case PSPSKY_REPLY_TO_POST:
        return pspsky_tr(PSPSKY_STR_UI_REPLIED_TO_POST);
    case PSPSKY_REPLY_TO_BLOCKED:
        return pspsky_tr(PSPSKY_STR_UI_REPLIED_TO_BLOCKED);
    default:
        return NULL;
    }
}

/*
 * Must match the vertical advances and context-line conditions in
 * ui_draw_post().
 */
static int ui_post_height(const pspsky_post_t *post, const ui_post_cache_t *c,
                          const pspsky_ui_tl_image_t *images)
{
    char buf[UI_CTX_BUF];
    int  col = UI_META_H + c->lines * UI_LINE_H + ui_post_extra_h(post, images);

    if (ui_reply_text(post, buf, sizeof buf) != NULL) {
        col += UI_CTX_H;
    }
    if (col < UI_AVATAR_PX) {
        col = UI_AVATAR_PX;
    }
    return UI_POST_PAD_T +
           ((ui_repost_text(post, buf, sizeof buf) != NULL) ? UI_CTX_H : 0) +
           col + UI_POST_PAD_B;
}

static float ui_time_w(const char *t)
{
    if (t[0] == '\0') {
        return 0.0f;
    }
    return (float)(2 * UI_DOT_GAP + UI_DOT_PX) + ui_measure(UI_SIZE_NAME, t);
}

static void ui_post_time(const pspsky_post_t *post, long long now_unix,
                         char *buf, size_t cap)
{
    if (pspsky_format_relative(post->created_at_unix, now_unix,
                               buf, cap) != PSPSKY_OK) {
        buf[0] = '\0';
    }
}

/*
 * Reserve room for the time; omit the handle when too little fits or it is
 * already used as the display name.
 */
static void ui_fit_meta(ui_post_cache_t *c, const pspsky_post_t *post, float room)
{
    char        namebuf[UI_NAME_BUF];
    char        hsrc[UI_HSRC_BUF];
    char        hbuf[UI_HANDLE_BUF];
    const char *name  = (post->display_name[0] != '\0') ? post->display_name
                                                         : post->handle;
    float       avail = (float)UI_CONTENT_W - room;
    float       name_max = (float)UI_CONTENT_W * 0.7f;
    float       w;

    hsrc[0] = '\0';
    if (name_max > avail || post->display_name[0] == '\0') {
        name_max = avail;
    }
    ui_fit_head_calc(name, UI_SIZE_NAME, name_max - 1.0f, UI_NAME_BUF, &c->name);
    w = ui_measure_bold(UI_SIZE_NAME,
                        ui_fit_head_apply(name, &c->name, namebuf, UI_NAME_BUF));

    c->time_room  = room;
    c->has_handle = 0;
    c->handle.cut = 0;
    c->handle.ell = 0;
    if (post->display_name[0] != '\0' && post->handle[0] != '\0') {
        snprintf(hsrc, sizeof hsrc, " @%s", post->handle);
        ui_fit_head_calc(hsrc, UI_SIZE_NAME, avail - w, UI_HANDLE_BUF, &c->handle);
        if (c->handle.cut >= 4) {  /* " @" and at least two handle bytes */
            c->has_handle = 1;
        } else if (name_max < avail) {
            ui_fit_head_calc(name, UI_SIZE_NAME, avail - 1.0f, UI_NAME_BUF,
                             &c->name);
            w = ui_measure_bold(UI_SIZE_NAME,
                                ui_fit_head_apply(name, &c->name, namebuf,
                                                  UI_NAME_BUF));
        }
    }
    c->handle_x = (float)UI_CONTENT_X + w;
    c->time_x   = c->handle_x;
    if (c->has_handle) {
        c->time_x += ui_measure(UI_SIZE_NAME,
                                ui_fit_head_apply(hsrc, &c->handle, hbuf,
                                                  UI_HANDLE_BUF));
    } else {
        c->handle.cut = 0;
        c->handle.ell = 0;
    }
}

static int ui_layout_post(int idx, const pspsky_post_t *post,
                          const pspsky_ui_tl_image_t *images, long long now_unix)
{
    ui_post_cache_t *c = &s_post_cache[idx];
    char             ctxbuf[UI_CTX_BUF];
    char             tbuf[24];
    const char      *ctx;
    int              lines, i;

    lines = ui_wrap(post->text, UI_SIZE_BODY, (float)UI_CONTENT_W);
    if (lines > UI_WRAP_MAX_LINES) {
        lines = UI_WRAP_MAX_LINES;
    }
    for (i = 0; i < lines; i++) {
        s_pw_start[idx][i] = (short)s_wrap_start[i];
        s_pw_len[idx][i]   = (short)s_wrap_len[i];
    }
    c->lines = (short)lines;

    /* Reserve one extra time digit before the names need refitting. */
    ui_post_time(post, now_unix, tbuf, sizeof tbuf);
    ui_fit_meta(c, post, ui_time_w(tbuf) + s_digit_w);

    c->repost.cut = 0;
    c->repost.ell = 0;
    ctx = ui_repost_text(post, ctxbuf, sizeof ctxbuf);
    if (ctx != NULL) {
        ui_fit_head_calc(ctx, UI_SIZE_SMALL, (float)UI_CONTENT_W, UI_CTX_BUF,
                         &c->repost);
    }
    c->reply.cut = 0;
    c->reply.ell = 0;
    ctx = ui_reply_text(post, ctxbuf, sizeof ctxbuf);
    if (ctx != NULL) {
        ui_fit_head_calc(ctx, UI_SIZE_SMALL,
                         (float)(UI_CONTENT_W - UI_CTX_ICON - 3), UI_CTX_BUF,
                         &c->reply);
    }

    return ui_post_height(post, c, images);
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
        y += ui_layout_post(i, &view->posts[i], ui_tl_images(view, i),
                            view->now_unix);
    }
    s_post_top[n]  = y;
    s_layout_more  = (view->has_more != 0);
    if (n > 0 && s_layout_more) {
        y += UI_END_H;
    }
    s_content_h    = y;
    s_layout_count = n;
    s_layout_valid = 1;
}

static int ui_max_scroll(void)
{
    int m = UI_PAD_TOP + s_content_h + UI_PAD_BOTTOM - UI_SCR_H;

    return (m > 0) ? m : 0;
}

/* Steps back to the start of the code point covering off, but not past lo. */
static int ui_cp_floor(const char *text, int off, int lo)
{
    while (off > lo && ((unsigned char)text[off] & 0xC0) == 0x80) {
        off--;
    }
    return off;
}

/* Draw text[start..start+len) and return the next pen position. */
static float ui_body_seg(const char *text, int start, int len,
                         unsigned int color, float x, float y)
{
    char seg[PSPSKY_MAX_TEXT];

    if (len <= 0) {
        return x;
    }
    if (len > (int)sizeof(seg) - 1) {
        len = (int)sizeof(seg) - 1;
    }
    memcpy(seg, text + start, (size_t)len);
    seg[len] = '\0';
    return ui_text(UI_SIZE_BODY, color, x, y, seg);
}

static void ui_draw_body_line(const pspsky_post_t *post, int start, int len,
                              float x, float y)
{
    int end = start + len;
    int pos = start;
    int k;

    for (k = 0; k < post->link_count && k < PSPSKY_MAX_LINK_RANGES; k++) {
        int ls = (int)post->link_start[k];
        int le = (int)post->link_end[k];

        if (le <= pos) {
            continue;
        }
        if (ls >= end) {
            break;  /* link ranges are sorted by start */
        }
        if (ls < pos) {
            ls = pos;
        }
        if (le > end) {
            le = end;
        }
        /* Clamp facet byte offsets to UTF-8 boundaries for intraFont. */
        ls = ui_cp_floor(post->text, ls, pos);
        le = ui_cp_floor(post->text, le, ls);
        if (le <= ls) {
            continue;
        }
        x   = ui_body_seg(post->text, pos, ls - pos, UI_COL_TEXT, x, y);
        x   = ui_body_seg(post->text, ls, le - ls, UI_COL_LINK, x, y);
        pos = le;
    }
    ui_body_seg(post->text, pos, end - pos, UI_COL_TEXT, x, y);
}

static int ui_visible(int y, int h)
{
    return (y + h > s_vis_top && y < s_vis_bottom);
}

/* Requires a valid layout cache and idx < s_layout_count. */
static void ui_draw_post(const pspsky_post_t *post, int idx, int top,
                         long long now_unix, const pspsky_ui_tl_image_t *images,
                         const pspsky_ui_avatar_t *av)
{
    ui_post_cache_t *c = &s_post_cache[idx];
    char        hsrc[UI_HSRC_BUF];
    char        namebuf[UI_NAME_BUF];
    char        handlebuf[UI_HANDLE_BUF];
    char        ctxbuf[UI_CTX_BUF];
    char        fitbuf[UI_CTX_BUF];
    char        tbuf[24];
    const char *name;
    const char *ctx;
    int         y, row, i;

    y = top + UI_POST_PAD_T;

    ctx = ui_repost_text(post, ctxbuf, sizeof ctxbuf);
    if (ctx != NULL) {
        if (ui_visible(y, UI_CTX_H)) {
            ui_icon(UI_ICON_REPOST, UI_CONTENT_X - UI_CTX_ICON - 4, y + 1,
                    UI_COL_MED);
            ui_text(UI_SIZE_SMALL, UI_COL_MED, (float)UI_CONTENT_X,
                    (float)(y + UI_CTX_BASE),
                    ui_fit_head_apply(ctx, &c->repost, fitbuf, sizeof fitbuf));
        }
        y += UI_CTX_H;
    }
    row = y;

    if (ui_visible(row, UI_AVATAR_PX)) {
        ui_draw_avatar(av, UI_AVATAR_X, row);
    }

    if (ui_visible(row, UI_META_H)) {
        float tw;

        ui_post_time(post, now_unix, tbuf, sizeof tbuf);
        tw = ui_time_w(tbuf);
        if (tw > c->time_room) {
            ui_fit_meta(c, post, tw + s_digit_w);
        }
        name = (post->display_name[0] != '\0') ? post->display_name : post->handle;
        ui_text_bold(UI_SIZE_NAME, UI_COL_TEXT, (float)UI_CONTENT_X,
                     (float)(row + UI_META_BASE),
                     ui_fit_head_apply(name, &c->name, namebuf, UI_NAME_BUF));
        if (c->has_handle) {
            snprintf(hsrc, sizeof hsrc, " @%s", post->handle);
            ui_text(UI_SIZE_NAME, UI_COL_MED, c->handle_x,
                    (float)(row + UI_META_BASE),
                    ui_fit_head_apply(hsrc, &c->handle, handlebuf, UI_HANDLE_BUF));
        }
        if (tbuf[0] != '\0') {
            /* jpn0.pgf lacks U+00B7; draw the separator as an icon. */
            int dx = (int)(c->time_x + 0.5f) + UI_DOT_GAP;

            ui_icon_scaled(UI_ICON_CIRCLE16, (float)dx,
                           (float)(row + UI_META_BASE - 5),
                           (float)UI_DOT_PX, (float)UI_DOT_PX, UI_COL_MED);
            ui_text(UI_SIZE_NAME, UI_COL_MED,
                    (float)(dx + UI_DOT_PX + UI_DOT_GAP),
                    (float)(row + UI_META_BASE), tbuf);
        }
    }
    y = row + UI_META_H;

    ctx = ui_reply_text(post, ctxbuf, sizeof ctxbuf);
    if (ctx != NULL) {
        if (ui_visible(y, UI_CTX_H)) {
            ui_icon(UI_ICON_REPLY, UI_CONTENT_X, y + 1, UI_COL_MED);
            ui_text(UI_SIZE_SMALL, UI_COL_MED,
                    (float)(UI_CONTENT_X + UI_CTX_ICON + 3),
                    (float)(y + UI_CTX_BASE),
                    ui_fit_head_apply(ctx, &c->reply, fitbuf, sizeof fitbuf));
        }
        y += UI_CTX_H;
    }

    for (i = 0; i < c->lines; i++) {
        int ly = y + i * UI_LINE_H;

        if (!ui_visible(ly, UI_LINE_H)) {
            continue;
        }
        ui_draw_body_line(post, s_pw_start[idx][i], s_pw_len[idx][i],
                          (float)UI_CONTENT_X, (float)(ly + UI_BASELINE));
    }
    y += c->lines * UI_LINE_H;

    /* Same cases as ui_post_extra_h(), which supplies the height. */
    if (images != NULL) {
        int count = ui_image_count(post);
        int iy = y + UI_TL_IMG_GAP;

        if (count == 1) {
            if (ui_visible(iy, images[0].h)) {
                ui_draw_tl_image(&images[0], UI_CONTENT_X, iy, UI_COL_BG);
            }
        } else {
            int r;

            for (r = 0; 2 * r < count; r++) {
                int rh = ui_tl_row_h(images, count, r);

                for (i = 2 * r; i < count && i < 2 * r + 2 &&
                                ui_visible(iy, rh); i++) {
                    ui_draw_tl_cell(ui_tl_image(images, i, count),
                                    UI_CONTENT_X + (i % 2) *
                                    (PSPSKY_UI_TL_GRID_COL_W +
                                     PSPSKY_UI_TL_GRID_GAP),
                                    iy, rh);
                }
                iy += rh + PSPSKY_UI_TL_GRID_GAP;
            }
        }
    } else if (post->img_count >= 1) {
        if (ui_visible(y + UI_TL_IMG_GAP, UI_CHIP_H)) {
            ui_draw_img_chip(post, UI_CONTENT_X, y + UI_TL_IMG_GAP);
        }
    }

    ui_draw_rect(0, top + (s_post_top[idx + 1] - s_post_top[idx]) - 1,
                 UI_SCR_W, 1, UI_COL_BORDER);
}

static void ui_draw_tl_header(int y, const char *my_handle)
{
    /* Language is fixed at startup, so this width remains valid. */
    static float tab_w = -1.0f;
    const char  *tab   = pspsky_tr(PSPSKY_STR_UI_TAB_FOLLOWING);

    ui_draw_header_bar(y);
    ui_icon(UI_ICON_LOGO20, 10, y + 3, UI_COL_PRIMARY);
    if (tab_w < 0.0f) {
        tab_w = ui_measure_bold(UI_SIZE_TAB, tab);
    }
    ui_text_bold(UI_SIZE_TAB, UI_COL_TEXT, 38.0f, (float)(y + 16), tab);
    ui_draw_rect(38, y + UI_HEADER_H - 3, (int)(tab_w + 0.5f), 2, UI_COL_PRIMARY);

    if (my_handle && my_handle[0] != '\0') {
        static char  cached[PSPSKY_MAX_HANDLE + 8];
        static float cached_w;
        char  buf[PSPSKY_MAX_HANDLE + 8];

        if (my_handle[0] == '@') {
            snprintf(buf, sizeof(buf), "%s", my_handle);
        } else {
            snprintf(buf, sizeof(buf), "@%s", my_handle);
        }
        if (strcmp(buf, cached) != 0) {
            cached_w = ui_measure(UI_SIZE_SMALL, buf);
            memcpy(cached, buf, strlen(buf) + 1);
        }
        ui_text(UI_SIZE_SMALL, UI_COL_MED,
                (float)UI_SCR_W - 10.0f - cached_w, (float)(y + 16), buf);
    }
}

static void ui_draw_tl_footer(int y, int has_more)
{
    static const ui_button_t post = {
        UI_ICON_TRIANGLE, PSPSKY_STR_UI_KEY_POST, 1
    };
    int   cy = y + 1 + (UI_FOOTER_H - 1) / 2;
    float x  = 10.0f;

    ui_draw_footer_bar(y);
    x = ui_hint(x, cy, "L", -1, PSPSKY_STR_UI_KEY_REFRESH) + 12.0f;
    if (has_more) {
        x = ui_hint(x, cy, "R", -1, PSPSKY_STR_UI_KEY_MORE) + 12.0f;
    }
    ui_hint(x, cy, "START", -1, PSPSKY_STR_UI_KEY_QUIT);
    ui_draw_button((float)UI_SCR_W - 10.0f - ui_button_w(&post),
                   cy - UI_BAR_BTN_H / 2, UI_BAR_BTN_H, &post, UI_COL_BORDER);
}

static void ui_draw_end_row(int y)
{
    float w = ui_hint_w("R", -1, PSPSKY_STR_UI_KEY_MORE);

    ui_hint(((float)UI_SCR_W - w) * 0.5f, y + UI_END_H / 2, "R", -1,
            PSPSKY_STR_UI_KEY_MORE);
}

static void ui_draw_scrollbar(void)
{
    int max_scroll = ui_max_scroll();
    int vis_h      = s_vis_bottom - s_vis_top;
    int total      = UI_PAD_TOP + s_content_h + UI_PAD_BOTTOM;
    int knob_h, knob_y;

    if (max_scroll <= 0 || vis_h <= 0) {
        return;
    }
    knob_h = vis_h * UI_SCR_H / total;
    if (knob_h < 12) {
        knob_h = 12;
    }
    knob_y = s_vis_top + (vis_h - knob_h) * s_scroll / max_scroll;
    ui_draw_rect(UI_SCR_W - 5, knob_y, 3, knob_h, UI_COL_KNOB);
}

static void ui_draw_dialog(const char *msg_utf8, int left,
                           const ui_button_t *btns, int nbtn)
{
    const char *msg   = (msg_utf8 != NULL) ? msg_utf8 : "";
    int         lines = ui_wrap(msg, UI_SIZE_MSG, UI_MSG_W);
    float       text_w = 0.0f;
    float       btn_w  = 0.0f;
    float       bx;
    int         card_w, card_h, card_x, card_y, ty, i;

    if (lines > UI_DLG_MAX_LINES) {
        lines = UI_DLG_MAX_LINES;
    }
    for (i = 0; i < lines; i++) {
        float w = ui_measure(UI_SIZE_MSG, ui_wrap_line(msg, i));

        if (w > text_w) {
            text_w = w;
        }
    }
    for (i = 0; i < nbtn; i++) {
        btn_w += ui_button_w(&btns[i]) + ((i > 0) ? (float)UI_BTN_GAP : 0.0f);
    }

    card_w = (int)((text_w > btn_w) ? text_w : btn_w) + 2 * UI_DLG_PAD;
    if (card_w < UI_DLG_MIN_W) {
        card_w = UI_DLG_MIN_W;
    }
    if (card_w > UI_DLG_MAX_W) {
        card_w = UI_DLG_MAX_W;
    }
    card_h = UI_DLG_PAD + lines * UI_LINE_H +
             ((nbtn > 0) ? 14 + UI_BTN_H : 0) + UI_DLG_PAD;
    card_x = (UI_SCR_W - card_w) / 2;
    card_y = (UI_SCR_H - card_h) / 2;

    ui_fill_rrect(card_x, card_y, card_w, card_h, UI_DLG_RADIUS, UI_COL_BORDER);
    ui_fill_rrect(card_x + 1, card_y + 1, card_w - 2, card_h - 2,
                  UI_DLG_RADIUS - 1, UI_COL_CARD);

    ty = card_y + UI_DLG_PAD;
    for (i = 0; i < lines; i++) {
        const char *line = ui_wrap_line(msg, i);
        float       x    = (float)(card_x + UI_DLG_PAD);

        if (!left) {
            x = ((float)UI_SCR_W - ui_measure(UI_SIZE_MSG, line)) * 0.5f;
        }
        ui_text(UI_SIZE_MSG, UI_COL_TEXT, x,
                (float)(ty + i * UI_LINE_H + UI_BASELINE), line);
    }

    bx = ((float)UI_SCR_W - btn_w) * 0.5f;
    for (i = 0; i < nbtn; i++) {
        ui_draw_button(bx, card_y + card_h - UI_DLG_PAD - UI_BTN_H, UI_BTN_H,
                       &btns[i], UI_COL_BORDER);
        bx += ui_button_w(&btns[i]) + (float)UI_BTN_GAP;
    }
}

static void ui_draw_status(const char *msg_utf8)
{
    const unsigned char *lr    = k_ui_icon_rect[UI_ICON_LOGO48];
    const char          *msg   = (msg_utf8 != NULL) ? msg_utf8 : "";
    int                  lines = ui_wrap(msg, UI_SIZE_MSG, UI_MSG_W);
    int                  top, i;

    if (lines > UI_DLG_MAX_LINES) {
        lines = UI_DLG_MAX_LINES;
    }
    top = (UI_SCR_H - (lr[3] + 20 + lines * UI_LINE_H)) / 2;
    ui_icon(UI_ICON_LOGO48, (UI_SCR_W - lr[2]) / 2, top, UI_COL_PRIMARY);
    top += lr[3] + 20;
    for (i = 0; i < lines; i++) {
        const char *line = ui_wrap_line(msg, i);
        float       w    = ui_measure(UI_SIZE_MSG, line);

        ui_text(UI_SIZE_MSG, UI_COL_MED, ((float)UI_SCR_W - w) * 0.5f,
                (float)(top + i * UI_LINE_H + UI_BASELINE), line);
    }
}

static void ui_draw_compose_buttons(int y, const ui_button_t *buttons, int count)
{
    float width = 0.0f;
    float x;
    int   i;

    for (i = 0; i < count; i++) {
        width += ui_button_w(&buttons[i]) + ((i > 0) ? UI_BTN_GAP : 0);
    }
    x = ((float)UI_SCR_W - width) * 0.5f;
    for (i = 0; i < count; i++) {
        ui_draw_button(x, y, UI_BTN_H, &buttons[i], UI_COL_BORDER);
        x += ui_button_w(&buttons[i]) + UI_BTN_GAP;
    }
}

static void ui_draw_compose_dialog(const char *msg_utf8,
                                   const char *const *image_paths,
                                   int image_count)
{
    static const ui_button_t actions[] = {
        { UI_ICON_CIRCLE, PSPSKY_STR_UI_KEY_POST,   1 },
        { UI_ICON_CROSS,  PSPSKY_STR_UI_KEY_CANCEL, 0 }
    };
    static const ui_button_t attachments[] = {
        { UI_ICON_SQUARE,   PSPSKY_STR_UI_KEY_POST_IMAGE,   0 },
        { UI_ICON_TRIANGLE, PSPSKY_STR_UI_KEY_REMOVE_IMAGE, 0 }
    };
    const char *msg = (msg_utf8 != NULL) ? msg_utf8 : "";
    int lines = ui_wrap(msg, UI_SIZE_MSG, UI_MSG_W);
    int fixed_h = 2 * UI_DLG_PAD + 8 + (image_count + 1) * UI_LINE_H +
                  12 + 2 * UI_BTN_H + 6;
    int max_lines = (UI_SCR_H - 16 - fixed_h) / UI_LINE_H;
    int card_x = (UI_SCR_W - UI_DLG_MAX_W) / 2;
    int card_h, card_y, y, i;
    int first_attachment = (image_count == PSPSKY_MAX_IMAGES) ? 1 : 0;
    int attachment_actions = (image_count > 0 ? 2 : 1) - first_attachment;
    char buf[PSPSKY_UI_PICK_PATH_MAX + 4];

    /* Keep the count, every filename and both control rows visible. */
    if (lines > max_lines) {
        lines = max_lines;
    }
    card_h = fixed_h + lines * UI_LINE_H;
    card_y = (UI_SCR_H - card_h) / 2;
    ui_fill_rrect(card_x, card_y, UI_DLG_MAX_W, card_h,
                  UI_DLG_RADIUS, UI_COL_BORDER);
    ui_fill_rrect(card_x + 1, card_y + 1, UI_DLG_MAX_W - 2, card_h - 2,
                  UI_DLG_RADIUS - 1, UI_COL_CARD);

    y = card_y + UI_DLG_PAD;
    for (i = 0; i < lines; i++) {
        ui_text(UI_SIZE_MSG, UI_COL_TEXT, (float)(card_x + UI_DLG_PAD),
                (float)(y + UI_BASELINE), ui_wrap_line(msg, i));
        y += UI_LINE_H;
    }
    y += 8;
    snprintf(buf, sizeof buf, pspsky_tr(PSPSKY_STR_UI_COMPOSE_IMAGES),
             image_count, PSPSKY_MAX_IMAGES);
    ui_text_bold(UI_SIZE_SMALL, UI_COL_LINK, (float)(card_x + UI_DLG_PAD),
                 (float)(y + UI_BASELINE), buf);
    y += UI_LINE_H;
    for (i = 0; i < image_count; i++) {
        const char *path = (image_paths != NULL && image_paths[i] != NULL)
                          ? image_paths[i] : "";
        const char *name = strrchr(path, '/');
        ui_fit_cache_t fit;

        name = (name != NULL) ? name + 1 : path;
        snprintf(buf, sizeof buf, "%d.", i + 1);
        ui_text(UI_SIZE_SMALL, UI_COL_MED, (float)(card_x + UI_DLG_PAD),
                (float)(y + UI_BASELINE), buf);
        ui_fit_head_calc(name, UI_SIZE_SMALL,
                         (float)(UI_DLG_MAX_W - 2 * UI_DLG_PAD - 24),
                         sizeof buf, &fit);
        ui_text(UI_SIZE_SMALL, UI_COL_TEXT,
                (float)(card_x + UI_DLG_PAD + 24), (float)(y + UI_BASELINE),
                ui_fit_head_apply(name, &fit, buf, sizeof buf));
        y += UI_LINE_H;
    }
    y += 12;
    ui_draw_compose_buttons(y, actions, 2);
    ui_draw_compose_buttons(y + UI_BTN_H + 6,
                            &attachments[first_attachment], attachment_actions);
}

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

static int ui_pick_at_root(void)
{
    size_t n = strlen(s_pick_cur);
    size_t i = 0;

    while (i < n && s_pick_cur[i] != '/') {
        i++;
    }
    return (i + 1 >= n);
}

/* s_pick_cur always ends in a slash. */
static int ui_pick_set_dir(const char *src)
{
    size_t n;

    if (src == NULL || src[0] == '\0') {
        return -1;
    }
    n = strlen(src);
    if (n + 2 > sizeof s_pick_cur) {
        return -1;
    }
    memcpy(s_pick_cur, src, n + 1);
    if (s_pick_cur[n - 1] != '/') {
        s_pick_cur[n]     = '/';
        s_pick_cur[n + 1] = '\0';
    }
    return 0;
}

static void ui_pick_parent_path(char *buf, size_t cap)
{
    size_t n    = strlen(s_pick_cur);
    size_t root = 0;

    while (root < n && s_pick_cur[root] != '/') {
        root++;
    }
    if (root < n) {
        root++;  /* keep the '/' immediately after the drive */
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

/* Returns nonzero if the joined path does not fit. */
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

static int ui_pick_load(void)
{
    SceUID      fd;
    SceIoDirent de;
    int         need_parent = !ui_pick_at_root();
    int         limit       = UI_PICK_MAX_ENTRIES - (need_parent ? 1 : 0);
    int         n           = 0;
    int         i;

    fd = sceIoDopen(s_pick_cur);
    if (fd < 0) {
        return -1;
    }
    while (n < limit) {
        int is_dir;

        memset(&de, 0, sizeof(de));
        if (sceIoDread(fd, &de) <= 0) {
            break;  /* 0 = end of directory, negative = error */
        }
        de.d_name[sizeof(de.d_name) - 1] = '\0';
        if (de.d_name[0] == '\0' ||
            strcmp(de.d_name, ".") == 0 || strcmp(de.d_name, "..") == 0) {
            continue;
        }
        if (strlen(de.d_name) > (size_t)(UI_PICK_NAME_MAX - 1)) {
            continue;
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
    /* Cache truncation to avoid measuring names each frame. */
    for (i = 0; i < n; i++) {
        ui_fit_head_calc(s_pick[i].name, UI_SIZE_NAME, (float)UI_PICK_NAME_W,
                         UI_PICK_FIT_BUF, &s_pick[i].fit);
    }
    s_pick_count  = n;
    s_pick_parent = need_parent;
    return 0;
}

/* Keep the current directory if opening dir fails. */
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

/* Preserve the path suffix when truncating. */
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

/* Cache the measured path until the directory changes. */
static const char *ui_pick_header_text(void)
{
    static char src[PSPSKY_UI_PICK_PATH_MAX];
    static char buf[PSPSKY_UI_PICK_PATH_MAX + 8];

    if (src[0] != '\0' && strcmp(src, s_pick_cur) == 0) {
        return buf;
    }
    snprintf(src, sizeof src, "%s", s_pick_cur);
    return ui_fit_tail(s_pick_cur, UI_SIZE_NAME, (float)(UI_SCR_W - 28 - 10),
                       buf, sizeof buf);
}

static void ui_pick_draw(int cursor, int top)
{
    static const ui_button_t select = {
        UI_ICON_CIRCLE, PSPSKY_STR_UI_KEY_SELECT, 1
    };
    int i;

    ui_frame_begin();

    ui_draw_header_bar(0);
    ui_icon(UI_ICON_FOLDER, 10, 6, UI_COL_LINK);
    ui_text(UI_SIZE_NAME, UI_COL_TEXT, 28.0f, 16.0f, ui_pick_header_text());

    for (i = 0; i < UI_PICK_ROWS && (top + i) < s_pick_count; i++) {
        const ui_pick_entry_t *e   = &s_pick[top + i];
        int   y                    = UI_PICK_TOP + i * UI_PICK_ROW_H;
        int   is_cursor            = ((top + i) == cursor);
        unsigned int col           = UI_COL_TEXT;
        unsigned int icol          = e->is_dir ? UI_COL_LINK : UI_COL_MED;
        char  namebuf[UI_PICK_FIT_BUF];

        if (is_cursor) {
            ui_fill_rrect(6, y + 1, UI_SCR_W - 12, UI_PICK_ROW_H - 2, 4,
                          UI_COL_SEL);
            ui_text(UI_SIZE_NAME, UI_COL_SEL_TEXT, 11.0f, (float)(y + 13),
                    UI_PICK_CURSOR);
            col  = UI_COL_SEL_TEXT;
            icol = UI_COL_SEL_TEXT;
        }
        ui_icon(e->is_dir ? UI_ICON_FOLDER : UI_ICON_PICTURE, 22, y + 3, icol);
        ui_text(UI_SIZE_NAME, col, (float)UI_PICK_NAME_X, (float)(y + 13),
                ui_fit_head_apply(e->name, &e->fit, namebuf, sizeof namebuf));
    }

    if (s_pick_count - (s_pick_parent ? 1 : 0) <= 0) {
        int y = UI_PICK_TOP + (s_pick_parent ? 1 : 0) * UI_PICK_ROW_H;

        ui_text(UI_SIZE_NAME, UI_COL_MED, (float)UI_PICK_NAME_X, (float)(y + 13),
                pspsky_tr(PSPSKY_STR_UI_PICK_EMPTY));
    }

    ui_draw_footer(UI_FOOTER_Y, &select, NULL, UI_ICON_CROSS,
                   PSPSKY_STR_UI_KEY_CANCEL);

    ui_frame_end();
}

/* Convert RGB888 to GU_PSM_8888 before entering the preview loop. */
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
    if (poth > pv->thumb_h) {
        memcpy(s_prev_tex + (size_t)pv->thumb_h * (size_t)potw,
               s_prev_tex + (size_t)(pv->thumb_h - 1) * (size_t)potw,
               (size_t)potw * sizeof s_prev_tex[0]);
    }

    sceKernelDcacheWritebackRange(s_prev_tex,
                                  (unsigned int)poth * (unsigned int)potw * 4u);
}

/* intraFont changes texture state; restore it on every draw. */
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

    /* intraFont re-enables depth testing. */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    /* Upload runs outside the display list, so flush the texture cache here. */
    sceGuTexFlush();
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, potw, poth, potw, s_prev_tex);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    /* GU_TCC_RGB needs a color alpha; these vertices have no color field. */
    sceGuColor(0xFFFFFFFF);
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   2, 0, v);
    s_tex = UI_TEX_OTHER;

    ui_corners(UI_ICON_ICIRCLE16, dx, dy, dw, dh, UI_PREV_RADIUS, UI_COL_BG);
}

static void ui_prev_draw(const pspsky_ui_preview_t *pv, int has_img,
                         int potw, int poth)
{
    static const ui_button_t post = {
        UI_ICON_CIRCLE, PSPSKY_STR_UI_KEY_POST_THIS, 1
    };
    static char  pathbuf[PSPSKY_UI_PICK_PATH_MAX + 8];
    char         info[64];
    unsigned int kb = (unsigned int)((pv->file_bytes + 1023u) / 1024u);
    float        w;

    ui_frame_begin();

    ui_draw_header_bar(0);
    ui_icon(UI_ICON_PICTURE, 10, 6, UI_COL_LINK);
    ui_text_bold(UI_SIZE_TAB, UI_COL_TEXT, 28.0f, 16.0f, UI_PREV_TITLE);

    if (has_img) {
        ui_prev_draw_image(pv, potw, poth);
    } else {
        w = ui_measure(UI_SIZE_MSG, UI_PREV_NO_IMAGE);
        ui_text(UI_SIZE_MSG, UI_COL_TEXT, ((float)UI_SCR_W - w) * 0.5f,
                (float)(UI_PREV_AREA_Y + UI_PREV_AREA_H / 2 - 4),
                UI_PREV_NO_IMAGE);
        w = ui_measure(UI_SIZE_NAME, UI_PREV_NO_IMAGE_SUB);
        ui_text(UI_SIZE_NAME, UI_COL_MED, ((float)UI_SCR_W - w) * 0.5f,
                (float)(UI_PREV_AREA_Y + UI_PREV_AREA_H / 2 + 18),
                UI_PREV_NO_IMAGE_SUB);
    }

    ui_text(UI_SIZE_NAME, UI_COL_TEXT, 10.0f,
            (float)(UI_PREV_INFO_Y1 + UI_BASELINE),
            ui_fit_tail(pv->path, UI_SIZE_NAME, UI_PREV_INFO_W,
                        pathbuf, sizeof pathbuf));

    if (pv->src_w > 0 && pv->src_h > 0) {
        snprintf(info, sizeof info, "%u KB   %d x %d px",
                 kb, pv->src_w, pv->src_h);
    } else {
        snprintf(info, sizeof info, "%u KB   %s", kb,
                 pspsky_tr(PSPSKY_STR_UI_PREV_DIM_UNKNOWN));
    }
    ui_text(UI_SIZE_NAME, UI_COL_MED, 10.0f,
            (float)(UI_PREV_INFO_Y2 + UI_BASELINE), info);

    ui_draw_footer(UI_FOOTER_Y, &post, NULL, UI_ICON_CROSS,
                   PSPSKY_STR_UI_KEY_RECHOOSE);

    ui_frame_end();
}

static void ui_build_atlas(void)
{
    int i;

    memset(s_atlas, 0, sizeof s_atlas);
    for (i = 0; i < UI_ATLAS_W * UI_ATLAS_ROWS / 2; i++) {
        unsigned int lo = (unsigned int)(k_ui_atlas_a4[i] & 0x0F) * 17u;
        unsigned int hi = (unsigned int)(k_ui_atlas_a4[i] >> 4) * 17u;

        s_atlas[2 * i]     = (lo << 24) | 0x00FFFFFFu;
        s_atlas[2 * i + 1] = (hi << 24) | 0x00FFFFFFu;
    }
    sceKernelDcacheWritebackRange(s_atlas, sizeof s_atlas);
}

/*
 * Top-left corner coverage, sampled at 4x4 subpixels per pixel and mirrored
 * into the other quadrants.
 */
static void ui_build_corner_mask(void)
{
    const int   e = 2 * UI_IMG_RADIUS - 1;
    const float r = (float)UI_IMG_RADIUS;
    int         x, y, i, j;

    for (y = 0; y < UI_IMG_RADIUS; y++) {
        for (x = 0; x < UI_IMG_RADIUS; x++) {
            unsigned int t;
            int          in = 0;

            for (j = 0; j < 4; j++) {
                for (i = 0; i < 4; i++) {
                    float px = r - ((float)x + ((float)i + 0.5f) / 4.0f);
                    float py = r - ((float)y + ((float)j + 0.5f) / 4.0f);

                    if (px * px + py * py <= r * r) {
                        in++;
                    }
                }
            }
            t = ((255u - (unsigned int)((in * 255 + 8) / 16)) << 24) |
                0x00FFFFFFu;
            s_corner_tex[y * UI_CORNER_TEX + x]           = t;
            s_corner_tex[y * UI_CORNER_TEX + e - x]       = t;
            s_corner_tex[(e - y) * UI_CORNER_TEX + x]     = t;
            s_corner_tex[(e - y) * UI_CORNER_TEX + e - x] = t;
        }
    }
    sceKernelDcacheWritebackRange(s_corner_tex, sizeof s_corner_tex);
}

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
    /*
     * intraFont re-enables depth testing; the GE default GU_NEVER would
     * discard later draws.
     */
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
    /* A 512x512 glyph cache avoids eviction on kanji-heavy screens. */
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

    ui_build_atlas();
    ui_build_corner_mask();
    s_digit_w = ui_measure(UI_SIZE_NAME, "0");

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    for (i = 0; i < UI_AV_SLOTS; i++) {
        s_av_key[i] = NULL;
    }
    s_prev_buttons = 0;
    s_scroll       = 0;
    s_shell        = 0;
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
    ui_draw_status(msg_utf8);
    ui_frame_end();
}

void pspsky_ui_message(const char *msg_utf8)
{
    static const ui_button_t ok[] = {
        { UI_ICON_CIRCLE, PSPSKY_STR_UI_KEY_OK, 1 }
    };
    SceCtrlData pad;
    int         first = 1;

    if (!s_inited) {
        return;
    }
    for (;;) {
        unsigned int pressed;

        ui_frame_begin();
        ui_draw_dialog(msg_utf8, 0, ok, 1);
        ui_frame_end();

        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        if (first) {
            /* Ignore buttons held when entering the dialog. */
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
    static const ui_button_t yes_no[] = {
        { UI_ICON_CIRCLE, PSPSKY_STR_UI_KEY_YES, 1 },
        { UI_ICON_CROSS,  PSPSKY_STR_UI_KEY_NO,  0 }
    };
    SceCtrlData pad;
    int         first = 1;

    if (!s_inited) {
        return 0;
    }
    for (;;) {
        unsigned int pressed;

        ui_frame_begin();
        ui_draw_dialog(msg_utf8, 0, yes_no, 2);
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

pspsky_ui_compose_choice_t pspsky_ui_compose_confirm(
    const char *msg_utf8, const char *const *image_paths, int image_count)
{
    SceCtrlData pad;
    int         first = 1;

    if (!s_inited) {
        return PSPSKY_UI_COMPOSE_CANCEL;
    }
    if (image_count < 0) {
        image_count = 0;
    } else if (image_count > PSPSKY_MAX_IMAGES) {
        image_count = PSPSKY_MAX_IMAGES;
    }
    for (;;) {
        unsigned int pressed;

        ui_frame_begin();
        ui_draw_compose_dialog(msg_utf8, image_paths, image_count);
        ui_frame_end();

        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        if (first) {
            /* Ignore buttons held when entering the dialog. */
            s_prev_buttons = pad.Buttons;
            first = 0;
            continue;
        }
        pressed        = pad.Buttons & ~s_prev_buttons;
        s_prev_buttons = pad.Buttons;
        if (pressed & PSP_CTRL_CIRCLE) {
            return PSPSKY_UI_COMPOSE_POST;
        }
        if ((pressed & PSP_CTRL_SQUARE) && image_count < PSPSKY_MAX_IMAGES) {
            return PSPSKY_UI_COMPOSE_POST_WITH_IMAGE;
        }
        if ((pressed & PSP_CTRL_TRIANGLE) && image_count > 0) {
            return PSPSKY_UI_COMPOSE_REMOVE_IMAGE;
        }
        if (pressed & PSP_CTRL_CROSS) {
            return PSPSKY_UI_COMPOSE_CANCEL;
        }
    }
}

int pspsky_ui_pick_jpeg(const char *start_dir, char *out_path, size_t out_cap)
{
    SceCtrlData  pad;
    unsigned int hold_dir    = 0;
    int          hold_frames = 0;
    int          cursor      = 0;
    int          top         = 0;
    int          first       = 1;

    if (out_path == NULL || out_cap == 0) {
        return PSPSKY_E_ARG;
    }
    out_path[0] = '\0';
    if (!s_inited) {
        return PSPSKY_E_ARG;
    }

    if (start_dir == NULL || start_dir[0] == '\0' ||
        ui_pick_set_dir(start_dir) != 0 || ui_pick_load() != 0) {
        if (ui_pick_set_dir("ms0:/") != 0 || ui_pick_load() != 0) {
            return PSPSKY_E_IO;
        }
    }

    for (;;) {
        unsigned int buttons, pressed;
        int          step = 0;

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
                    cursor = 0;
                    top    = 0;
                }
                first       = 1;
                hold_dir    = 0;
                hold_frames = 0;
                continue;
            }

            if (ui_pick_join(s_pick_path, sizeof s_pick_path, e->name, 0) != 0 ||
                strlen(s_pick_path) + 1 > out_cap) {
                pspsky_ui_message(pspsky_tr(PSPSKY_STR_UI_MSG_PATH_TOO_LONG));
                first = 1;
                continue;
            }
            snprintf(out_path, out_cap, "%s", s_pick_path);
            return PSPSKY_OK;
        }

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
    int         potw    = UI_TEX_MIN_DIM;
    int         poth    = UI_TEX_MIN_DIM;

    if (!s_inited || pv == NULL || pv->path == NULL) {
        return 0;
    }

    if (pv->rgb != NULL &&
        pv->thumb_w >= 1 && pv->thumb_w <= UI_TEX_MAX_DIM &&
        pv->thumb_h >= 1 && pv->thumb_h <= UI_TEX_MAX_DIM) {
        potw    = ui_tex_pow2(pv->thumb_w);
        poth    = ui_tex_pow2(pv->thumb_h);
        has_img = 1;
        ui_prev_upload(pv, potw, poth);
    }

    for (;;) {
        unsigned int pressed;

        ui_prev_draw(pv, has_img, potw, poth);

        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        if (first) {
            /* Ignore buttons held when entering the preview. */
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

void pspsky_ui_tl_image_max(int count, int *max_w, int *max_h)
{
    *max_w = (count > 1) ? PSPSKY_UI_TL_GRID_COL_W : PSPSKY_UI_TL_IMG_MAX_W;
    *max_h = (count > 2) ? PSPSKY_UI_TL_GRID_ROW_H : PSPSKY_UI_TL_IMG_MAX_H;
}

size_t pspsky_ui_tl_image_bytes(int w, int h)
{
    return (size_t)ui_tl_stride(w) * (size_t)h * sizeof(unsigned short);
}

/* The GE samples the result in place, so thumbnails need no upload slots. */
int pspsky_ui_tl_image_make(pspsky_ui_tl_image_t *out,
                            const unsigned char *rgb, int w, int h)
{
    unsigned short *tex;
    size_t          bytes;
    int             stride, x, y;

    if (out == NULL || rgb == NULL ||
        w < 1 || w > PSPSKY_UI_TL_IMG_MAX_W ||
        h < 1 || h > PSPSKY_UI_TL_IMG_MAX_H) {
        return PSPSKY_E_ARG;
    }
    stride = ui_tl_stride(w);
    bytes  = pspsky_ui_tl_image_bytes(w, h);
    /* The GE requires 16-byte aligned texture addresses. */
    tex = (unsigned short *)memalign(16, bytes);
    if (tex == NULL) {
        return PSPSKY_E_NOMEM;
    }
    for (y = 0; y < h; y++) {
        const unsigned char *src = rgb + (size_t)y * (size_t)w * 3u;
        const unsigned char *d   = k_ui_dither[y & 3];
        unsigned short      *dst = tex + (size_t)y * (size_t)stride;

        for (x = 0; x < w; x++) {
            /* Offsets cover one quantization step, preserving the mean. */
            unsigned int t = d[x & 3];
            unsigned int r = src[x * 3 + 0] + (t >> 1);
            unsigned int g = src[x * 3 + 1] + (t >> 2);
            unsigned int b = src[x * 3 + 2] + (t >> 1);

            r = (r > 255u) ? 255u : r;
            g = (g > 255u) ? 255u : g;
            b = (b > 255u) ? 255u : b;
            /* GU_PSM_5650 stores red in the low bits. */
            dst[x] = (unsigned short)(((b >> 3) << 11) | ((g >> 2) << 5) |
                                      (r >> 3));
        }
    }
    sceKernelDcacheWritebackRange(tex, (unsigned int)bytes);
    out->tex = tex;
    out->w   = (short)w;
    out->h   = (short)h;
    return PSPSKY_OK;
}

void pspsky_ui_timeline_invalidate(int reset_scroll)
{
    int i;

    s_layout_valid = 0;
    for (i = 0; i < UI_AV_SLOTS; i++) {
        s_av_key[i] = NULL;
    }
    if (reset_scroll) {
        s_scroll = 0;
        s_shell  = 0;
    }
}

pspsky_ui_action_t pspsky_ui_timeline_frame(const pspsky_ui_timeline_view_t *view)
{
    pspsky_ui_action_t act = PSPSKY_UI_ACT_NONE;
    SceCtrlData        pad;
    unsigned int       buttons, pressed;
    int                max_scroll, at_bottom, prev_scroll, step, tilt;
    int                header_y, footer_y;
    int                n, i;

    if (!s_inited || !view) {
        return PSPSKY_UI_ACT_NONE;
    }

    n = ui_clamp_count(view->count);
    if (!s_layout_valid || s_layout_count != n ||
        s_layout_more != (view->has_more != 0)) {
        ui_rebuild_layout(view);
    }

    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(&pad, 1);
    buttons        = pad.Buttons;
    pressed        = buttons & ~s_prev_buttons;
    s_prev_buttons = buttons;

    max_scroll  = ui_max_scroll();
    prev_scroll = s_scroll;
    if (buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN)) {
        s_hold++;
    } else {
        s_hold = 0;
    }
    step = (s_hold > UI_SCROLL_HOLD) ? UI_SCROLL_FAST : UI_SCROLL_STEP;
    if (buttons & PSP_CTRL_UP) {
        s_scroll -= step;
    }
    if (buttons & PSP_CTRL_DOWN) {
        s_scroll += step;
    }
    tilt = (int)pad.Ly - 128;
    if (tilt > UI_ANALOG_DEAD) {
        s_scroll += 1 + (tilt - UI_ANALOG_DEAD) * (UI_ANALOG_MAX - 1) /
                        (127 - UI_ANALOG_DEAD);
    } else if (tilt < -UI_ANALOG_DEAD) {
        s_scroll -= 1 + (-tilt - UI_ANALOG_DEAD) * (UI_ANALOG_MAX - 1) /
                        (128 - UI_ANALOG_DEAD);
    }
    if (s_scroll > max_scroll) {
        s_scroll = max_scroll;
    }
    if (s_scroll < 0) {
        s_scroll = 0;
    }
    at_bottom = (s_scroll >= max_scroll);

    s_shell += s_scroll - prev_scroll;
    if (s_shell > UI_HEADER_H) {
        s_shell = UI_HEADER_H;
    }
    if (s_shell < 0 || s_scroll == 0) {
        s_shell = 0;
    }

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

    ui_frame_begin();

    header_y     = -s_shell;
    footer_y     = UI_FOOTER_Y + s_shell * UI_FOOTER_H / UI_HEADER_H;
    s_vis_top    = header_y + UI_HEADER_H;
    s_vis_bottom = footer_y;

    if (n == 0 || view->posts == NULL) {
        const char *empty = pspsky_tr(PSPSKY_STR_UI_TL_EMPTY);
        float       w     = ui_measure(UI_SIZE_MSG, empty);

        ui_text(UI_SIZE_MSG, UI_COL_MED, ((float)UI_SCR_W - w) * 0.5f,
                (float)(UI_SCR_H / 2), empty);
    } else {
        for (i = 0; i < n; i++) {
            int top    = UI_PAD_TOP + s_post_top[i] - s_scroll;
            int bottom = UI_PAD_TOP + s_post_top[i + 1] - s_scroll;

            if (bottom <= s_vis_top) {
                continue;
            }
            if (top >= s_vis_bottom) {
                break;
            }
            /* intraFont may change the scissor. */
            sceGuScissor(0, s_vis_top, UI_SCR_W, s_vis_bottom - s_vis_top);
            ui_draw_post(&view->posts[i], i, top, view->now_unix,
                         ui_tl_images(view, i), ui_avatar(view, i));
        }
        if (s_layout_more) {
            int y = UI_PAD_TOP + s_post_top[n] - s_scroll;

            if (ui_visible(y, UI_END_H)) {
                sceGuScissor(0, s_vis_top, UI_SCR_W, s_vis_bottom - s_vis_top);
                ui_draw_end_row(y);
            }
        }
        sceGuScissor(0, 0, UI_SCR_W, UI_SCR_H);
        ui_draw_scrollbar();
    }

    sceGuScissor(0, 0, UI_SCR_W, UI_SCR_H);
    if (s_vis_top > 0) {
        ui_draw_tl_header(header_y, view->my_handle);
    }
    if (footer_y < UI_SCR_H) {
        ui_draw_tl_footer(footer_y, view->has_more);
    }

    ui_frame_end();

    return act;
}
