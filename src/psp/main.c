/*
 * PSPSky - entry point and application state machine.
 * Every network call blocks, so the UI is drawn one frame beforehand.
 */

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

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define APP_MAX_POSTS       100   /* posts kept in memory           */
#define APP_TIMELINE_LIMIT  10    /* posts per getTimeline call     */
#define APP_POST_MAX_CHARS  300   /* max characters per post        */
#define APP_LOGIN_TRIES     3     /* max login attempts             */
#define APP_NET_CONF_INDEX  1     /* connection profile 1           */
#define APP_NET_TIMEOUT_SEC 45

/* Heap spent on decoded timeline thumbnails; past it, posts keep the marker. */
#define APP_IMG_CACHE_MAX   (4 * 1024 * 1024)

#define APP_DEFAULT_BASEDIR "ms0:/PSP/GAME/PSPSKY/"

#define APP_BASEDIR_MAX     256
#define APP_PATH_MAX        (APP_BASEDIR_MAX + 32)
/* The OSK limit is in characters; UTF-8 needs up to 3 bytes each. */
#define APP_CRED_CHARS      128
#define APP_CRED_BUF        (APP_CRED_CHARS * 3 + 8)
#define APP_TEXT_BUF        (APP_POST_MAX_CHARS * 3 + 8)

/* Positive, so it never collides with the OK/negative error codes. */
#define APP_COMPOSE_BACK    1

/* ------------------------------------------------------------------ */
/* State (large structs are kept off the stack)                        */
/* ------------------------------------------------------------------ */

static pspsky_post_t      g_posts[APP_MAX_POSTS];
static int                g_post_count;
static char               g_cursor[PSPSKY_MAX_CURSOR];
static int                g_has_more;

/* Thumbnails parallel to g_posts; rgb is malloc'd and owned here. */
static pspsky_ui_tl_image_t g_tl_images[APP_MAX_POSTS];
static size_t               g_img_cache_bytes;
/* Decode scratch, reused for every thumbnail. ~90KB. */
static unsigned char g_img_work[PSPSKY_UI_TL_IMG_MAX_W *
                                PSPSKY_UI_TL_IMG_MAX_H * 3];

static pspsky_client_t    g_client;    /* ~5KB  */
static pspsky_timeline_t  g_tl;        /* ~36KB */
static pspsky_transport_t g_transport;
static pspsky_config_t    g_cfg;
/* Result of the single config load done by init_language(). */
static int                g_cfg_rc = PSPSKY_E_IO;

static char g_basedir[APP_BASEDIR_MAX];
static char g_cacert_path[APP_PATH_MAX];
static char g_cfg_path[APP_PATH_MAX];

/* cacert.pem self-test: replays each step of mbedtls_pk_load_file() so a load
   failure can be pinned to one stage, reported once the UI is up. */

/* Above this, skip the calloc rather than trust a bogus size. */
#define CA_DIAG_SIZE_MAX (1L << 20)

/* "stage not reached"; distinct from fseek's 0 and -1. */
#define CA_DIAG_NA (-9)

typedef struct {
    int  open_ok;      /* fopen succeeded                     */
    int  seek_end_rc;  /* fseek SEEK_END rc (-9 = skipped)    */
    long size;         /* ftell (-1 = failed)                 */
    int  seek_set_rc;  /* fseek SEEK_SET rc (-9 = skipped)    */
    int  alloc_ok;     /* calloc succeeded                    */
    long read_len;     /* fread bytes (-1 = skipped)          */
    int  begin_ok;     /* starts with "-----BEGIN "           */
    int  parse_rc;     /* mbedtls_x509_crt_parse_file rc      */
    int  cert_count;   /* certificates parsed                 */
    unsigned int free_kb; /* free memory at the time (KB)     */
} cacert_diag_t;

static cacert_diag_t g_ca;
static char          g_ca_line[224];   /* formatted diagnostic line (ASCII) */

static volatile int g_exit_requested;

/* ------------------------------------------------------------------ */
/* Exit callback (HOME menu)                                           */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* Path resolution / current time                                      */
/* ------------------------------------------------------------------ */

/* A failed parse still leaves a version==0 head node, so skip those. */
static int crt_chain_len(const mbedtls_x509_crt *crt)
{
    const mbedtls_x509_crt *p;
    int n = 0;

    for (p = crt; p != NULL && p->version != 0; p = p->next) {
        n++;
    }
    return n;
}

/* Negative mbedTLS codes as "-0x2900", non-negative as decimal. */
static void fmt_mbedtls_rc(char *out, size_t n, int rc)
{
    if (rc < 0) {
        snprintf(out, n, "-0x%04X", (unsigned int)(-rc));
    } else {
        snprintf(out, n, "%d", rc);
    }
}

/* Replays mbedtls_pk_load_file()'s steps, then runs the real parse too so the
   two can be compared. */
static void cacert_selftest(void)
{
    /* Several hundred bytes; keep it off the stack. */
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

    /* init and free must always be paired. */
    mbedtls_x509_crt_init(&crt);
    g_ca.parse_rc   = mbedtls_x509_crt_parse_file(&crt, g_cacert_path);
    g_ca.cert_count = crt_chain_len(&crt);
    mbedtls_x509_crt_free(&crt);

    g_ca.free_kb = (unsigned int)(sceKernelTotalFreeMemSize() / 1024u);

    fmt_mbedtls_rc(rcbuf, sizeof rcbuf, g_ca.parse_rc);
    /* ASCII and language-independent; the caller adds a localised heading. */
    snprintf(g_ca_line, sizeof g_ca_line,
             "open=%s seek=%d size=%ld seek2=%d alloc=%s read=%ld "
             "begin=%s parse=%s certs=%d free=%uKB",
             g_ca.open_ok ? "OK" : "NG", g_ca.seek_end_rc, g_ca.size,
             g_ca.seek_set_rc, g_ca.alloc_ok ? "OK" : "NG", g_ca.read_len,
             g_ca.begin_ok ? "OK" : "NG", rcbuf, g_ca.cert_count,
             g_ca.free_kb);
}

/* 1 = every stage as expected, nothing to report. */
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

/* basedir is argv[0] up to the last '/', else APP_DEFAULT_BASEDIR. */
static void resolve_paths(int argc, char *argv[])
{
    g_basedir[0] = '\0';

    if (argc > 0 && argv != NULL && argv[0] != NULL) {
        const char *slash = strrchr(argv[0], '/');
        if (slash != NULL) {
            size_t n = (size_t)(slash - argv[0]) + 1; /* include the '/' */
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

/* System language, overridden by lang= in the config. Call once, before any
   UI string is produced. */
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

/* Reports only on failure, then continues anyway. */
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

/* UTC epoch seconds from the RTC, independent of time(). */
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

/* mbedTLS validates certificate dates with time(), but the PSP newlib time()
 * does not return true UTC, so valid certificates are rejected. -Wl,--wrap=time
 * routes every caller here; __real_time is the fallback when the RTC fails. */
extern time_t __real_time(time_t *t);

time_t __wrap_time(time_t *t)
{
    long long now = now_unix();
    time_t    v;

    if (now <= 0) {
        /* RTC unavailable */
        return __real_time(t);
    }
    v = (time_t)now;
    if (t != NULL) {
        *t = v;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Message helpers                                                     */
/* ------------------------------------------------------------------ */

static void net_progress(const char *msg_utf8, void *user)
{
    (void)user;
    pspsky_ui_status(msg_utf8);
}

/* E_TLS is special-cased: its generic text tells the user to check the clock,
   which contradicts the specific reason the transport already supplied. */
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

/* ------------------------------------------------------------------ */
/* Timeline model                                                      */
/* ------------------------------------------------------------------ */

/* Drops every cached thumbnail. The UI caches textures by post index, so it
   has to let go of them before the buffers behind them are freed. */
static void img_cache_clear(void)
{
    int i;

    pspsky_ui_timeline_invalidate(0);
    for (i = 0; i < APP_MAX_POSTS; i++) {
        free((void *)g_tl_images[i].rgb);
    }
    memset(g_tl_images, 0, sizeof(g_tl_images));
    g_img_cache_bytes = 0;
}

/* Fetches and decodes the first thumbnail of each post in
   [start, g_post_count). Every failure is silent: the post keeps its marker. */
static void fetch_new_images(int start)
{
    char msg[128];
    int  i;
    int  m = 0;
    int  n = 0;

    if (!g_cfg.show_images) {
        return;
    }
    for (i = start; i < g_post_count; i++) {
        if (g_posts[i].thumb_url[0] != '\0') {
            m++;
        }
    }
    if (m == 0) {
        return;
    }

    for (i = start; i < g_post_count; i++) {
        unsigned char *body = NULL;
        unsigned char *rgb;
        size_t         len  = 0;
        size_t         sz;
        int            tw   = 0;
        int            th   = 0;
        int            sw   = 0;   /* source dims; unused, tw/th are shown */
        int            sh   = 0;
        int            decoded;

        if (g_posts[i].thumb_url[0] == '\0') {
            continue;
        }
        n++;
        /* Budget spent: stop paying for transfers that cannot be kept. */
        if (g_img_cache_bytes >= (size_t)APP_IMG_CACHE_MAX) {
            continue;
        }

        snprintf(msg, sizeof(msg), pspsky_tr(PSPSKY_STR_ST_FETCHING_IMAGES),
                 n, m);
        pspsky_ui_status(msg);

        /* The reason never reaches the user, so no errmsg buffer is kept. */
        decoded = 0;
        if (pspsky_atp_fetch_image(&g_transport, g_posts[i].thumb_url,
                                   &body, &len, NULL) == PSPSKY_OK &&
            pspsky_webp_thumb_decode(body, len,
                                     PSPSKY_UI_TL_IMG_MAX_W,
                                     PSPSKY_UI_TL_IMG_MAX_H,
                                     g_img_work, sizeof(g_img_work),
                                     &tw, &th, &sw, &sh) == PSPSKY_OK) {
            decoded = 1;
        }
        /* fetch_image leaves body NULL unless it handed one over. */
        free(body);
        if (!decoded) {
            printf("PSPSky: thumbnail %d unavailable\n", i);
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
        g_tl_images[i].rgb = rgb;
        g_tl_images[i].w   = (short)tw;
        g_tl_images[i].h   = (short)th;
        g_img_cache_bytes += sz;
    }
}

static void timeline_clear(void)
{
    g_post_count = 0;
    g_cursor[0]  = '\0';
    g_has_more   = 0;
}

/* Appends to g_posts and updates cursor/has_more. */
static void timeline_append(const pspsky_timeline_t *tl)
{
    int i;

    for (i = 0; i < tl->count && g_post_count < APP_MAX_POSTS; i++) {
        g_posts[g_post_count++] = tl->posts[i];
    }
    snprintf(g_cursor, sizeof(g_cursor), "%s", tl->cursor);
    /* Stop offering more once the cap is reached. */
    g_has_more = (g_cursor[0] != '\0' && g_post_count < APP_MAX_POSTS) ? 1 : 0;
}

/* REFRESH */
static int timeline_reload(void)
{
    int rc;

    pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_TIMELINE_LOADING));
    rc = pspsky_client_get_timeline(&g_client, APP_TIMELINE_LIMIT, NULL, &g_tl);
    if (rc != PSPSKY_OK) {
        return rc;
    }
    img_cache_clear();
    timeline_clear();
    timeline_append(&g_tl);
    fetch_new_images(0);
    pspsky_ui_timeline_invalidate(1);
    return PSPSKY_OK;
}

/* LOAD_OLDER */
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

/* ------------------------------------------------------------------ */
/* Authentication                                                      */
/* ------------------------------------------------------------------ */

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

/* from_config=1 means the credentials came from the file, so they are not
   written back on success. */
static int app_login(int from_config)
{
    int attempt;
    int rc      = PSPSKY_E_AUTH;
    int entered = from_config ? 0 : 1;   /* entered via OSK = worth saving */

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
            /* wrong credentials: ask again */
            int prc = prompt_credentials(&g_cfg);
            if (prc != PSPSKY_OK) {
                return prc;
            }
            entered = 1;
        } else {
            /* otherwise ask whether to retry */
            if (!pspsky_ui_confirm(pspsky_tr(PSPSKY_STR_CONFIRM_RETRY))) {
                return PSPSKY_E_CANCELLED;
            }
        }
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Composing a post                                                    */
/* ------------------------------------------------------------------ */

/* Returns 0 on failure, after showing a message. */
static int compose_now_iso(char *iso, size_t cap)
{
    if (pspsky_iso8601_format(now_unix(), iso, cap) != PSPSKY_OK) {
        pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_TIME_FAIL));
        return 0;
    }
    return 1;
}

/* img=NULL produces exactly the text-only body. */
static int compose_send(const char *text, const char *iso,
                        const pspsky_embed_image_t *img)
{
    int rc;

    pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_POSTING));
    rc = pspsky_client_create_post_ex(&g_client, text, iso, img);
    if (rc != PSPSKY_OK) {
        return rc;
    }
    pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_POSTED));
    return timeline_reload();
}

/* Rejects oversize files on size alone, before allocating. Caller frees. */
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
            /* Extension lies about the content: reject here, not at the
               server. */
            free(*out_buf);
            *out_buf = NULL;
            *out_len = 0;
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_IMG_UNSUPPORTED));
            continue;   /* back to the browser */
        }
        if (rc == PSPSKY_E_NOMEM) {
            /* Re-picking would fail the same way. */
            pspsky_ui_message(pspsky_strerror(PSPSKY_E_NOMEM));
            return APP_COMPOSE_BACK;
        }
        if (rc == PSPSKY_E_ARG) {
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_IMG_TOO_LARGE));
        } else {
            pspsky_ui_message(pspsky_tr(PSPSKY_STR_MSG_IMG_READ_FAIL));
        }
        /* back to the browser */
    }
}

/* APP_COMPOSE_BACK returns to the confirm screen with the text intact. */
static int app_compose_with_image(const char *text)
{
    /* ~850 bytes; keep off the stack. */
    static pspsky_embed_image_t img;
    static char    alt_raw[APP_TEXT_BUF];
    static char    img_path[PSPSKY_UI_PICK_PATH_MAX];
    unsigned char *buf  = NULL;
    size_t         len  = 0;
    /* Sniffed from the content; a static literal. */
    const char    *mime = NULL;
    char           iso[32];
    int            w    = 0;
    int            h    = 0;
    int            is_png;
    int            rc;

    /* Cross re-picks; the text lives in app_compose and is never lost. */
    for (;;) {
        unsigned char *thumb = NULL;
        int            tw    = 0;
        int            th    = 0;
        int            sw    = 0;   /* source dims from thumb_decode;   */
        int            sh    = 0;   /* unused, w/h are what is shown    */
        int            ok;

        if (pick_and_load_image(img_path, sizeof(img_path),
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

        /* One buffer shared by both decoders; any failure just means no
           preview, never an abort. */
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

        /* pv->rgb is uploaded once before the UI loop, so it can be freed as
           soon as this returns. */
        {
            pspsky_ui_preview_t pv = { img_path, len, w, h, thumb, tw, th };
            ok = pspsky_ui_preview_image(&pv);
        }
        free(thumb);
        thumb = NULL;

        if (ok) {
            break;      /* confirmed */
        }
        /* re-pick: drop the buffer and go back to the browser */
        free(buf);
        buf = NULL;
    }

    memset(&img, 0, sizeof(img));
    img.aspect_w = w;
    img.aspect_h = h;

    /* Last UI step: a blob can expire if anything blocks between uploadBlob
       and createRecord. Cancelling here just means alt="". */
    alt_raw[0] = '\0';
    if (pspsky_osk_input(pspsky_tr(PSPSKY_STR_OSK_ALT), "",
                         APP_POST_MAX_CHARS,
                         alt_raw, sizeof(alt_raw)) == PSPSKY_OK) {
        pspsky_utf8_truncate(alt_raw, PSPSKY_MAX_ALT - 1);
        snprintf(img.alt, sizeof(img.alt), "%s", alt_raw);
    }

    /* buf must live until this returns: libcurl does not copy it, and an
       expired session makes upload_blob refresh and send again. */
    pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_UPLOADING));
    rc = pspsky_client_upload_blob(&g_client, buf, len, mime, &img.blob);
    free(buf);
    buf = NULL;
    if (rc != PSPSKY_OK) {
        /* app_main_loop drives re-login on E_AUTH_EXPIRED. */
        return rc;
    }

    /* createdAt is taken after the upload, which can take minutes. */
    if (!compose_now_iso(iso, sizeof(iso))) {
        return APP_COMPOSE_BACK;
    }
    return compose_send(text, iso, &img);
}

static int app_compose(void)
{
    static char text[APP_TEXT_BUF];
    char iso[32];
    int  rc;

    /* Text is required even when an image is attached. */
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

    /* Leaving the image flow returns here with the text intact. */
    for (;;) {
        pspsky_ui_compose_choice_t choice = pspsky_ui_compose_confirm(text);

        if (choice == PSPSKY_UI_COMPOSE_POST_WITH_IMAGE) {
            rc = app_compose_with_image(text);
            if (rc == APP_COMPOSE_BACK) {
                continue;
            }
            return rc;
        }
        if (choice == PSPSKY_UI_COMPOSE_POST) {
            if (!compose_now_iso(iso, sizeof(iso))) {
                return PSPSKY_OK;
            }
            return compose_send(text, iso, NULL);
        }
        /* CANCEL: discard */
        return PSPSKY_OK;
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

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
            /* Pass the real rc: a hardcoded one contradicts the TLS message. */
            rc = timeline_reload();
            if (rc != PSPSKY_OK) {
                show_error(rc);
            }
        } else {
            show_error(rc);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

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

    /* Must settle before any UI string. */
    init_language();

    /* No UI to ask through if this fails, so exit directly. */
    rc = pspsky_ui_init();
    if (rc != PSPSKY_OK) {
        /* Console only, and deliberately not localised. */
        printf("PSPSky: ui_init failed (%d: %s)\n", rc, pspsky_strerror(rc));
        sceKernelDelayThread(3 * 1000 * 1000);
        sceKernelExitGame();
        return 0;
    }

    report_cacert_diag();

    /* Network */
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

    /* HTTP transport */
    pspsky_ui_status(pspsky_tr(PSPSKY_STR_ST_TRANSPORT_INIT));
    rc = pspsky_curl_transport_init(&g_transport, g_cacert_path);
    if (rc != PSPSKY_OK) {
        pspsky_ui_message(pspsky_strerror(rc));
        goto quit_net;
    }

    /* Already loaded by init_language(); a hand-written lang= survives an
       otherwise unusable config and is written back on save. */
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

    /* Login */
    rc = app_login(from_config);
    if (rc != PSPSKY_OK) {
        goto quit_all;
    }

    /* First timeline fetch */
    rc = timeline_reload();
    if (rc != PSPSKY_OK) {
        show_error(rc);
        /* Start empty; L re-fetches. */
        pspsky_ui_timeline_invalidate(1);
    }

    app_main_loop();

    /* Cleanup */
quit_all:
    pspsky_curl_transport_term(&g_transport);
quit_net:
    pspsky_net_disconnect();
    pspsky_net_term();
quit_ui:
    img_cache_clear();
    pspsky_ui_term();
    sceKernelExitGame();
    return 0;
}
