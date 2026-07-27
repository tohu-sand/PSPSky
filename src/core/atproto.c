/* AT Protocol (XRPC) client. Portable C99 over libc and vendor/cjson.
 *
 * Engagement counts and viewer state are deliberately never read or stored;
 * only handle, displayName, text, createdAt and the cursor are parsed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cjson/cJSON.h"

#include "pspsky/atproto.h"
#include "pspsky/error.h"
#include "pspsky/timeutil.h"
#include "pspsky/transport.h"
#include "pspsky/utf8.h"

/* ------------------------------------------------------------------ */
/* Endpoints                                                           */
/* ------------------------------------------------------------------ */

#define EP_CREATE_SESSION  PSPSKY_HOST_URL "/xrpc/com.atproto.server.createSession"
#define EP_REFRESH_SESSION PSPSKY_HOST_URL "/xrpc/com.atproto.server.refreshSession"
#define EP_GET_TIMELINE    PSPSKY_HOST_URL "/xrpc/app.bsky.feed.getTimeline"
#define EP_CREATE_RECORD   PSPSKY_HOST_URL "/xrpc/com.atproto.repo.createRecord"
#define EP_UPLOAD_BLOB     PSPSKY_HOST_URL "/xrpc/com.atproto.repo.uploadBlob"

#define POST_COLLECTION    "app.bsky.feed.post"
#define EMBED_IMAGES_TYPE  "app.bsky.embed.images"

/* "via" is not in the app.bsky.feed.post lexicon, but records tolerate extra
   top-level fields and clients render it by convention as "via PSPSky". */
#define PSPSKY_VIA_NAME    "PSPSky"

/* %XX encoding can grow the cursor up to 3x in the worst case */
#define ENC_CURSOR_CAP (3 * PSPSKY_MAX_CURSOR + 1)
/* "?limit=NN&cursor=" + the encoded cursor + slack */
#define URL_CAP        (sizeof(EP_GET_TIMELINE) + 32 + ENC_CURSOR_CAP)

/* ------------------------------------------------------------------ */
/* String utilities                                                    */
/* ------------------------------------------------------------------ */

/* Drops the incomplete multi-byte sequence a raw byte-level cut leaves behind */
static void trim_partial_utf8(char *s)
{
    size_t len = strlen(s);
    size_t k;

    for (k = 1; k <= 4 && k <= len; ++k) {
        unsigned char c = (unsigned char)s[len - k];
        size_t need;

        if ((c & 0xC0) == 0x80) {
            continue;               /* continuation byte; keep walking back */
        }
        if (c < 0x80)                need = 1;
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else                         need = 1;  /* invalid lead byte: leave as is */

        if (need > k) {
            s[len - k] = '\0';      /* incomplete: drop it along with its lead byte */
        }
        return;
    }
}

/* Truncates on a code point boundary if src does not fit */
static void copy_utf8_trunc(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    trim_partial_utf8(dst);
    pspsky_utf8_truncate(dst, cap - 1);
}

/* Copy that must not truncate, for JWT/DID/handle */
static int copy_exact(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (dst == NULL || cap == 0) {
        return PSPSKY_E_ARG;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return PSPSKY_E_ARG;
    }
    n = strlen(src);
    if (n >= cap) {
        return PSPSKY_E_NOMEM;
    }
    memcpy(dst, src, n + 1);
    return PSPSKY_OK;
}

/* errmsg may be NULL; the result is always NUL-terminated */
static void set_errmsg(char *errmsg, const char *src)
{
    if (errmsg == NULL) {
        return;
    }
    copy_utf8_trunc(errmsg, (size_t)PSPSKY_ERRMSG_MAX, src);
}

/* Percent-encodes everything outside [A-Za-z0-9._~-] */
static int urlencode(char *dst, size_t cap, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    if (dst == NULL || cap == 0) {
        return PSPSKY_E_ARG;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return PSPSKY_OK;
    }
    for (; *src != '\0'; ++src) {
        unsigned char c = (unsigned char)*src;

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '~' || c == '-') {
            if (o + 1 >= cap) {
                dst[0] = '\0';
                return PSPSKY_E_NOMEM;
            }
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= cap) {
                dst[0] = '\0';
                return PSPSKY_E_NOMEM;
            }
            dst[o++] = '%';
            dst[o++] = hex[(c >> 4) & 0x0F];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
    return PSPSKY_OK;
}

/* ------------------------------------------------------------------ */
/* JSON utilities                                                      */
/* ------------------------------------------------------------------ */

/* NULL unless obj[key] is a string */
static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsString(it) || it->valuestring == NULL) {
        return NULL;
    }
    return it->valuestring;
}

/* NULL unless obj[key] is an object */
static const cJSON *json_obj(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsObject(it)) {
        return NULL;
    }
    return it;
}

/* ------------------------------------------------------------------ */
/* Request execution and shared error classification                   */
/* ------------------------------------------------------------------ */

/* Performs one request and classifies the result. body and body_bin are
   mutually exclusive; on success *out_body is the malloc'd 2xx body to free. */
static int do_request_ex(const pspsky_transport_t *t,
                         const char *method, const char *url,
                         const char *bearer, const char *body,
                         const unsigned char *body_bin, size_t body_bin_len,
                         const char *content_type,
                         char **out_body, int *out_status, char *errmsg)
{
    pspsky_http_req_t  req;
    pspsky_http_resp_t resp;
    char terr[PSPSKY_HTTP_ERRMSG_MAX + 1];
    int rc;

    if (out_body != NULL) {
        *out_body = NULL;
    }
    if (out_status != NULL) {
        *out_status = 0;
    }
    if (errmsg != NULL) {
        errmsg[0] = '\0';
    }
    if (t == NULL || t->perform == NULL || url == NULL) {
        return PSPSKY_E_ARG;
    }

    /* Must be zeroed in full before any field is set */
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.method       = method;
    req.url          = url;
    req.bearer       = bearer;
    req.body         = body;
    req.body_bin     = body_bin;
    req.body_bin_len = (body_bin != NULL) ? body_bin_len : (size_t)0;
    req.content_type = (body_bin != NULL) ? content_type : NULL;

    rc = t->perform(t->ctx, &req, &resp);

    if (out_status != NULL) {
        *out_status = resp.status;
    }

    /* The transport errmsg is not guaranteed NUL-terminated */
    memcpy(terr, resp.errmsg, sizeof(resp.errmsg));
    terr[PSPSKY_HTTP_ERRMSG_MAX] = '\0';

    /* Transport failures propagate unchanged */
    if (rc != PSPSKY_OK) {
        set_errmsg(errmsg, terr[0] != '\0' ? terr : NULL);
        free(resp.body);
        return rc;
    }

    /* 2xx: the body goes to the caller */
    if (resp.status >= 200 && resp.status <= 299) {
        if (out_body != NULL) {
            *out_body = resp.body;
        } else {
            free(resp.body);
        }
        return PSPSKY_OK;
    }

    /* Classify non-2xx statuses */
    if (resp.status == 429) {
        rc = PSPSKY_E_RATELIMIT;
    } else if (resp.status == 400 || resp.status == 401) {
        rc = PSPSKY_E_AUTH;
    } else {
        rc = PSPSKY_E_HTTP;
    }

    if (resp.body != NULL) {
        cJSON *root = cJSON_Parse(resp.body);

        if (root != NULL) {
            const char *ecode = json_str(root, "error");
            const char *emsg  = json_str(root, "message");

            if ((resp.status == 400 || resp.status == 401) &&
                ecode != NULL && strcmp(ecode, "ExpiredToken") == 0) {
                rc = PSPSKY_E_AUTH_EXPIRED;
            }
            if (emsg != NULL && emsg[0] != '\0') {
                set_errmsg(errmsg, emsg);
            } else if (ecode != NULL && ecode[0] != '\0') {
                set_errmsg(errmsg, ecode);
            }
            cJSON_Delete(root);
        }
    }

    if (errmsg != NULL && errmsg[0] == '\0') {
        char fallback[32];
        snprintf(fallback, sizeof(fallback), "HTTP %d", resp.status);
        set_errmsg(errmsg, fallback);
    }

    free(resp.body);
    return rc;
}

/* Thin wrapper for a JSON body, or no body at all */
static int do_request(const pspsky_transport_t *t,
                      const char *method, const char *url,
                      const char *bearer, const char *body,
                      char **out_body, int *out_status, char *errmsg)
{
    return do_request_ex(t, method, url, bearer, body, NULL, 0, NULL,
                         out_body, out_status, errmsg);
}

/* ------------------------------------------------------------------ */
/* Session JSON -> pspsky_session_t                                    */
/* ------------------------------------------------------------------ */

/* require_ids also demands did and handle. Every length check runs first, so a
   mid-way failure cannot leave the session partially overwritten. */
static int session_fill(const cJSON *root, pspsky_session_t *out, int require_ids)
{
    const char *access  = json_str(root, "accessJwt");
    const char *refresh = json_str(root, "refreshJwt");
    const char *did     = json_str(root, "did");
    const char *handle  = json_str(root, "handle");

    if (access == NULL || access[0] == '\0' ||
        refresh == NULL || refresh[0] == '\0') {
        return PSPSKY_E_PARSE;
    }
    if (require_ids) {
        if (did == NULL || did[0] == '\0' || handle == NULL || handle[0] == '\0') {
            return PSPSKY_E_PARSE;
        }
    }
    if (did != NULL && did[0] == '\0') {
        did = NULL;
    }
    if (handle != NULL && handle[0] == '\0') {
        handle = NULL;
    }

    if (strlen(access)  >= sizeof(out->access_jwt)  ||
        strlen(refresh) >= sizeof(out->refresh_jwt) ||
        (did    != NULL && strlen(did)    >= sizeof(out->did)) ||
        (handle != NULL && strlen(handle) >= sizeof(out->handle))) {
        return PSPSKY_E_NOMEM;
    }

    (void)copy_exact(out->access_jwt,  sizeof(out->access_jwt),  access);
    (void)copy_exact(out->refresh_jwt, sizeof(out->refresh_jwt), refresh);
    if (did != NULL) {
        (void)copy_exact(out->did, sizeof(out->did), did);
    }
    if (handle != NULL) {
        (void)copy_exact(out->handle, sizeof(out->handle), handle);
    }
    return PSPSKY_OK;
}

/* ------------------------------------------------------------------ */
/* Low-level API implementation (internal, reports HTTP status)        */
/* ------------------------------------------------------------------ */

static int atp_create_session_impl(const pspsky_transport_t *t,
                                   const char *identifier,
                                   const char *app_password,
                                   pspsky_session_t *out,
                                   char *errmsg,
                                   int *status)
{
    cJSON *req  = NULL;
    cJSON *root = NULL;
    char  *json = NULL;
    char  *body = NULL;
    int rc;

    if (errmsg != NULL) {
        errmsg[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    if (t == NULL || t->perform == NULL || out == NULL ||
        identifier == NULL || identifier[0] == '\0' ||
        app_password == NULL || app_password[0] == '\0') {
        return PSPSKY_E_ARG;
    }

    memset(out, 0, sizeof(*out));

    req = cJSON_CreateObject();
    if (req == NULL) {
        return PSPSKY_E_NOMEM;
    }
    if (cJSON_AddStringToObject(req, "identifier", identifier) == NULL ||
        cJSON_AddStringToObject(req, "password", app_password) == NULL) {
        rc = PSPSKY_E_NOMEM;
        goto cleanup;
    }
    json = cJSON_PrintUnformatted(req);
    if (json == NULL) {
        rc = PSPSKY_E_NOMEM;
        goto cleanup;
    }

    rc = do_request(t, "POST", EP_CREATE_SESSION, NULL, json,
                    &body, status, errmsg);
    if (rc != PSPSKY_OK) {
        goto cleanup;
    }
    if (body == NULL) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }

    root = cJSON_Parse(body);
    if (root == NULL) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }
    rc = session_fill(root, out, 1);
    if (rc != PSPSKY_OK) {
        memset(out, 0, sizeof(*out));
    }

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (req != NULL) {
        cJSON_Delete(req);
    }
    if (json != NULL) {
        cJSON_free(json);
    }
    free(body);
    return rc;
}

static int atp_refresh_session_impl(const pspsky_transport_t *t,
                                    pspsky_session_t *sess,
                                    char *errmsg,
                                    int *status)
{
    cJSON *root = NULL;
    char  *body = NULL;
    int rc;

    if (errmsg != NULL) {
        errmsg[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    if (t == NULL || t->perform == NULL || sess == NULL) {
        return PSPSKY_E_ARG;
    }
    /* An empty token is not a bad argument; let the server response decide */

    rc = do_request(t, "POST", EP_REFRESH_SESSION, sess->refresh_jwt, NULL,
                    &body, status, errmsg);
    if (rc != PSPSKY_OK) {
        goto cleanup;
    }
    if (body == NULL) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }

    root = cJSON_Parse(body);
    if (root == NULL) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }
    rc = session_fill(root, sess, 0);

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    free(body);
    return rc;
}

/* E_PARSE when handle or text is missing; the caller skips that element */
static int post_from_feed_item(const cJSON *item, pspsky_post_t *p)
{
    const cJSON *post;
    const cJSON *author;
    const cJSON *record;
    const char  *handle;
    const char  *display;
    const char  *text;
    const char  *created;

    if (!cJSON_IsObject(item)) {
        return PSPSKY_E_PARSE;
    }
    post = json_obj(item, "post");
    if (post == NULL) {
        return PSPSKY_E_PARSE;
    }
    author = json_obj(post, "author");
    record = json_obj(post, "record");
    if (author == NULL || record == NULL) {
        return PSPSKY_E_PARSE;
    }

    handle = json_str(author, "handle");
    text   = json_str(record, "text");
    if (handle == NULL || handle[0] == '\0' || text == NULL) {
        return PSPSKY_E_PARSE;
    }

    display = json_str(author, "displayName");   /* optional */
    created = json_str(record, "createdAt");     /* optional */

    memset(p, 0, sizeof(*p));

    /* Presence only: a quote embed can carry engagement counts inside, so
       nothing below this key is ever read */
    p->has_embed =
        (cJSON_GetObjectItemCaseSensitive(record, "embed") != NULL) ? 1 : 0;

    copy_utf8_trunc(p->handle,       sizeof(p->handle),       handle);
    copy_utf8_trunc(p->display_name, sizeof(p->display_name), display);
    copy_utf8_trunc(p->text,         sizeof(p->text),         text);
    copy_utf8_trunc(p->created_at,   sizeof(p->created_at),   created);

    /* Parse the raw string, not the truncated copy */
    p->created_at_unix = (created != NULL) ? pspsky_iso8601_parse(created) : -1;

    return PSPSKY_OK;
}

static int atp_get_timeline_impl(const pspsky_transport_t *t,
                                 const pspsky_session_t *sess,
                                 int limit,
                                 const char *cursor,
                                 pspsky_timeline_t *out,
                                 char *errmsg,
                                 int *status)
{
    char  url[URL_CAP];
    char  enc[ENC_CURSOR_CAP];
    cJSON *root = NULL;
    char  *body = NULL;
    const cJSON *feed;
    const cJSON *item;
    const char  *cur;
    int rc;

    if (errmsg != NULL) {
        errmsg[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    if (t == NULL || t->perform == NULL || sess == NULL || out == NULL) {
        return PSPSKY_E_ARG;
    }
    /* Empty token: same reasoning as above */

    memset(out, 0, sizeof(*out));

    if (limit < 1) {
        limit = 1;
    }
    if (limit > PSPSKY_MAX_POSTS) {
        limit = PSPSKY_MAX_POSTS;
    }

    if (cursor != NULL && cursor[0] != '\0') {
        rc = urlencode(enc, sizeof(enc), cursor);
        if (rc != PSPSKY_OK) {
            return rc;
        }
        snprintf(url, sizeof(url), "%s?limit=%d&cursor=%s",
                 EP_GET_TIMELINE, limit, enc);
    } else {
        snprintf(url, sizeof(url), "%s?limit=%d", EP_GET_TIMELINE, limit);
    }

    rc = do_request(t, "GET", url, sess->access_jwt, NULL, &body, status, errmsg);
    if (rc != PSPSKY_OK) {
        goto cleanup;
    }
    if (body == NULL) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }

    root = cJSON_Parse(body);
    if (root == NULL) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }

    feed = cJSON_GetObjectItemCaseSensitive(root, "feed");
    if (!cJSON_IsArray(feed)) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }

    cJSON_ArrayForEach(item, feed) {
        if (out->count >= PSPSKY_MAX_POSTS) {
            break;
        }
        /* Skip malformed elements and keep going */
        if (post_from_feed_item(item, &out->posts[out->count]) == PSPSKY_OK) {
            out->count++;
        }
    }

    cur = json_str(root, "cursor");
    if (cur != NULL && cur[0] != '\0' && strlen(cur) < sizeof(out->cursor)) {
        (void)copy_exact(out->cursor, sizeof(out->cursor), cur);
    } else {
        out->cursor[0] = '\0';   /* missing or too long becomes empty */
    }

    rc = PSPSKY_OK;

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    free(body);
    return rc;
}

/* Builds record.embed; the caller owns the result. Escaping is left to cJSON. */
static cJSON *build_embed_images(const pspsky_embed_image_t *image)
{
    cJSON *embed  = NULL;
    cJSON *images = NULL;
    cJSON *entry  = NULL;
    cJSON *blob   = NULL;
    cJSON *ref    = NULL;
    cJSON *aspect = NULL;

    embed = cJSON_CreateObject();
    if (embed == NULL) {
        return NULL;
    }
    if (cJSON_AddStringToObject(embed, "$type", EMBED_IMAGES_TYPE) == NULL) {
        goto fail;
    }

    images = cJSON_CreateArray();
    if (images == NULL || !cJSON_AddItemToObject(embed, "images", images)) {
        cJSON_Delete(images);
        goto fail;
    }
    /* embed now owns images */

    entry = cJSON_CreateObject();
    if (entry == NULL || !cJSON_AddItemToArray(images, entry)) {
        cJSON_Delete(entry);
        goto fail;
    }
    /* images now owns entry */

    /* alt is required, and an empty string is a valid value */
    if (cJSON_AddStringToObject(entry, "alt", image->alt) == NULL) {
        goto fail;
    }

    blob = cJSON_CreateObject();
    if (blob == NULL || !cJSON_AddItemToObject(entry, "image", blob)) {
        cJSON_Delete(blob);
        goto fail;
    }
    /* entry now owns blob */

    if (cJSON_AddStringToObject(blob, "$type", "blob") == NULL) {
        goto fail;
    }
    ref = cJSON_CreateObject();
    if (ref == NULL || !cJSON_AddItemToObject(blob, "ref", ref)) {
        cJSON_Delete(ref);
        goto fail;
    }
    /* blob now owns ref */

    if (cJSON_AddStringToObject(ref, "$link", image->blob.cid) == NULL ||
        cJSON_AddStringToObject(blob, "mimeType", image->blob.mime_type) == NULL ||
        cJSON_AddNumberToObject(blob, "size", (double)image->blob.size) == NULL) {
        goto fail;
    }

    /* aspectRatio only when both sides are positive */
    if (image->aspect_w > 0 && image->aspect_h > 0) {
        aspect = cJSON_CreateObject();
        if (aspect == NULL || !cJSON_AddItemToObject(entry, "aspectRatio", aspect)) {
            cJSON_Delete(aspect);
            goto fail;
        }
        /* entry now owns aspect */
        if (cJSON_AddNumberToObject(aspect, "width", (double)image->aspect_w) == NULL ||
            cJSON_AddNumberToObject(aspect, "height", (double)image->aspect_h) == NULL) {
            goto fail;
        }
    }

    return embed;

fail:
    cJSON_Delete(embed);
    return NULL;
}

static int atp_create_post_impl(const pspsky_transport_t *t,
                                const pspsky_session_t *sess,
                                const char *text_utf8,
                                const char *created_at_iso,
                                const pspsky_embed_image_t *image,
                                char *errmsg,
                                int *status)
{
    cJSON *root   = NULL;
    cJSON *record = NULL;
    char  *json   = NULL;
    char   isobuf[32];
    const char *iso;
    int rc;

    if (errmsg != NULL) {
        errmsg[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    if (t == NULL || t->perform == NULL || sess == NULL) {
        return PSPSKY_E_ARG;
    }
    if (text_utf8 == NULL || text_utf8[0] == '\0') {
        return PSPSKY_E_ARG;   /* never send an empty post body */
    }
    if (image != NULL && image->blob.cid[0] == '\0') {
        return PSPSKY_E_ARG;   /* never build an embed without a cid */
    }
    /* Empty did/accessJwt: same reasoning as above */

    iso = created_at_iso;
    if (iso == NULL || iso[0] == '\0') {
        /* Only when the caller supplied no timestamp */
        time_t now = time(NULL);
        if (now == (time_t)-1) {
            return PSPSKY_E_ARG;
        }
        if (pspsky_iso8601_format((long long)now, isobuf, sizeof(isobuf)) != PSPSKY_OK) {
            return PSPSKY_E_ARG;
        }
        iso = isobuf;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        return PSPSKY_E_NOMEM;
    }
    record = cJSON_CreateObject();
    if (record == NULL) {
        rc = PSPSKY_E_NOMEM;
        goto cleanup;
    }
    if (!cJSON_AddItemToObject(root, "record", record)) {
        cJSON_Delete(record);
        rc = PSPSKY_E_NOMEM;
        goto cleanup;
    }
    /* root now owns record */

    if (cJSON_AddStringToObject(root, "repo", sess->did) == NULL ||
        cJSON_AddStringToObject(root, "collection", POST_COLLECTION) == NULL ||
        cJSON_AddStringToObject(record, "$type", POST_COLLECTION) == NULL ||
        cJSON_AddStringToObject(record, "text", text_utf8) == NULL ||
        cJSON_AddStringToObject(record, "createdAt", iso) == NULL ||
        /* Directly under record on both paths, embed or not */
        cJSON_AddStringToObject(record, "via", PSPSKY_VIA_NAME) == NULL) {
        rc = PSPSKY_E_NOMEM;
        goto cleanup;
    }

    /* image == NULL omits the key entirely, matching plain create_post */
    if (image != NULL) {
        cJSON *embed = build_embed_images(image);

        if (embed == NULL || !cJSON_AddItemToObject(record, "embed", embed)) {
            cJSON_Delete(embed);
            rc = PSPSKY_E_NOMEM;
            goto cleanup;
        }
        /* record now owns embed */
    }

    json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        rc = PSPSKY_E_NOMEM;
        goto cleanup;
    }

    /* The response uri/cid are unused, so the body is discarded */
    rc = do_request(t, "POST", EP_CREATE_RECORD, sess->access_jwt, json,
                    NULL, status, errmsg);

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (json != NULL) {
        cJSON_free(json);
    }
    return rc;
}

static int atp_upload_blob_impl(const pspsky_transport_t *t,
                                const pspsky_session_t *sess,
                                const unsigned char *data, size_t len,
                                const char *mime_type,
                                pspsky_blob_t *out,
                                char *errmsg,
                                int *status)
{
    cJSON *root = NULL;
    char  *body = NULL;
    const cJSON *blob;
    const cJSON *ref;
    const cJSON *size_it;
    const char  *link;
    const char  *mime;
    int rc;

    if (errmsg != NULL) {
        errmsg[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    /* Bad arguments must not reach the transport */
    if (t == NULL || t->perform == NULL || sess == NULL ||
        data == NULL || out == NULL ||
        len == 0 || len > (size_t)PSPSKY_IMAGE_MAX_BYTES ||
        mime_type == NULL || mime_type[0] == '\0') {
        return PSPSKY_E_ARG;
    }

    memset(out, 0, sizeof(*out));

    rc = do_request_ex(t, "POST", EP_UPLOAD_BLOB, sess->access_jwt, NULL,
                       data, len, mime_type, &body, status, errmsg);
    if (rc != PSPSKY_OK) {
        goto cleanup;
    }
    if (body == NULL) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }

    root = cJSON_Parse(body);
    if (root == NULL) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }

    blob = json_obj(root, "blob");
    if (blob == NULL) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }
    ref = json_obj(blob, "ref");
    if (ref == NULL) {
        rc = PSPSKY_E_PARSE;
        goto cleanup;
    }
    link = json_str(ref, "$link");
    if (link == NULL || link[0] == '\0' || strlen(link) >= sizeof(out->cid)) {
        rc = PSPSKY_E_PARSE;    /* cid is required */
        goto cleanup;
    }
    (void)copy_exact(out->cid, sizeof(out->cid), link);

    /* mimeType is optional; fall back to the argument */
    mime = json_str(blob, "mimeType");
    if (mime == NULL || mime[0] == '\0' || strlen(mime) >= sizeof(out->mime_type)) {
        mime = mime_type;
    }
    copy_utf8_trunc(out->mime_type, sizeof(out->mime_type), mime);

    /* size is optional; fall back to the bytes sent */
    size_it = cJSON_GetObjectItemCaseSensitive(blob, "size");
    if (cJSON_IsNumber(size_it)) {
        out->size = (long long)size_it->valuedouble;
    } else {
        out->size = (long long)len;
    }

    rc = PSPSKY_OK;

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    free(body);
    if (rc != PSPSKY_OK && out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Low-level API (public)                                              */
/* ------------------------------------------------------------------ */

int pspsky_atp_create_session(const pspsky_transport_t *t,
                              const char *identifier,
                              const char *app_password,
                              pspsky_session_t *out,
                              char *errmsg)
{
    int status = 0;
    return atp_create_session_impl(t, identifier, app_password, out, errmsg, &status);
}

int pspsky_atp_refresh_session(const pspsky_transport_t *t,
                               pspsky_session_t *sess,
                               char *errmsg)
{
    int status = 0;
    return atp_refresh_session_impl(t, sess, errmsg, &status);
}

int pspsky_atp_get_timeline(const pspsky_transport_t *t,
                            const pspsky_session_t *sess,
                            int limit,
                            const char *cursor,
                            pspsky_timeline_t *out,
                            char *errmsg)
{
    int status = 0;
    return atp_get_timeline_impl(t, sess, limit, cursor, out, errmsg, &status);
}

/* Compatibility wrapper for the _ex variant with image = NULL */
int pspsky_atp_create_post(const pspsky_transport_t *t,
                           const pspsky_session_t *sess,
                           const char *text_utf8,
                           const char *created_at_iso,
                           char *errmsg)
{
    return pspsky_atp_create_post_ex(t, sess, text_utf8, created_at_iso, NULL,
                                     errmsg);
}

int pspsky_atp_create_post_ex(const pspsky_transport_t *t,
                              const pspsky_session_t *sess,
                              const char *text_utf8,
                              const char *created_at_iso,
                              const pspsky_embed_image_t *image,
                              char *errmsg)
{
    int status = 0;
    return atp_create_post_impl(t, sess, text_utf8, created_at_iso, image,
                                errmsg, &status);
}

int pspsky_atp_upload_blob(const pspsky_transport_t *t,
                           const pspsky_session_t *sess,
                           const unsigned char *data, size_t len,
                           const char *mime_type,
                           pspsky_blob_t *out,
                           char *errmsg)
{
    int status = 0;
    return atp_upload_blob_impl(t, sess, data, len, mime_type, out,
                                errmsg, &status);
}

/* ------------------------------------------------------------------ */
/* High-level client                                                   */
/* ------------------------------------------------------------------ */

static void client_record(pspsky_client_t *c, int rc, int status, const char *msg)
{
    c->last_http_status = status;
    if (rc == PSPSKY_OK) {
        c->last_error_msg[0] = '\0';
    } else {
        copy_utf8_trunc(c->last_error_msg, sizeof(c->last_error_msg), msg);
    }
}

int pspsky_client_login(pspsky_client_t *c, const pspsky_transport_t *t,
                        const char *identifier, const char *app_password)
{
    char errmsg[PSPSKY_ERRMSG_MAX];
    int status = 0;
    int rc;

    if (c == NULL) {
        return PSPSKY_E_ARG;
    }
    memset(c, 0, sizeof(*c));
    if (t == NULL || t->perform == NULL) {
        return PSPSKY_E_ARG;
    }
    c->transport = t;

    errmsg[0] = '\0';
    rc = atp_create_session_impl(t, identifier, app_password, &c->session,
                                 errmsg, &status);
    client_record(c, rc, status, errmsg);
    return rc;
}

/* Returns 1 on success; on failure sets *out_rc/errmsg/status for the caller */
static int client_try_refresh(pspsky_client_t *c, int *out_rc,
                              char *errmsg, int *status)
{
    char rerr[PSPSKY_ERRMSG_MAX];
    int rstatus = 0;
    int rrc;

    rerr[0] = '\0';
    rrc = atp_refresh_session_impl(c->transport, &c->session, rerr, &rstatus);
    if (rrc == PSPSKY_OK) {
        return 1;
    }
    /* Refresh failed; a full re-login is required */
    *out_rc  = PSPSKY_E_AUTH_EXPIRED;
    *status  = rstatus;
    if (rerr[0] != '\0') {
        memcpy(errmsg, rerr, sizeof(rerr));
    }
    return 0;
}

int pspsky_client_get_timeline(pspsky_client_t *c, int limit,
                               const char *cursor, pspsky_timeline_t *out)
{
    char errmsg[PSPSKY_ERRMSG_MAX];
    int status = 0;
    int rc;

    if (c == NULL || out == NULL) {
        return PSPSKY_E_ARG;
    }
    if (c->transport == NULL || c->transport->perform == NULL) {
        client_record(c, PSPSKY_E_ARG, 0, "");
        return PSPSKY_E_ARG;
    }

    errmsg[0] = '\0';
    rc = atp_get_timeline_impl(c->transport, &c->session, limit, cursor, out,
                               errmsg, &status);
    if (rc == PSPSKY_E_AUTH_EXPIRED) {
        if (!client_try_refresh(c, &rc, errmsg, &status)) {
            client_record(c, rc, status, errmsg);
            return rc;
        }
        errmsg[0] = '\0';
        status = 0;
        rc = atp_get_timeline_impl(c->transport, &c->session, limit, cursor, out,
                                   errmsg, &status);   /* retried exactly once */
    }

    client_record(c, rc, status, errmsg);
    return rc;
}

/* Compatibility wrapper for the _ex variant with image = NULL */
int pspsky_client_create_post(pspsky_client_t *c, const char *text_utf8,
                              const char *created_at_iso)
{
    return pspsky_client_create_post_ex(c, text_utf8, created_at_iso, NULL);
}

int pspsky_client_create_post_ex(pspsky_client_t *c, const char *text_utf8,
                                 const char *created_at_iso,
                                 const pspsky_embed_image_t *image)
{
    char errmsg[PSPSKY_ERRMSG_MAX];
    int status = 0;
    int rc;

    if (c == NULL) {
        return PSPSKY_E_ARG;
    }
    if (c->transport == NULL || c->transport->perform == NULL) {
        client_record(c, PSPSKY_E_ARG, 0, "");
        return PSPSKY_E_ARG;
    }

    errmsg[0] = '\0';
    rc = atp_create_post_impl(c->transport, &c->session, text_utf8,
                              created_at_iso, image, errmsg, &status);
    if (rc == PSPSKY_E_AUTH_EXPIRED) {
        if (!client_try_refresh(c, &rc, errmsg, &status)) {
            client_record(c, rc, status, errmsg);
            return rc;
        }
        errmsg[0] = '\0';
        status = 0;
        rc = atp_create_post_impl(c->transport, &c->session, text_utf8,
                                  created_at_iso, image, errmsg, &status);
    }

    client_record(c, rc, status, errmsg);
    return rc;
}

int pspsky_client_upload_blob(pspsky_client_t *c, const unsigned char *data,
                              size_t len, const char *mime_type,
                              pspsky_blob_t *out)
{
    char errmsg[PSPSKY_ERRMSG_MAX];
    int status = 0;
    int rc;

    if (c == NULL || out == NULL) {
        return PSPSKY_E_ARG;
    }
    if (c->transport == NULL || c->transport->perform == NULL) {
        client_record(c, PSPSKY_E_ARG, 0, "");
        return PSPSKY_E_ARG;
    }

    errmsg[0] = '\0';
    rc = atp_upload_blob_impl(c->transport, &c->session, data, len, mime_type,
                              out, errmsg, &status);
    if (rc == PSPSKY_E_AUTH_EXPIRED) {
        if (!client_try_refresh(c, &rc, errmsg, &status)) {
            client_record(c, rc, status, errmsg);
            return rc;
        }
        errmsg[0] = '\0';
        status = 0;
        rc = atp_upload_blob_impl(c->transport, &c->session, data, len,
                                  mime_type, out, errmsg, &status);
    }

    client_record(c, rc, status, errmsg);
    return rc;
}
