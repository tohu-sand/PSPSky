/* Network calls block; draw the status before starting a request. */

#include <pspkernel.h>
#include <psprtc.h>
#include <pspsysmem.h>
#include <psputility.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mbedtls/x509_crt.h>

#include "pspsky/error.h"
#include "pspsky/atproto.h"
#include "pspsky/jpeg.h"
#include "pspsky/png.h"
#include "pspsky/webp.h"
#include "pspsky/transport.h"
#include "pspsky/transport_curl.h"
#include "pspsky/config.h"
#include "pspsky/net.h"
#include "pspsky/osk.h"
#include "pspsky/strings.h"
#include "pspsky/ui.h"
#include "pspsky/utf8.h"
#include "pspsky/timeutil.h"

PSP_MODULE_INFO("PSPSky", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-2048);

#define APP_MAX_POSTS       100
#define APP_TIMELINE_LIMIT  10
#define APP_POST_MAX_CHARS  300
#define APP_LOGIN_TRIES     3
#define APP_NET_CONF_INDEX  1
#define APP_NET_TIMEOUT_SEC 45

/* Thumbnails beyond this heap budget remain placeholders. */
#define APP_IMG_CACHE_MAX   (4 * 1024 * 1024)

/* Avatars survive timeline refreshes and are shared by author URL. */
#define APP_AVATAR_MAX      128
#define APP_AVATAR_BYTES    (PSPSKY_UI_AVATAR_PX * PSPSKY_UI_AVATAR_PX * 3)

#define APP_DEFAULT_BASEDIR "ms0:/PSP/GAME/PSPSKY/"

#define APP_BASEDIR_MAX     256
#define APP_PATH_MAX        (APP_BASEDIR_MAX + 32)
/* The UCS-2 OSK needs up to 3 UTF-8 bytes per character. */
#define APP_CRED_CHARS      128
#define APP_CRED_BUF        (APP_CRED_CHARS * 3 + 8)
#define APP_TEXT_BUF        (APP_POST_MAX_CHARS * 3 + 8)

#define APP_COMPOSE_BACK    1

/* Keep large buffers off the PSP stack. */

static pspsky_post_t      g_posts[APP_MAX_POSTS];
static int                g_post_count;
static char               g_cursor[PSPSKY_MAX_CURSOR];
static int                g_has_more;

/* Owns thumbnail RGB buffers; the UI borrows them. */
static pspsky_ui_tl_image_t g_tl_images[APP_MAX_POSTS][PSPSKY_MAX_IMAGES];
static size_t               g_img_cache_bytes;
/* Shared decode scratch for thumbnails and avatars. */
static unsigned char g_img_work[PSPSKY_UI_TL_IMG_MAX_W *
                                PSPSKY_UI_TL_IMG_MAX_H * 3];

/* Owns RGB buffers. NULL marks a failed fetch until the next refresh. */
typedef struct {
    char           url[PSPSKY_MAX_THUMB_URL];
    unsigned char *rgb;
} app_avatar_t;

static app_avatar_t       g_avatars[APP_AVATAR_MAX];
static int                g_avatar_count;
/* Borrows avatar buffers from g_avatars, indexed like g_posts. */
static pspsky_ui_avatar_t g_post_avatars[APP_MAX_POSTS];

static pspsky_client_t    g_client;
static pspsky_timeline_t  g_tl;
static pspsky_transport_t g_transport;
static pspsky_config_t    g_cfg;
static int                g_cfg_rc = PSPSKY_E_IO;

static char g_basedir[APP_BASEDIR_MAX];
static char g_cacert_path[APP_PATH_MAX];
static char g_cfg_path[APP_PATH_MAX];

#define CA_DIAG_SIZE_MAX (1L << 20)

/* Stage not reached; distinct from fseek success and failure. */
#define CA_DIAG_NA (-9)

typedef struct {
    int  open_ok;
    int  seek_end_rc;  /* CA_DIAG_NA if skipped. */
    long size;         /* -1 if unavailable. */
    int  seek_set_rc;  /* CA_DIAG_NA if skipped. */
    int  alloc_ok;
    long read_len;     /* -1 if skipped. */
    int  begin_ok;     /* PEM header found. */
    int  parse_rc;     /* mbedtls_x509_crt_parse_file rc */
    int  cert_count;
    unsigned int free_kb;
} cacert_diag_t;

static cacert_diag_t g_ca;
static char          g_ca_line[224];

static volatile int g_exit_requested;

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    g_exit_requested = 1;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    int cbid;

    (void)args;
    (void)argp;
    cbid = sceKernelCreateCallback("PSPSky Exit Callback", exit_callback, NULL);
    if (cbid >= 0) {
        sceKernelRegisterExitCallback(cbid);
    }
    sceKernelSleepThreadCB();
    return 0;
}

static int setup_callbacks(void)
{
    int thid;

    thid = sceKernelCreateThread("pspsky_cb", callback_thread, 0x11, 0xFA0,
                                 PSP_THREAD_ATTR_USER, NULL);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, NULL);
    }
    return thid;
}

/* A failed parse can leave an empty head node with version == 0. */
static int crt_chain_len(const mbedtls_x509_crt *crt)
{
    const mbedtls_x509_crt *p;
    int n = 0;

    for (p = crt; p != NULL && p->version != 0; p = p->next) {
        n++;
    }
    return n;
}

static void fmt_mbedtls_rc(char *out, size_t n, int rc)
{
    if (rc < 0) {
        snprintf(out, n, "-0x%04X", (unsigned int)(-rc));
    } else {
        snprintf(out, n, "%d", rc);
    }
}

/* Check file I/O separately to distinguish read failures from parse errors. */
static void cacert_selftest(void)
{
    static mbedtls_x509_crt crt;
    char  rcbuf[16];
    FILE *fp;

    memset(&g_ca, 0, sizeof g_ca);
    g_ca.seek_end_rc = CA_DIAG_NA;
    g_ca.seek_set_rc = CA_DIAG_NA;
    g_ca.size        = -1;
    g_ca.read_len    = -1;

    fp = fopen(g_cacert_path, "rb");
    if (fp != NULL) {
        g_ca.open_ok     = 1;
        g_ca.seek_end_rc = fseek(fp, 0, SEEK_END);
        g_ca.size        = ftell(fp);
        g_ca.seek_set_rc = fseek(fp, 0, SEEK_SET);

        if (g_ca.size >= 0 && g_ca.size <= CA_DIAG_SIZE_MAX) {
            char *buf = (char *)calloc(1, (size_t)g_ca.size + 1);

            if (buf != NULL) {
                size_t got = fread(buf, 1, (size_t)g_ca.size, fp);

                g_ca.alloc_ok = 1;
                g_ca.read_len = (long)got;
                buf[got]      = '\0';
                g_ca.begin_ok = (strstr(buf, "-----BEGIN ") != NULL) ? 1 : 0;
                free(buf);
            }
        }
        fclose(fp);
    }

    mbedtls_x509_crt_init(&crt);
    g_ca.parse_rc   = mbedtls_x509_crt_parse_file(&crt, g_cacert_path);
    g_ca.cert_count = crt_chain_len(&crt);
    mbedtls_x509_crt_free(&crt);

    g_ca.free_kb = (unsigned int)(sceKernelTotalFreeMemSize() / 1024u);

    fmt_mbedtls_rc(rcbuf, sizeof rcbuf, g_ca.parse_rc);
    snprintf(g_ca_line, sizeof g_ca_line,
             "open=%s seek=%d size=%ld seek2=%d alloc=%s read=%ld "
             "begin=%s parse=%s certs=%d free=%uKB",
             g_ca.open_ok ? "OK" : "NG", g_ca.seek_end_rc, g_ca.size,
             g_ca.seek_set_rc, g_ca.alloc_ok ? "OK" : "NG", g_ca.read_len,
             g_ca.begin_ok ? "OK" : "NG", rcbuf, g_ca.cert_count,
             g_ca.free_kb);
}

static int cacert_diag_ok(void)
{
    return (g_ca.open_ok &&
            g_ca.seek_end_rc == 0 &&
            g_ca.size > 0 &&
            g_ca.seek_set_rc == 0 &&
            g_ca.alloc_ok &&
            g_ca.read_len == g_ca.size &&
            g_ca.begin_ok &&
            g_ca.parse_rc == 0 &&
            g_ca.cert_count > 0);
}

static void resolve_paths(int argc, char *argv[])
{
    g_basedir[0] = '\0';

    if (argc > 0 && argv != NULL && argv[0] != NULL) {
        const char *slash = strrchr(argv[0], '/');
        if (slash != NULL) {
            size_t n = (size_t)(slash - argv[0]) + 1;
            if (n > 0 && n < sizeof(g_basedir)) {
                memcpy(g_basedir, argv[0], n);
                g_basedir[n] = '\0';
            }
        }
    }
    if (g_basedir[0] == '\0') {
        snprintf(g_basedir, sizeof(g_basedir), "%s", APP_DEFAULT_BASEDIR);
    }

    snprintf(g_cacert_path, sizeof(g_cacert_path), "%scacert.pem", g_basedir);
    snprintf(g_cfg_path, sizeof(g_cfg_path), "%spspsky.cfg", g_basedir);
    printf("PSPSky: basedir=%s\n", g_basedir);

    /* Runs before UI init, so only record the result here. */
    cacert_selftest();
    printf("PSPSky: %s\n", g_ca_line);
}

/* Load once before any UI text; lang= overrides the system language. */
static void init_language(void)
{
    int syslang = -1;

    if (sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,
                                    &syslang) == 0 &&
        syslang == PSP_SYSTEMPARAM_LANGUAGE_JAPANESE) {
        pspsky_lang_set(PSPSKY_LANG_JA);
    } else {
        pspsky_lang_set(PSPSKY_LANG_EN);
    }

    g_cfg_rc = pspsky_config_load(g_cfg_path, &g_cfg);
    if (strcmp(g_cfg.lang, "ja") == 0) {
        pspsky_lang_set(PSPSKY_LANG_JA);
    } else if (strcmp(g_cfg.lang, "en") == 0) {
        pspsky_lang_set(PSPSKY_LANG_EN);
    }
}

static void report_cacert_diag(void)
{
    static char buf[APP_PATH_MAX + sizeof g_ca_line + 128];

    if (cacert_diag_ok()) {
        return;
    }
    snprintf(buf, sizeof(buf),
             "%s\n%s%s\n%s",
             g_ca.open_ok ? pspsky_tr(PSPSKY_STR_CACERT_UNREADABLE)
                          : pspsky_tr(PSPSKY_STR_CACERT_NOT_FOUND),
             pspsky_tr(PSPSKY_STR_CA_DIAG_PREFIX), g_ca_line, g_cacert_path);
    pspsky_ui_message(buf);
}

static long long now_unix(void)
{
    ScePspDateTime pt;

    memset(&pt, 0, sizeof(pt));
    if (sceRtcGetCurrentClock(&pt, 0) < 0) {
        return 0;
    }
    return pspsky_civil_to_unix((int)pt.year, (int)pt.month, (int)pt.day,
                                (int)pt.hour, (int)pt.minute, (int)pt.second);
}

/*
 * PSP newlib time() is not UTC, which breaks TLS certificate date checks.
 * The linker wraps time() to use the RTC, falling back if it is unavailable.
 */
extern time_t __real_time(time_t *t);

time_t __wrap_time(time_t *t)
{
    long long now = now_unix();
    time_t    v;

    if (now <= 0) {
        return __real_time(t);
    }
    v = (time_t)now;
    if (t != NULL) {
        *t = v;
    }
    return v;
}

static void net_progress(const char *msg_utf8, void *user)
{
    (void)user;
    pspsky_ui_status(msg_utf8);
}

/* Prefer the transport's TLS diagnosis over generic clock advice. */
static void show_error(int err)
{
    char buf[PSPSKY_ERRMSG_MAX + 128];

    if (err == PSPSKY_E_TLS && g_client.last_error_msg[0] != '\0') {
        pspsky_ui_message(g_client.last_error_msg);
        return;
    }

    if (g_client.last_error_msg[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s\n(%s)",
                 pspsky_strerror(err), g_client.last_error_msg);
    } else {
        snprintf(buf, sizeof(buf), "%s", pspsky_strerror(err));
    }
    pspsky_ui_message(buf);
}

/* Invalidate UI textures before freeing the RGB buffers they reference. */
static void img_cache_clear(void)
{
    int i, j;

    pspsky_ui_timeline_invalidate(0);
    for (i = 0; i < APP_MAX_POSTS; i++) {
        for (j = 0; j < PSPSKY_MAX_IMAGES; j++) {
            free((void *)g_tl_images[i][j].rgb);
        }
    }
    memset(g_tl_images, 0, sizeof(g_tl_images));
    memset(g_post_avatars, 0, sizeof(g_post_avatars));
    g_img_cache_bytes = 0;
}

/* Call only after detaching posts from the avatar cache. */
static void avatar_cache_clear(void)
{
    int i;

    pspsky_ui_timeline_invalidate(0);
    for (i = 0; i < g_avatar_count; i++) {
        free(g_avatars[i].rgb);
    }
    memset(g_avatars, 0, sizeof(g_avatars));
    memset(g_post_avatars, 0, sizeof(g_post_avatars));
    g_avatar_count = 0;
}

/* Prune only after detaching posts; removing failed entries allows retries. */
static void avatar_cache_prune(void)
{
    int i;
    int kept = 0;

    if (g_avatar_count > APP_AVATAR_MAX * 3 / 4) {
        avatar_cache_clear();
        return;
    }
    for (i = 0; i < g_avatar_count; i++) {
        if (g_avatars[i].rgb != NULL) {
            g_avatars[kept++] = g_avatars[i];
        }
    }
    g_avatar_count = kept;
}

static int avatar_find(const char *url)
{
    int i;

    for (i = 0; i < g_avatar_count; i++) {
        if (strcmp(g_avatars[i].url, url) == 0) {
            return i;
        }
    }
    return -1;
}

/* Count each uncached URL once within the batch. */
static int avatar_pending(int start, int i)
{
    const char *url = g_posts[i].avatar_url;
    int         j;

    if (url[0] == '\0' || avatar_find(url) >= 0) {
        return 0;
    }
    for (j = start; j < i; j++) {
        if (strcmp(g_posts[j].avatar_url, url) == 0) {
            return 0;
        }
    }
    return 1;
}

/* Decode WebP or JPEG to a PSPSKY_UI_AVATAR_PX square in RGB888. */
static int avatar_decode(const unsigned char *data, size_t len,
                         unsigned char *out)
{
    int tw = 0;
    int th = 0;
    int sw = 0;
    int sh = 0;
    int rc;
    int x, y;

    rc = pspsky_webp_thumb_decode(data, len,
                                  PSPSKY_UI_AVATAR_PX, PSPSKY_UI_AVATAR_PX,
                                  g_img_work, sizeof(g_img_work),
                                  &tw, &th, &sw, &sh);
    if (rc != PSPSKY_OK) {
        const char *mime = pspsky_image_sniff_mime(data, len);

        if (mime == NULL || strcmp(mime, "image/jpeg") != 0) {
            return rc;
        }
        rc = pspsky_jpeg_thumb_decode(data, len, g_img_work, sizeof(g_img_work),
                                      &tw, &th, &sw, &sh);
        if (rc != PSPSKY_OK) {
            return rc;
        }
    }
    if (tw < 1 || th < 1) {
        return PSPSKY_E_PARSE;
    }
    for (y = 0; y < PSPSKY_UI_AVATAR_PX; y++) {
        const unsigned char *src = g_img_work +
            (size_t)(y * th / PSPSKY_UI_AVATAR_PX) * (size_t)tw * 3u;

        for (x = 0; x < PSPSKY_UI_AVATAR_PX; x++) {
            memcpy(out + ((size_t)y * PSPSKY_UI_AVATAR_PX + (size_t)x) * 3u,
                   src + (size_t)(x * tw / PSPSKY_UI_AVATAR_PX) * 3u, 3);
        }
    }
    return PSPSKY_OK;
}

/* Cache failures too, so posts by the same author do not repeat the request. */
static int avatar_add(const char *url)
{
    app_avatar_t  *a;
    unsigned char *body = NULL;
    size_t         len  = 0;

    if (g_avatar_count >= APP_AVATAR_MAX) {
        return -1;
    }
    a = &g_avatars[g_avatar_count];
    snprintf(a->url, sizeof(a->url), "%s", url);
    a->rgb = NULL;

    if (pspsky_atp_fetch_image(&g_transport, url, &body, &len, NULL) == PSPSKY_OK) {
        unsigned char *rgb = (unsigned char *)malloc(APP_AVATAR_BYTES);

        if (rgb != NULL && avatar_decode(body, len, rgb) == PSPSKY_OK) {
            a->rgb = rgb;
        } else {
            free(rgb);
        }
    }
    free(body);
    if (a->rgb == NULL) {
        printf("PSPSky: avatar unavailable: %s\n", url);
    }
    return g_avatar_count++;
}

/* Failed image requests leave placeholders without interrupting the timeline. */
static void fetch_new_images(int start)
{
    char msg[128];
    int  i, j;
    int  m = 0;
    int  n = 0;
    int  room = APP_AVATAR_MAX - g_avatar_count;

    if (!g_cfg.show_images) {
        return;
    }
    for (i = start; i < g_post_count; i++) {
        if (room > 0 && avatar_pending(start, i)) {
            room--;
            m++;
        }
        for (j = 0; j < g_posts[i].img_count && j < PSPSKY_MAX_IMAGES; j++) {
            if (g_posts[i].images[j].thumb_url[0] != '\0') {
                m++;
            }
        }
    }

    /* Fetch small avatars before larger attachments. */
    for (i = start; i < g_post_count; i++) {
        int k;

        if (g_posts[i].avatar_url[0] == '\0') {
            continue;
        }
        k = avatar_find(g_posts[i].avatar_url);
        if (k < 0 && g_avatar_count < APP_AVATAR_MAX) {
            n++;
            snprintf(msg, sizeof(msg), pspsky_tr(PSPSKY_STR_ST_FETCHING_IMAGES),
                     n, m);
            pspsky_ui_status(msg);
            k = avatar_add(g_posts[i].avatar_url);
        }
        if (k >= 0) {
            g_post_avatars[i].rgb = g_avatars[k].rgb;
        }
    }

    for (i = start; i < g_post_count; i++) {
        int max_w = (g_posts[i].img_count > 1) ? PSPSKY_UI_TL_GRID_COL_W
                                                : PSPSKY_UI_TL_IMG_MAX_W;

        for (j = 0; j < g_posts[i].img_count && j < PSPSKY_MAX_IMAGES; j++) {
            unsigned char *body = NULL;
            unsigned char *rgb;
            size_t         len  = 0;
            size_t         sz;
            int            tw   = 0;
            int            th   = 0;
            int            sw   = 0;
            int            sh   = 0;
            int            decoded;

            if (g_posts[i].images[j].thumb_url[0] == '\0') {
                continue;
            }
            n++;
            /* Skip transfers once the decoded image cache is full. */
            if (g_img_cache_bytes >= (size_t)APP_IMG_CACHE_MAX) {
                continue;
            }

            snprintf(msg, sizeof(msg), pspsky_tr(PSPSKY_STR_ST_FETCHING_IMAGES),
                     n, m);
            pspsky_ui_status(msg);

            decoded = 0;
            if (pspsky_atp_fetch_image(&g_transport,
                                       g_posts[i].images[j].thumb_url,
                                       &body, &len, NULL) == PSPSKY_OK &&
                pspsky_webp_thumb_decode(body, len, max_w,
                                         PSPSKY_UI_TL_IMG_MAX_H,
                                         g_img_work, sizeof(g_img_work),
                                         &tw, &th, &sw, &sh) == PSPSKY_OK) {
                decoded = 1;
            }
            free(body);
            if (!decoded) {
                printf("PSPSky: thumbnail %d/%d unavailable\n", i, j);
                continue;
            }

            sz = (size_t)tw * (size_t)th * 3u;
            if (g_img_cache_bytes + sz > (size_t)APP_IMG_CACHE_MAX) {
                continue;
            }
            rgb = (unsigned char *)malloc(sz);
            if (rgb == NULL) {
                continue;
            }
            memcpy(rgb, g_img_work, sz);
            g_tl_images[i][j].rgb = rgb;
            g_tl_images[i][j].w   = (short)tw;
            g_tl_images[i][j].h   = (short)th;
            g_img_cache_bytes += sz;
        }
    }
}

static void timeline_clear(void)
{
    g_post_count = 0;
    g_cursor[0]  = '\0';
    g_has_more   = 0;
}

static void timeline_append(const pspsky_timeline_t *tl)
{
    int i;

    for (i = 0; i < tl->count && g_post_count < APP_MAX_POSTS; i++) {
        g_posts[g_post_count++] = tl->posts[i];
    }
    snprintf(g_cursor, sizeof(g_cursor), "%s", tl->cursor);
    g_has_more = (g_cursor[0] != '\0' && g_post_count < APP_MAX_POSTS) ? 1 : 0;
}

static int timeline_reload(void)
{
    int rc;

    pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_TIMELINE_LOADING));
    rc = pspsky_client_get_timeline(&g_client, APP_TIMELINE_LIMIT, NULL, &g_tl);
    if (rc != PSPSKY_OK) {
        return rc;
    }
    img_cache_clear();
    avatar_cache_prune();
    timeline_clear();
    timeline_append(&g_tl);
    fetch_new_images(0);
    pspsky_ui_timeline_invalidate(1);
    return PSPSKY_OK;
}

static int timeline_load_older(void)
{
    int rc;

    if (g_cursor[0] == '\0' || g_post_count >= APP_MAX_POSTS) {
        g_has_more = 0;
        return PSPSKY_OK;
    }
    pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_LOADING_MORE));
    rc = pspsky_client_get_timeline(&g_client, APP_TIMELINE_LIMIT, g_cursor, &g_tl);
    if (rc != PSPSKY_OK) {
        return rc;
    }
    {
        int added_from = g_post_count;

        timeline_append(&g_tl);
        fetch_new_images(added_from);
    }
    pspsky_ui_timeline_invalidate(0);
    return PSPSKY_OK;
}

static int prompt_credentials(pspsky_config_t *cfg)
{
    static char buf[APP_CRED_BUF];
    int rc;

    for (;;) {
        rc = pspsky_osk_input(pspsky_tr(PSPSKY_STR_OSK_HANDLE),
                              cfg->identifier, APP_CRED_CHARS,
                              buf, sizeof(buf));
        if (rc != PSPSKY_OK) {
            return rc;
        }
        if (buf[0] == '\0') {
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_HANDLE_EMPTY));
            continue;
        }
        if (strlen(buf) >= sizeof(cfg->identifier)) {
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_HANDLE_TOO_LONG));
            continue;
        }
        break;
    }
    snprintf(cfg->identifier, sizeof(cfg->identifier), "%s", buf);

    for (;;) {
        rc = pspsky_osk_input(pspsky_tr(PSPSKY_STR_OSK_PASSWORD), "",
                              APP_CRED_CHARS, buf, sizeof(buf));
        if (rc != PSPSKY_OK) {
            return rc;
        }
        if (buf[0] == '\0') {
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_PASSWORD_EMPTY));
            continue;
        }
        if (strlen(buf) >= sizeof(cfg->app_password)) {
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_PASSWORD_TOO_LONG));
            continue;
        }
        break;
    }
    snprintf(cfg->app_password, sizeof(cfg->app_password), "%s", buf);

    memset(buf, 0, sizeof(buf));
    return PSPSKY_OK;
}

/* Save credentials only when entered through the OSK. */
static int app_login(int from_config)
{
    int attempt;
    int rc      = PSPSKY_E_AUTH;
    int entered = from_config ? 0 : 1;

    for (attempt = 1; attempt <= APP_LOGIN_TRIES; attempt++) {
        pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_LOGGING_IN));
        rc = pspsky_client_login(&g_client, &g_transport,
                                 g_cfg.identifier, g_cfg.app_password);
        if (rc == PSPSKY_OK) {
            if (entered) {
                if (pspsky_config_save(g_cfg_path, &g_cfg) != PSPSKY_OK) {
                    pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_CFG_SAVE_FAIL));
                }
            }
            return PSPSKY_OK;
        }

        show_error(rc);
        if (attempt >= APP_LOGIN_TRIES) {
            break;
        }

        if (rc == PSPSKY_E_AUTH) {
            int prc = prompt_credentials(&g_cfg);
            if (prc != PSPSKY_OK) {
                return prc;
            }
            entered = 1;
        } else {
            if (!pspsky_ui_confirm(pspsky_tr(PSPSKY_STR_CONFIRM_RETRY))) {
                return PSPSKY_E_CANCELLED;
            }
        }
    }
    return rc;
}

static int compose_now_iso(char *iso, size_t cap)
{
    if (pspsky_iso8601_format(now_unix(), iso, cap) != PSPSKY_OK) {
        pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_TIME_FAIL));
        return 0;
    }
    return 1;
}

static int compose_send(const char *text, const char *iso,
                        const pspsky_embed_image_t *images, int image_count)
{
    int rc;

    pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_POSTING));
    rc = pspsky_client_create_post_ex(&g_client, text, iso, images, image_count);
    if (rc != PSPSKY_OK) {
        return rc;
    }
    pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_POSTED));
    return timeline_reload();
}

/* Reject oversized files before allocating; the caller owns *out_buf. */
static int load_image_file(const char *path, unsigned char **out_buf,
                           size_t *out_len)
{
    FILE          *fp;
    long           size;
    unsigned char *buf;
    size_t         got;

    *out_buf = NULL;
    *out_len = 0;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return PSPSKY_E_IO;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return PSPSKY_E_IO;
    }
    size = ftell(fp);
    if (size <= 0) {
        fclose(fp);
        return PSPSKY_E_IO;
    }
    if (size > (long)PSPSKY_IMAGE_MAX_BYTES) {
        fclose(fp);
        return PSPSKY_E_ARG;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return PSPSKY_E_IO;
    }
    buf = (unsigned char *)malloc((size_t)size);
    if (buf == NULL) {
        fclose(fp);
        return PSPSKY_E_NOMEM;
    }
    got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        free(buf);
        return PSPSKY_E_IO;
    }

    *out_buf = buf;
    *out_len = got;
    return PSPSKY_OK;
}

/* MIME comes from the magic bytes, not the extension. Caller frees out_buf. */
static int pick_and_load_image(char *path_out, size_t path_cap,
                               unsigned char **out_buf, size_t *out_len,
                               const char **out_mime)
{
    int rc;

    for (;;) {
        rc = pspsky_ui_pick_jpeg("ms0:/PSP/PHOTO/", path_out, path_cap);
        if (rc == PSPSKY_E_CANCELLED) {
            return APP_COMPOSE_BACK;
        }
        if (rc == PSPSKY_E_IO) {
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_UI_MSG_DIR_OPEN_FAIL));
            return APP_COMPOSE_BACK;
        }
        if (rc != PSPSKY_OK) {
            pspsky_ui_message(pspsky_strerror(rc));
            return APP_COMPOSE_BACK;
        }

        rc = load_image_file(path_out, out_buf, out_len);
        if (rc == PSPSKY_OK) {
            const char *mime = pspsky_image_sniff_mime(*out_buf, *out_len);
            if (mime != NULL) {
                *out_mime = mime;
                return PSPSKY_OK;
            }
            free(*out_buf);
            *out_buf = NULL;
            *out_len = 0;
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_IMG_UNSUPPORTED));
            continue;
        }
        if (rc == PSPSKY_E_NOMEM) {
            pspsky_ui_message(pspsky_strerror(PSPSKY_E_NOMEM));
            return APP_COMPOSE_BACK;
        }
        if (rc == PSPSKY_E_ARG) {
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_IMG_TOO_LARGE));
        } else {
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_IMG_READ_FAIL));
        }
    }
}

/* Select one attachment without uploading; cancellation keeps the draft intact. */
static int app_compose_add_image(char *img_path, size_t path_cap,
                                 pspsky_embed_image_t *img)
{
    static char    alt_raw[APP_TEXT_BUF];
    unsigned char *buf  = NULL;
    size_t         len  = 0;
    const char    *mime = NULL;
    int            w    = 0;
    int            h    = 0;
    int            is_png;
    int            rc;

    for (;;) {
        unsigned char *thumb = NULL;
        int            tw    = 0;
        int            th    = 0;
        int            sw    = 0;
        int            sh    = 0;
        int            ok;

        if (pick_and_load_image(img_path, path_cap,
                                &buf, &len, &mime) != PSPSKY_OK) {
            return APP_COMPOSE_BACK;
        }
        is_png = (strcmp(mime, "image/png") == 0);

        /* Missing dimensions just omit aspectRatio; the server has final say. */
        if (is_png) {
            rc = pspsky_png_dimensions(buf, len, &w, &h);
        } else {
            rc = pspsky_jpeg_dimensions(buf, len, &w, &h);
        }
        if (rc != PSPSKY_OK) {
            w = 0;
            h = 0;
        }

        /* A preview failure does not prevent uploading the original image. */
        pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_PREVIEW_PREP));
        thumb = (unsigned char *)malloc(PSPSKY_JPEG_THUMB_MAX_RGB);
        if (thumb != NULL) {
            if (is_png) {
                rc = pspsky_png_thumb_decode(buf, len, thumb,
                                             PSPSKY_JPEG_THUMB_MAX_RGB,
                                             &tw, &th, &sw, &sh);
            } else {
                rc = pspsky_jpeg_thumb_decode(buf, len, thumb,
                                              PSPSKY_JPEG_THUMB_MAX_RGB,
                                              &tw, &th, &sw, &sh);
            }
            if (rc != PSPSKY_OK) {
                free(thumb);
                thumb = NULL;
                tw = 0;
                th = 0;
            }
        }

        /* The preview does not retain thumb after returning. */
        {
            pspsky_ui_preview_t pv = { img_path, len, w, h, thumb, tw, th };
            ok = pspsky_ui_preview_image(&pv);
        }
        free(thumb);
        thumb = NULL;

        if (ok) {
            break;
        }
        free(buf);
        buf = NULL;
    }

    free(buf);
    memset(img, 0, sizeof(*img));
    img->aspect_w = w;
    img->aspect_h = h;

    /* Collect alt text before upload: blobs can expire before createRecord. */
    alt_raw[0] = '\0';
    if (pspsky_osk_input(pspsky_tr(PSPSKY_STR_OSK_ALT), "",
                         APP_POST_MAX_CHARS,
                         alt_raw, sizeof(alt_raw)) == PSPSKY_OK) {
        pspsky_utf8_truncate(alt_raw, PSPSKY_MAX_ALT - 1);
        snprintf(img->alt, sizeof(img->alt), "%s", alt_raw);
    }
    return PSPSKY_OK;
}

/* Reopen each file at send time so four attachments need only one file buffer. */
static int app_compose_upload_images(const char *const *paths,
                                     pspsky_embed_image_t *images, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        unsigned char *buf = NULL;
        size_t         len = 0;
        const char    *mime;
        char           status[96];
        int            rc;

        rc = load_image_file(paths[i], &buf, &len);
        if (rc != PSPSKY_OK) {
            pspsky_ui_message(rc == PSPSKY_E_NOMEM ? pspsky_strerror(rc) :
                              pspsky_tr(rc == PSPSKY_E_ARG ?
                                PSPSKY_STR_MSG_IMG_TOO_LARGE :
                                PSPSKY_STR_MSG_IMG_READ_FAIL));
            return APP_COMPOSE_BACK;
        }
        mime = pspsky_image_sniff_mime(buf, len);
        if (mime == NULL) {
            free(buf);
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_IMG_UNSUPPORTED));
            return APP_COMPOSE_BACK;
        }

        /* Dimensions must describe the actual file if it changed since preview. */
        if (strcmp(mime, "image/png") == 0) {
            rc = pspsky_png_dimensions(buf, len,
                                       &images[i].aspect_w, &images[i].aspect_h);
        } else {
            rc = pspsky_jpeg_dimensions(buf, len,
                                        &images[i].aspect_w, &images[i].aspect_h);
        }
        if (rc != PSPSKY_OK) {
            images[i].aspect_w = 0;
            images[i].aspect_h = 0;
        }

        /* curl borrows buf through the upload and any authentication retry. */
        snprintf(status, sizeof(status), pspsky_tr(PSPSKY_STR_ST_UPLOADING),
                 i + 1, count);
        pspsky_ui_status(status);
        rc = pspsky_client_upload_blob(&g_client, buf, len, mime, &images[i].blob);
        free(buf);
        if (rc != PSPSKY_OK) {
            return rc;
        }
    }
    return PSPSKY_OK;
}

static int app_compose(void)
{
    static char text[APP_TEXT_BUF];
    static char image_paths[PSPSKY_MAX_IMAGES][PSPSKY_UI_PICK_PATH_MAX];
    static pspsky_embed_image_t images[PSPSKY_MAX_IMAGES];
    const char *paths[PSPSKY_MAX_IMAGES];
    char iso[32];
    int  image_count = 0;
    int  i;
    int  rc;

    memset(image_paths, 0, sizeof(image_paths));
    memset(images, 0, sizeof(images));
    for (i = 0; i < PSPSKY_MAX_IMAGES; i++) {
        paths[i] = image_paths[i];
    }

    rc = pspsky_osk_input(pspsky_tr(PSPSKY_STR_OSK_COMPOSE), "", APP_POST_MAX_CHARS,
                          text, sizeof(text));
    if (rc == PSPSKY_E_CANCELLED) {
        return PSPSKY_OK;
    }
    if (rc != PSPSKY_OK) {
        pspsky_ui_message(pspsky_strerror(rc));
        return PSPSKY_OK;
    }
    if (text[0] == '\0') {
        return PSPSKY_OK;
    }
    if (pspsky_utf8_count(text) > APP_POST_MAX_CHARS) {
        pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_TEXT_TOO_LONG));
        return PSPSKY_OK;
    }

    for (;;) {
        pspsky_ui_compose_choice_t choice =
            pspsky_ui_compose_confirm(text, paths, image_count);

        if (choice == PSPSKY_UI_COMPOSE_POST_WITH_IMAGE) {
            if (image_count < PSPSKY_MAX_IMAGES &&
                app_compose_add_image(image_paths[image_count],
                                       sizeof(image_paths[image_count]),
                                       &images[image_count]) == PSPSKY_OK) {
                image_count++;
            }
            continue;
        }
        if (choice == PSPSKY_UI_COMPOSE_REMOVE_IMAGE) {
            if (image_count > 0) {
                image_count--;
                image_paths[image_count][0] = '\0';
                memset(&images[image_count], 0, sizeof(images[image_count]));
            }
            continue;
        }
        if (choice == PSPSKY_UI_COMPOSE_POST) {
            rc = app_compose_upload_images(paths, images, image_count);
            if (rc == APP_COMPOSE_BACK) {
                continue;
            }
            if (rc != PSPSKY_OK) {
                return rc;
            }
            /* Timestamp after all uploads, which can take several minutes. */
            if (!compose_now_iso(iso, sizeof(iso))) {
                continue;
            }
            return compose_send(text, iso, image_count > 0 ? images : NULL,
                                image_count);
        }
        return PSPSKY_OK;
    }
}

static void app_main_loop(void)
{
    pspsky_ui_timeline_view_t view;
    pspsky_ui_action_t        act;
    int rc;

    while (!g_exit_requested) {
        view.posts     = g_posts;
        view.count     = g_post_count;
        view.has_more  = g_has_more;
        view.now_unix  = now_unix();
        view.my_handle = g_client.session.handle;
        view.images    = g_tl_images;
        view.avatars   = g_post_avatars;

        act = pspsky_ui_timeline_frame(&view);
        rc  = PSPSKY_OK;

        switch (act) {
        case PSPSKY_UI_ACT_LOAD_OLDER:
            rc = timeline_load_older();
            break;
        case PSPSKY_UI_ACT_REFRESH:
            rc = timeline_reload();
            break;
        case PSPSKY_UI_ACT_COMPOSE:
            rc = app_compose();
            break;
        case PSPSKY_UI_ACT_QUIT:
            if (pspsky_ui_confirm(pspsky_tr(PSPSKY_STR_CONFIRM_QUIT))) {
                return;
            }
            break;
        case PSPSKY_UI_ACT_NONE:
        default:
            break;
        }

        if (rc == PSPSKY_OK) {
            continue;
        }

        if (rc == PSPSKY_E_AUTH_EXPIRED) {
            /* Only reached when the automatic refresh also failed. */
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_SESSION_EXPIRED_RELOGIN));
            if (app_login(1) != PSPSKY_OK) {
                return;
            }
            rc = timeline_reload();
            if (rc != PSPSKY_OK) {
                show_error(rc);
            }
        } else {
            show_error(rc);
        }
    }
}

int main(int argc, char *argv[])
{
    int rc;
    int from_config;

    memset(&g_client, 0, sizeof(g_client));
    memset(&g_transport, 0, sizeof(g_transport));
    memset(&g_cfg, 0, sizeof(g_cfg));
    timeline_clear();

    setup_callbacks();
    resolve_paths(argc, argv);

    init_language();

    rc = pspsky_ui_init();
    if (rc != PSPSKY_OK) {
        printf("PSPSky: ui_init failed (%d: %s)\n", rc, pspsky_strerror(rc));
        sceKernelDelayThread(3 * 1000 * 1000);
        sceKernelExitGame();
        return 0;
    }

    report_cacert_diag();

    pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_NET_INIT));
    rc = pspsky_net_init();
    if (rc != PSPSKY_OK) {
        pspsky_ui_message(pspsky_strerror(rc));
        goto quit_ui;
    }

    pspsky_ui_status(pspsky_tr(PSPSKY_STR_NET_CONNECTING_AP));
    rc = pspsky_net_connect(APP_NET_CONF_INDEX, APP_NET_TIMEOUT_SEC,
                            net_progress, NULL);
    if (rc != PSPSKY_OK) {
        pspsky_ui_message(pspsky_strerror(rc));
        goto quit_net;
    }

    pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_TRANSPORT_INIT));
    rc = pspsky_curl_transport_init(&g_transport, g_cacert_path);
    if (rc != PSPSKY_OK) {
        pspsky_ui_message(pspsky_strerror(rc));
        goto quit_net;
    }

    /* Preserve language and image settings when replacing invalid credentials. */
    from_config = (g_cfg_rc == PSPSKY_OK) ? 1 : 0;
    if (!from_config) {
        memset(g_cfg.identifier, 0, sizeof(g_cfg.identifier));
        memset(g_cfg.app_password, 0, sizeof(g_cfg.app_password));
        for (;;) {
            int prc = prompt_credentials(&g_cfg);
            if (prc == PSPSKY_OK) {
                break;
            }
            if (pspsky_ui_confirm(pspsky_tr(PSPSKY_STR_CONFIRM_ABORT_QUIT))) {
                goto quit_all;
            }
        }
    }

    rc = app_login(from_config);
    if (rc != PSPSKY_OK) {
        goto quit_all;
    }

    rc = timeline_reload();
    if (rc != PSPSKY_OK) {
        show_error(rc);
        pspsky_ui_timeline_invalidate(1);
    }

    app_main_loop();

quit_all:
    pspsky_curl_transport_term(&g_transport);
quit_net:
    pspsky_net_disconnect();
    pspsky_net_term();
quit_ui:
    img_cache_clear();
    avatar_cache_clear();
    pspsky_ui_term();
    sceKernelExitGame();
    return 0;
}
