/*
 * PSPSky - pspsky_transport_t over libcurl (mbedTLS). One easy handle is
 * reused across requests; a PSP TLS handshake costs seconds. Never proxied.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <curl/curl.h>

#include "pspsky/error.h"
#include "pspsky/strings.h"
#include "pspsky/transport.h"
#include "pspsky/transport_curl.h"

#define CACERT_PATH_MAX 256
#define WBUF_INITIAL    4096

/* "-0x" + 5 hex digits + NUL, for codes curl's mbedTLS backend puts in errbuf. */
#define MBEDTLS_CODE_MAX 9

/* Longest BADCERT_* token curl writes into errbuf, + NUL. */
#define BADCERT_TOKEN_MAX 25

typedef struct {
    CURL *easy;
    char  cacert[CACERT_PATH_MAX];
    /* CURLOPT_ERRORBUFFER; curl writes only on failure, so clear before each
       perform. */
    char  errbuf[CURL_ERROR_SIZE];
} curl_ctx_t;

static curl_ctx_t g_ctx;
static int        g_global_inited = 0;

/* ------------------------------------------------------ response body buffer */

typedef struct {
    char  *data;
    size_t len;      /* byte length excluding the NUL              */
    size_t cap;      /* bytes allocated, NUL included              */
    int    overflow; /* 1 = over limit or alloc failed -> E_NOMEM  */
} wbuf_t;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    wbuf_t *b = (wbuf_t *)userdata;
    size_t  n = size * nmemb;
    size_t  need;

    if (b->overflow) {
        return 0; /* abort the transfer */
    }
    if (n == 0) {
        return 0;
    }
    if (b->len + n > (size_t)PSPSKY_HTTP_BODY_MAX) {
        b->overflow = 1;
        return 0;
    }

    need = b->len + n + 1; /* room for the NUL terminator */
    if (need > b->cap) {
        size_t ncap = (b->cap != 0) ? b->cap : (size_t)WBUF_INITIAL;
        char  *np;

        while (ncap < need) {
            ncap *= 2; /* grow by doubling */
        }
        if (ncap > (size_t)PSPSKY_HTTP_BODY_MAX + 1) {
            ncap = (size_t)PSPSKY_HTTP_BODY_MAX + 1;
        }
        np = (char *)realloc(b->data, ncap);
        if (np == NULL) {
            b->overflow = 1;
            return 0;
        }
        b->data = np;
        b->cap  = ncap;
    }

    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* ---------------------------------------------------- error classification */

/* Policy: TLS/certificate failures map to E_TLS, everything else to
   E_TRANSPORT; the split only chooses the message shown to the user.
   CURLE_SSL_CACERT is an alias of CURLE_PEER_FAILED_VERIFICATION since libcurl
   7.62, so the two can never both appear as switch cases. */
static int is_tls_error(CURLcode rc)
{
    return (rc == CURLE_PEER_FAILED_VERIFICATION ||
            rc == CURLE_SSL_CACERT ||
            rc == CURLE_SSL_CACERT_BADFILE ||
            rc == CURLE_SSL_CONNECT_ERROR);
}

static int is_connectivity_error(CURLcode rc)
{
    switch (rc) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
        return 1;
    default:
        return 0;
    }
}

/* Longest prefix of s within max bytes that ends on a UTF-8 boundary. */
static int utf8_fit_bytes(const char *s, int max)
{
    int n;

    if (max <= 0) {
        return 0;
    }
    n = (int)strlen(s);
    if (n <= max) {
        return n;
    }
    /* Back out of a continuation byte (10xxxxxx). */
    while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) {
        max--;
    }
    return max;
}

/* Pull the "-0x2900" style code out of errbuf; 1 if one was found. */
static int extract_mbedtls_code(const char *errbuf, char *out, size_t outsz)
{
    const char *p;
    size_t      i;

    if (out == NULL || outsz == 0) {
        return 0;
    }
    out[0] = '\0';
    if (errbuf == NULL || errbuf[0] == '\0') {
        return 0;
    }
    p = strstr(errbuf, "-0x");
    if (p == NULL) {
        return 0;
    }
    /* First 3 characters are "-0x"; after that, hex digits only. */
    for (i = 0; i < 8 && i + 1 < outsz && p[i] != '\0'; i++) {
        if (i >= 3 && !isxdigit((unsigned char)p[i])) {
            break;
        }
        out[i] = p[i];
    }
    out[i] = '\0';
    if (i <= 3) {
        out[0] = '\0';   /* "-0x" with no digits: unusable */
        return 0;
    }
    return 1;
}

/* Pull a BADCERT_* token out of errbuf; 1 if one was found. */
static int extract_badcert_token(const char *errbuf, char *out, size_t outsz)
{
    const char *p;
    size_t      i;

    if (out == NULL || outsz == 0) {
        return 0;
    }
    out[0] = '\0';
    if (errbuf == NULL || errbuf[0] == '\0') {
        return 0;
    }
    p = strstr(errbuf, "BADCERT_");
    if (p == NULL) {
        return 0;
    }
    for (i = 0; i + 1 < outsz && i < BADCERT_TOKEN_MAX - 1; i++) {
        char ch = p[i];

        if (!((ch >= 'A' && ch <= 'Z') || ch == '_')) {
            break;
        }
        out[i] = ch;
    }
    out[i] = '\0';
    return (i > 0) ? 1 : 0;
}

/* Appends only if it fits, so low-priority fragments drop out on their own. */
static int errmsg_append(char *dst, size_t dstsz, const char *add)
{
    size_t cur;
    size_t n;

    if (dst == NULL || add == NULL || add[0] == '\0') {
        return 0;
    }
    cur = strlen(dst);
    n   = strlen(add);
    if (cur + n + 1 > dstsz) {
        return 0;
    }
    memcpy(dst + cur, add, n + 1);
    return 1;
}

/* Splits the TLS message three ways; the caller still returns E_TLS. */
static void fill_tls_errmsg(CURLcode rc, const curl_ctx_t *c,
                            pspsky_http_resp_t *resp)
{
    const char *eb = (c != NULL) ? c->errbuf : NULL;
    char code[MBEDTLS_CODE_MAX];
    char sfx[MBEDTLS_CODE_MAX + 16];

    /* Kept as " mbedTLS:-0x2900" when available. */
    sfx[0] = '\0';
    if (extract_mbedtls_code(eb, code, sizeof code)) {
        snprintf(sfx, sizeof sfx, " mbedTLS:%s", code);
    }

    if (rc == CURLE_SSL_CACERT_BADFILE) {
        /* Truncate the path, not the trailing codes, so both stay visible. */
        const char *pre = pspsky_tr(PSPSKY_STR_TRC_CACERT_READ_PRE);
        static const char post[] = " (curl:77)";
        const char *path = (c != NULL && c->cacert[0] != '\0')
                               ? c->cacert
                               : pspsky_tr(PSPSKY_STR_TRC_PATH_UNSET);
        int room = (int)sizeof resp->errmsg - 1
                   - (int)strlen(pre) - (int)(sizeof post - 1)
                   - (int)strlen(sfx);

        if (room < 0) {
            room = 0;
        }
        snprintf(resp->errmsg, sizeof resp->errmsg, "%s%.*s%s%s",
                 pre, utf8_fit_bytes(path, room), path, post, sfx);
    } else if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT) {
        /* The rejecting BADCERT flag and the clock time() saw, appended while
           they fit: together they show whether a wrong clock is to blame.
           e.g. "Cert verification failed (curl:60) BADCERT_EXPIRED t=1785067200" */
        char tok[BADCERT_TOKEN_MAX];
        char frag[BADCERT_TOKEN_MAX + 8];

        snprintf(resp->errmsg, sizeof resp->errmsg, "%s",
                 pspsky_tr(PSPSKY_STR_TRC_CERT_VERIFY));

        if (extract_badcert_token(eb, tok, sizeof tok)) {
            snprintf(frag, sizeof frag, " %s", tok);
            errmsg_append(resp->errmsg, sizeof resp->errmsg, frag);
        }
        snprintf(frag, sizeof frag, " t=%lld", (long long)time(NULL));
        errmsg_append(resp->errmsg, sizeof resp->errmsg, frag);

        errmsg_append(resp->errmsg, sizeof resp->errmsg, sfx);
    } else {
        /* Remaining is_tls_error() case: CURLE_SSL_CONNECT_ERROR. */
        snprintf(resp->errmsg, sizeof resp->errmsg,
                 pspsky_tr(PSPSKY_STR_TRC_TLS_HANDSHAKE), sfx);
    }
}

/* Maps a failed rc onto the interface error codes and fills resp->errmsg. */
static int classify_curl_error(CURLcode rc, const curl_ctx_t *c,
                               pspsky_http_resp_t *resp)
{
    if (is_tls_error(rc)) {
        fill_tls_errmsg(rc, c, resp);
        return PSPSKY_E_TLS;
    }
    if (is_connectivity_error(rc)) {
        snprintf(resp->errmsg, sizeof resp->errmsg,
                 pspsky_tr(PSPSKY_STR_TRC_CONNECT_FAIL), (int)rc);
        return PSPSKY_E_TRANSPORT;
    }
    snprintf(resp->errmsg, sizeof resp->errmsg,
             pspsky_tr(PSPSKY_STR_TRC_COMM_FAIL), (int)rc);
    return PSPSKY_E_TRANSPORT;
}

/* ---------------------------------------------------------------- perform */

static int curl_transport_perform(void *ctx,
                                  const pspsky_http_req_t *req,
                                  pspsky_http_resp_t *resp)
{
    curl_ctx_t         *c       = (curl_ctx_t *)ctx;
    struct curl_slist  *hdrs    = NULL;
    struct curl_slist  *tmp     = NULL;
    char               *authhdr = NULL;
    CURL               *easy;
    CURLcode            rc;
    wbuf_t              buf;
    long                code = 0;
    int                 ret;

    if (resp == NULL) {
        return PSPSKY_E_ARG;
    }
    resp->status    = 0;
    resp->body      = NULL;
    resp->body_len  = 0;
    resp->errmsg[0] = '\0';

    if (c == NULL || c->easy == NULL || req == NULL ||
        req->url == NULL || req->method == NULL) {
        snprintf(resp->errmsg, sizeof resp->errmsg, "%s",
                 pspsky_tr(PSPSKY_STR_ERR_ARG));
        return PSPSKY_E_ARG;
    }
    /* body (JSON) and body_bin (raw) are exclusive; body_bin is POST only. */
    if (req->body != NULL && req->body_bin != NULL) {
        snprintf(resp->errmsg, sizeof resp->errmsg, "%s",
                 pspsky_tr(PSPSKY_STR_ERR_ARG));
        return PSPSKY_E_ARG;
    }
    if (req->body_bin != NULL &&
        (req->content_type == NULL || req->content_type[0] == '\0')) {
        snprintf(resp->errmsg, sizeof resp->errmsg, "%s",
                 pspsky_tr(PSPSKY_STR_ERR_ARG));
        return PSPSKY_E_ARG;
    }
    if (req->body_bin != NULL && strcmp(req->method, "POST") != 0) {
        snprintf(resp->errmsg, sizeof resp->errmsg, "%s",
                 pspsky_tr(PSPSKY_STR_ERR_ARG));
        return PSPSKY_E_ARG;
    }
    easy = c->easy;

    memset(&buf, 0, sizeof buf);

    /* --- headers --- */
    hdrs = curl_slist_append(NULL, "Accept: application/json");
    if (hdrs == NULL) {
        goto nomem;
    }
    if (req->body_bin != NULL) {
        /* A mime is at most PSPSKY_MAX_BLOB_MIME (64), so 96 always fits. */
        char ctbuf[96];

        snprintf(ctbuf, sizeof ctbuf, "Content-Type: %s", req->content_type);
        tmp = curl_slist_append(hdrs, ctbuf);
        if (tmp == NULL) {
            goto nomem;
        }
        hdrs = tmp;
    } else if (req->body != NULL) {
        tmp = curl_slist_append(hdrs, "Content-Type: application/json");
        if (tmp == NULL) {
            goto nomem;
        }
        hdrs = tmp;
    }
    if (req->bearer != NULL && req->bearer[0] != '\0') {
        size_t n = strlen(req->bearer) + sizeof("Authorization: Bearer ");

        authhdr = (char *)malloc(n);
        if (authhdr == NULL) {
            goto nomem;
        }
        snprintf(authhdr, n, "Authorization: Bearer %s", req->bearer);
        tmp = curl_slist_append(hdrs, authhdr);
        if (tmp == NULL) {
            goto nomem;
        }
        hdrs = tmp;
    }

    /* --- method and body --- */
    if (strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        if (req->body_bin != NULL) {
            /* curl does not copy this: body_bin must outlive perform. */
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS,
                             (const char *)req->body_bin);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE,
                             (long)req->body_bin_len);
        } else {
            const char *body = (req->body != NULL) ? req->body : "";

            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
        }
    } else {
        curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    }

    /* --- options re-applied on every request --- */
    curl_easy_setopt(easy, CURLOPT_URL, req->url);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
    if (c->cacert[0] != '\0') {
        curl_easy_setopt(easy, CURLOPT_CAINFO, c->cacert);
    }
    /* The SDK's libcurl has the build machine's /etc/ssl/certs baked in as the
       default CA path, which CAINFO does not override. Probing that missing
       directory fails the request with curl:77, so it must be cleared. */
    curl_easy_setopt(easy, CURLOPT_CAPATH, NULL);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 30L);
    /* ~2MB over 802.11b can exceed 60 s, so only uploads get 180 s. A caller
       that states its own budget (thumbnails) overrides both. */
    curl_easy_setopt(easy, CURLOPT_TIMEOUT,
                     (req->timeout_sec > 0)
                         ? (long)req->timeout_sec
                         : ((req->body_bin != NULL) ? 180L : 60L));
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT,
                     "PSPSky/1.1.0 (PlayStation Portable; ARK-4)");
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, (void *)&buf);

    /* curl leaves errbuf untouched on success. */
    c->errbuf[0] = '\0';

    rc = curl_easy_perform(easy);

    /* The handle is reused: drop the reference before freeing the slist. */
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(hdrs);
    hdrs = NULL;
    free(authhdr);
    authhdr = NULL;

    if (buf.overflow) {
        printf("pspsky_curl: response body exceeded %d bytes\n",
               (int)PSPSKY_HTTP_BODY_MAX);
        snprintf(resp->errmsg, sizeof resp->errmsg, "%s",
                 pspsky_tr(PSPSKY_STR_TRC_RESP_TOO_LARGE));
        ret = PSPSKY_E_NOMEM;
        goto fail;
    }
    if (rc != CURLE_OK) {
        printf("pspsky_curl: curl_easy_perform = %d (%s)\n",
               (int)rc, curl_easy_strerror(rc));
        if (c->errbuf[0] != '\0') {
            printf("pspsky_curl: errbuf=%s\n", c->errbuf);
        }
        ret = classify_curl_error(rc, c, resp);
        goto fail;
    }

    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);

    if (buf.data == NULL) {
        /* Empty string so the caller can free() even with no body. */
        buf.data = (char *)malloc(1);
        if (buf.data == NULL) {
            snprintf(resp->errmsg, sizeof resp->errmsg, "%s",
                     pspsky_tr(PSPSKY_STR_ERR_NOMEM));
            ret = PSPSKY_E_NOMEM;
            goto fail;
        }
        buf.data[0] = '\0';
        buf.len     = 0;
    }

    resp->status    = (int)code;
    resp->body      = buf.data;
    resp->body_len  = buf.len;
    resp->errmsg[0] = '\0';
    return PSPSKY_OK;

nomem:
    if (hdrs != NULL) {
        curl_slist_free_all(hdrs);
    }
    free(authhdr);
    snprintf(resp->errmsg, sizeof resp->errmsg, "%s",
             pspsky_tr(PSPSKY_STR_ERR_NOMEM));
    ret = PSPSKY_E_NOMEM;
    /* fallthrough */

fail:
    free(buf.data);
    resp->status   = 0;
    resp->body     = NULL;
    resp->body_len = 0;
    return ret;
}

/* ------------------------------------------------------------ init / term */

int pspsky_curl_transport_init(pspsky_transport_t *out, const char *cacert_path)
{
    if (out == NULL) {
        return PSPSKY_E_ARG;
    }
    out->ctx     = NULL;
    out->perform = NULL;

    if (!g_global_inited) {
        if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
            printf("pspsky_curl: curl_global_init failed\n");
            return PSPSKY_E_NOMEM;
        }
        g_global_inited = 1;
    }

    if (g_ctx.easy == NULL) {
        g_ctx.easy = curl_easy_init();
        if (g_ctx.easy == NULL) {
            printf("pspsky_curl: curl_easy_init failed\n");
            curl_global_cleanup();
            g_global_inited = 0;
            return PSPSKY_E_NOMEM;
        }
    }

    g_ctx.cacert[0] = '\0';
    if (cacert_path != NULL && cacert_path[0] != '\0') {
        snprintf(g_ctx.cacert, sizeof g_ctx.cacert, "%s", cacert_path);
    }

    /* g_ctx is static, so setting this once is enough. */
    g_ctx.errbuf[0] = '\0';
    curl_easy_setopt(g_ctx.easy, CURLOPT_ERRORBUFFER, g_ctx.errbuf);

    out->ctx     = &g_ctx;
    out->perform = curl_transport_perform;
    return PSPSKY_OK;
}

void pspsky_curl_transport_term(pspsky_transport_t *t)
{
    if (g_ctx.easy != NULL) {
        curl_easy_cleanup(g_ctx.easy);
        g_ctx.easy = NULL;
    }
    g_ctx.cacert[0] = '\0';
    g_ctx.errbuf[0] = '\0';

    if (g_global_inited) {
        curl_global_cleanup();
        g_global_inited = 0;
    }

    if (t != NULL) {
        t->ctx     = NULL;
        t->perform = NULL;
    }
}
