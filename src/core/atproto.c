/* Engagement counts and viewer state must not be read or stored. */

#include <limits.h>
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

#define EP_CREATE_SESSION  PSPSKY_HOST_URL "/xrpc/com.atproto.server.createSession"
#define EP_REFRESH_SESSION PSPSKY_HOST_URL "/xrpc/com.atproto.server.refreshSession"
#define EP_GET_TIMELINE    PSPSKY_HOST_URL "/xrpc/app.bsky.feed.getTimeline"
#define EP_CREATE_RECORD   PSPSKY_HOST_URL "/xrpc/com.atproto.repo.createRecord"
#define EP_UPLOAD_BLOB     PSPSKY_HOST_URL "/xrpc/com.atproto.repo.uploadBlob"

#define POST_COLLECTION    "app.bsky.feed.post"
#define EMBED_IMAGES_TYPE  "app.bsky.embed.images"

#define EMBED_IMAGES_VIEW  "app.bsky.embed.images#view"
#define EMBED_RECORD_VIEW  "app.bsky.embed.record#view"
#define EMBED_RWM_VIEW     "app.bsky.embed.recordWithMedia#view"

#define QUOTE_RECORD       "app.bsky.embed.record#viewRecord"
#define QUOTE_NOT_FOUND    "app.bsky.embed.record#viewNotFound"
#define QUOTE_BLOCKED      "app.bsky.embed.record#viewBlocked"
#define QUOTE_DETACHED     "app.bsky.embed.record#viewDetached"

/* Only link facets are rendered. */
#define FACET_LINK_FEATURE "app.bsky.richtext.facet#link"

#define REASON_REPOST      "app.bsky.feed.defs#reasonRepost"
#define PARENT_BLOCKED     "app.bsky.feed.defs#blockedPost"

/* Use the 128px avatar preset to reduce transfer size. */
#define AVATAR_PRESET       "/img/avatar/"
#define AVATAR_THUMB_PRESET "/img/avatar_thumbnail/"

/* Allow for slow 802.11b transfers. */
#define ATP_IMAGE_TIMEOUT_SEC 15

/* Nonstandard record field used by clients to display "via PSPSky". */
#define PSPSKY_VIA_NAME    "PSPSky"

/* %XX encoding can grow the cursor up to 3x in the worst case */
#define ENC_CURSOR_CAP (3 * PSPSKY_MAX_CURSOR + 1)
/* "?limit=NN&cursor=" + the encoded cursor + slack */
#define URL_CAP        (sizeof(EP_GET_TIMELINE) + 32 + ENC_CURSOR_CAP)

static void trim_partial_utf8(char *s)
{
    size_t len = strlen(s);
    size_t k;

    for (k = 1; k <= 4 && k <= len; ++k) {
        unsigned char c = (unsigned char)s[len - k];
        size_t need;

        if ((c & 0xC0) == 0x80) {
            continue;
        }
        if (c < 0x80)                need = 1;
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else                         need = 1;  /* invalid lead byte: leave as is */

        if (need > k) {
            s[len - k] = '\0';
        }
        return;
    }
}

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

/* Tokens and identifiers must not be truncated. */
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

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsString(it) || it->valuestring == NULL) {
        return NULL;
    }
    return it->valuestring;
}

static const cJSON *json_obj(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsObject(it)) {
        return NULL;
    }
    return it;
}

/* 0 unless obj[key] is a positive integer representable as int */
static int json_pos_int(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsNumber(it) ||
        !(it->valuedouble >= 1.0 && it->valuedouble <= (double)INT_MAX) ||
        it->valuedouble != (double)it->valueint) {
        return 0;
    }
    return it->valueint;
}

/* -1 unless obj[key] is a number within [0, 65535] */
static long json_byte_offset(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsNumber(it) ||
        it->valuedouble < 0.0 || it->valuedouble > 65535.0) {
        return -1;
    }
    return (long)it->valuedouble;
}

/* body and body_bin are mutually exclusive. The caller frees *out_body on success. */
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

    if (rc != PSPSKY_OK) {
        set_errmsg(errmsg, terr[0] != '\0' ? terr : NULL);
        free(resp.body);
        return rc;
    }

    if (resp.status >= 200 && resp.status <= 299) {
        if (out_body != NULL) {
            *out_body = resp.body;
        } else {
            free(resp.body);
        }
        return PSPSKY_OK;
    }

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

static int do_request(const pspsky_transport_t *t,
                      const char *method, const char *url,
                      const char *bearer, const char *body,
                      char **out_body, int *out_status, char *errmsg)
{
    return do_request_ex(t, method, url, bearer, body, NULL, 0, NULL,
                         out_body, out_status, errmsg);
}

/* Validate all fields before replacing the session; failures must leave it intact. */
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
    /* Let the server classify empty tokens as authentication errors. */

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

/* Image views of an embed view, including the media of recordWithMedia. */
static const cJSON *embed_view_images(const cJSON *embed)
{
    const cJSON *images = NULL;
    const char  *type;

    if (!cJSON_IsObject(embed)) {
        return NULL;
    }
    type = json_str(embed, "$type");
    if (type == NULL) {
        return NULL;
    }

    if (strcmp(type, EMBED_IMAGES_VIEW) == 0) {
        images = cJSON_GetObjectItemCaseSensitive(embed, "images");
    } else if (strcmp(type, EMBED_RWM_VIEW) == 0) {
        const cJSON *media = json_obj(embed, "media");
        const char  *mtype = (media != NULL) ? json_str(media, "$type") : NULL;

        if (mtype != NULL && strcmp(mtype, EMBED_IMAGES_VIEW) == 0) {
            images = cJSON_GetObjectItemCaseSensitive(media, "images");
        }
    }

    return cJSON_IsArray(images) ? images : NULL;
}

/*
 * Returns the image count. Keep missing images in place so placeholders
 * preserve embed order.
 */
static int read_embed_images(const cJSON *embed, pspsky_post_image_t *out)
{
    const cJSON *images = embed_view_images(embed);
    int          n;
    int          i;

    if (images == NULL) {
        return 0;
    }
    n = cJSON_GetArraySize(images);
    if (n <= 0) {
        return 0;
    }
    if (n > PSPSKY_MAX_IMAGES) {
        n = PSPSKY_MAX_IMAGES;
    }

    for (i = 0; i < n; ++i) {
        const cJSON *entry = cJSON_GetArrayItem(images, i);
        const cJSON *ratio;
        const char  *thumb;
        pspsky_post_image_t *image = &out[i];
        int w;
        int h;

        if (!cJSON_IsObject(entry)) {
            continue;
        }
        /* Leave missing or overlong URLs empty for the placeholder. */
        thumb = json_str(entry, "thumb");
        if (thumb != NULL && thumb[0] != '\0') {
            (void)copy_exact(image->thumb_url, sizeof(image->thumb_url), thumb);
        }

        ratio = json_obj(entry, "aspectRatio");
        if (ratio == NULL) {
            continue;
        }
        w = json_pos_int(ratio, "width");
        h = json_pos_int(ratio, "height");
        if (w > 0 && h > 0) {
            image->aspect_w = w;
            image->aspect_h = h;
        }
    }
    return n;
}

static int facet_has_link(const cJSON *facet)
{
    const cJSON *features = cJSON_GetObjectItemCaseSensitive(facet, "features");
    const cJSON *feature;

    if (!cJSON_IsArray(features)) {
        return 0;
    }
    cJSON_ArrayForEach(feature, features) {
        const char *type = json_str(feature, "$type");

        if (type != NULL && strcmp(type, FACET_LINK_FEATURE) == 0) {
            return 1;
        }
    }
    return 0;
}

static void sort_link_ranges(pspsky_post_t *p)
{
    int i;

    for (i = 1; i < p->link_count; ++i) {
        unsigned short s = p->link_start[i];
        unsigned short e = p->link_end[i];
        int j = i - 1;

        while (j >= 0 && p->link_start[j] > s) {
            p->link_start[j + 1] = p->link_start[j];
            p->link_end[j + 1]   = p->link_end[j];
            --j;
        }
        p->link_start[j + 1] = s;
        p->link_end[j + 1]   = e;
    }
}

/* Store text first: facet offsets are clipped to its truncated length. */
static void post_read_link_facets(const cJSON *record, pspsky_post_t *p)
{
    const cJSON *facets = cJSON_GetObjectItemCaseSensitive(record, "facets");
    const cJSON *facet;
    size_t len = strlen(p->text);

    if (!cJSON_IsArray(facets)) {
        return;
    }

    cJSON_ArrayForEach(facet, facets) {
        const cJSON *index;
        long start;
        long end;

        if (p->link_count >= PSPSKY_MAX_LINK_RANGES) {
            break;  /* the rest is dropped, not an error */
        }
        if (!facet_has_link(facet)) {
            continue;
        }
        index = json_obj(facet, "index");
        if (index == NULL) {
            continue;
        }
        start = json_byte_offset(index, "byteStart");
        end   = json_byte_offset(index, "byteEnd");
        if (start < 0 || end < 0 || start >= end) {
            continue;
        }
        if ((size_t)start >= len) {
            continue;
        }
        if ((size_t)end > len) {
            end = (long)len;
        }
        p->link_start[p->link_count] = (unsigned short)start;
        p->link_end[p->link_count]   = (unsigned short)end;
        p->link_count++;
    }

    sort_link_ranges(p);
}

/* Overlong URLs leave avatar_url empty so the UI uses the default avatar. */
static void post_read_avatar(const cJSON *author, pspsky_post_t *p)
{
    const char *url = json_str(author, "avatar");
    const char *preset;
    int         n;

    if (url == NULL || url[0] == '\0') {
        return;
    }
    preset = strstr(url, AVATAR_PRESET);
    if (preset == NULL) {
        (void)copy_exact(p->avatar_url, sizeof(p->avatar_url), url);
        return;
    }
    n = snprintf(p->avatar_url, sizeof(p->avatar_url), "%.*s%s%s",
                 (int)(preset - url), url, AVATAR_THUMB_PRESET,
                 preset + strlen(AVATAR_PRESET));
    if (n < 0 || (size_t)n >= sizeof(p->avatar_url)) {
        p->avatar_url[0] = '\0';
    }
}

/* Display name, else handle, of a profile view; NULL when it has neither */
static const char *profile_name(const cJSON *profile)
{
    const char *name = json_str(profile, "displayName");

    if (name == NULL || name[0] == '\0') {
        name = json_str(profile, "handle");
    }
    return (name != NULL && name[0] != '\0') ? name : NULL;
}

static int profile_is_self(const cJSON *profile, const char *self_did)
{
    const char *did = json_str(profile, "did");

    return (self_did != NULL && self_did[0] != '\0' &&
            did != NULL && strcmp(did, self_did) == 0);
}

/* Only repost reasons are displayed. */
static void post_read_reason(const cJSON *item, const char *self_did,
                             pspsky_post_t *p)
{
    const cJSON *reason = json_obj(item, "reason");
    const cJSON *by;
    const char  *type;
    const char  *name;

    if (reason == NULL) {
        return;
    }
    type = json_str(reason, "$type");
    by   = json_obj(reason, "by");
    if (type == NULL || strcmp(type, REASON_REPOST) != 0 || by == NULL) {
        return;
    }
    if (profile_is_self(by, self_did)) {
        p->repost = PSPSKY_REPOST_BY_SELF;
        return;
    }
    name = profile_name(by);
    if (name == NULL) {
        return;
    }
    copy_utf8_trunc(p->repost_by, sizeof(p->repost_by), name);
    p->repost = PSPSKY_REPOST_BY;
}

/* A reply without a hydrated parent still needs a reply indicator. */
static void post_read_reply(const cJSON *item, const cJSON *record,
                            const char *self_did, pspsky_post_t *p)
{
    const cJSON *reply  = json_obj(item, "reply");
    const cJSON *parent = (reply != NULL) ? json_obj(reply, "parent") : NULL;
    const cJSON *author;
    const char  *type;
    const char  *name;

    if (parent == NULL) {
        if (json_obj(record, "reply") != NULL) {
            p->reply = PSPSKY_REPLY_TO_POST;
        }
        return;
    }
    type = json_str(parent, "$type");
    if (type != NULL && strcmp(type, PARENT_BLOCKED) == 0) {
        p->reply = PSPSKY_REPLY_TO_BLOCKED;
        return;
    }
    author = json_obj(parent, "author");
    if (author != NULL && profile_is_self(author, self_did)) {
        p->reply = PSPSKY_REPLY_TO_SELF;
        return;
    }
    name = (author != NULL) ? profile_name(author) : NULL;
    if (name == NULL) {
        p->reply = PSPSKY_REPLY_TO_POST;  /* notFoundPost has no author */
        return;
    }
    copy_utf8_trunc(p->reply_to, sizeof(p->reply_to), name);
    p->reply = PSPSKY_REPLY_TO;
}

/* recordWithMedia nests a record#view whose $type is optional. */
static const cJSON *embed_view_quote(const cJSON *embed)
{
    const char *type = json_str(embed, "$type");

    if (type == NULL) {
        return NULL;
    }
    if (strcmp(type, EMBED_RWM_VIEW) == 0) {
        embed = json_obj(embed, "record");
    } else if (strcmp(type, EMBED_RECORD_VIEW) != 0) {
        return NULL;
    }
    return (embed != NULL) ? json_obj(embed, "record") : NULL;
}

/*
 * viewRecord also carries engagement counts and viewer state; read only the
 * fields kept in pspsky_quote_t.
 */
static void post_read_quote(const cJSON *post, pspsky_post_t *p)
{
    const cJSON *view = embed_view_quote(json_obj(post, "embed"));
    const cJSON *author;
    const cJSON *value;
    const cJSON *embeds;
    pspsky_quote_t *q = &p->quoted;
    const char  *type;
    const char  *handle;
    const char  *text;
    const char  *created;

    type = (view != NULL) ? json_str(view, "$type") : NULL;
    if (type == NULL) {
        return;
    }
    if (strcmp(type, QUOTE_NOT_FOUND) == 0) {
        p->quote = PSPSKY_QUOTE_NOT_FOUND;
        return;
    }
    if (strcmp(type, QUOTE_BLOCKED) == 0) {
        p->quote = PSPSKY_QUOTE_BLOCKED;
        return;
    }
    if (strcmp(type, QUOTE_DETACHED) == 0) {
        p->quote = PSPSKY_QUOTE_DETACHED;
        return;
    }
    if (strcmp(type, QUOTE_RECORD) != 0) {
        return;  /* feeds, lists and other records are not shown */
    }

    author = json_obj(view, "author");
    value  = json_obj(view, "value");
    handle = (author != NULL) ? json_str(author, "handle") : NULL;
    text   = (value != NULL) ? json_str(value, "text") : NULL;
    if (handle == NULL || handle[0] == '\0' || text == NULL) {
        p->quote = PSPSKY_QUOTE_NOT_FOUND;
        return;
    }

    copy_utf8_trunc(q->handle,       sizeof(q->handle),       handle);
    copy_utf8_trunc(q->display_name, sizeof(q->display_name),
                    json_str(author, "displayName"));
    copy_utf8_trunc(q->text,         sizeof(q->text),         text);
    created = json_str(value, "createdAt");
    q->created_at_unix = (created != NULL) ? pspsky_iso8601_parse(created) : -1;

    /* A post has one embed at most; a nested quote is reduced to its media. */
    embeds = cJSON_GetObjectItemCaseSensitive(view, "embeds");
    if (cJSON_IsArray(embeds)) {
        q->img_count = read_embed_images(cJSON_GetArrayItem(embeds, 0),
                                         q->images);
    }

    p->quote = PSPSKY_QUOTE_POST;
}

/* Missing handle or text makes the caller skip this item. self_did may be NULL. */
static int post_from_feed_item(const cJSON *item, const char *self_did,
                               pspsky_post_t *p)
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

    display = json_str(author, "displayName");
    created = json_str(record, "createdAt");

    memset(p, 0, sizeof(*p));

    /* Check presence only; quote contents may contain engagement counts. */
    p->has_embed =
        (cJSON_GetObjectItemCaseSensitive(record, "embed") != NULL) ? 1 : 0;

    /* Thumbnail URLs and quotes come from the hydrated view, outside record. */
    p->img_count = read_embed_images(json_obj(post, "embed"), p->images);
    post_read_quote(post, p);

    copy_utf8_trunc(p->handle,       sizeof(p->handle),       handle);
    copy_utf8_trunc(p->display_name, sizeof(p->display_name), display);
    copy_utf8_trunc(p->text,         sizeof(p->text),         text);
    copy_utf8_trunc(p->created_at,   sizeof(p->created_at),   created);

    post_read_link_facets(record, p);

    post_read_avatar(author, p);
    post_read_reason(item, self_did, p);
    post_read_reply(item, record, self_did, p);

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

        if (post_from_feed_item(item, sess->did,
                                &out->posts[out->count]) == PSPSKY_OK) {
            out->count++;
        }
    }

    cur = json_str(root, "cursor");
    if (cur != NULL && cur[0] != '\0' && strlen(cur) < sizeof(out->cursor)) {
        (void)copy_exact(out->cursor, sizeof(out->cursor), cur);
    } else {
        out->cursor[0] = '\0';  /* missing or too long becomes empty */
    }

    rc = PSPSKY_OK;

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    free(body);
    return rc;
}

/* The caller owns the returned tree. Attached children are freed with their parent. */
static cJSON *build_embed_images(const pspsky_embed_image_t *images,
                                 int image_count)
{
    cJSON *embed   = NULL;
    cJSON *entries = NULL;
    int i;

    embed = cJSON_CreateObject();
    if (embed == NULL) {
        return NULL;
    }
    if (cJSON_AddStringToObject(embed, "$type", EMBED_IMAGES_TYPE) == NULL) {
        goto fail;
    }

    entries = cJSON_CreateArray();
    if (entries == NULL || !cJSON_AddItemToObject(embed, "images", entries)) {
        cJSON_Delete(entries);
        goto fail;
    }

    for (i = 0; i < image_count; ++i) {
        const pspsky_embed_image_t *image = &images[i];
        cJSON *entry;
        cJSON *blob;
        cJSON *ref;
        cJSON *aspect;

        entry = cJSON_CreateObject();
        if (entry == NULL || !cJSON_AddItemToArray(entries, entry)) {
            cJSON_Delete(entry);
            goto fail;
        }

        /* alt is required, and an empty string is a valid value */
        if (cJSON_AddStringToObject(entry, "alt", image->alt) == NULL) {
            goto fail;
        }

        blob = cJSON_CreateObject();
        if (blob == NULL || !cJSON_AddItemToObject(entry, "image", blob)) {
            cJSON_Delete(blob);
            goto fail;
        }

        if (cJSON_AddStringToObject(blob, "$type", "blob") == NULL) {
            goto fail;
        }
        ref = cJSON_CreateObject();
        if (ref == NULL || !cJSON_AddItemToObject(blob, "ref", ref)) {
            cJSON_Delete(ref);
            goto fail;
        }

        if (cJSON_AddStringToObject(ref, "$link", image->blob.cid) == NULL ||
            cJSON_AddStringToObject(blob, "mimeType", image->blob.mime_type) == NULL ||
            cJSON_AddNumberToObject(blob, "size", (double)image->blob.size) == NULL) {
            goto fail;
        }

        if (image->aspect_w > 0 && image->aspect_h > 0) {
            aspect = cJSON_CreateObject();
            if (aspect == NULL || !cJSON_AddItemToObject(entry, "aspectRatio", aspect)) {
                cJSON_Delete(aspect);
                goto fail;
            }

            if (cJSON_AddNumberToObject(aspect, "width", (double)image->aspect_w) == NULL ||
                cJSON_AddNumberToObject(aspect, "height", (double)image->aspect_h) == NULL) {
                goto fail;
            }
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
                                const pspsky_embed_image_t *images,
                                int image_count,
                                char *errmsg,
                                int *status)
{
    cJSON *root   = NULL;
    cJSON *record = NULL;
    char  *json   = NULL;
    char   isobuf[32];
    const char *iso;
    int rc;
    int i;

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
        return PSPSKY_E_ARG;
    }
    if (image_count < 0 || image_count > PSPSKY_MAX_IMAGES ||
        (image_count > 0 && images == NULL)) {
        return PSPSKY_E_ARG;
    }
    for (i = 0; i < image_count; ++i) {
        if (images[i].blob.cid[0] == '\0') {
            return PSPSKY_E_ARG;
        }
    }

    iso = created_at_iso;
    if (iso == NULL || iso[0] == '\0') {
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
    /* root owns record after attachment. */

    if (cJSON_AddStringToObject(root, "repo", sess->did) == NULL ||
        cJSON_AddStringToObject(root, "collection", POST_COLLECTION) == NULL ||
        cJSON_AddStringToObject(record, "$type", POST_COLLECTION) == NULL ||
        cJSON_AddStringToObject(record, "text", text_utf8) == NULL ||
        cJSON_AddStringToObject(record, "createdAt", iso) == NULL ||
        cJSON_AddStringToObject(record, "via", PSPSKY_VIA_NAME) == NULL) {
        rc = PSPSKY_E_NOMEM;
        goto cleanup;
    }

    if (image_count > 0) {
        cJSON *embed = build_embed_images(images, image_count);

        if (embed == NULL || !cJSON_AddItemToObject(record, "embed", embed)) {
            cJSON_Delete(embed);
            rc = PSPSKY_E_NOMEM;
            goto cleanup;
        }
        /* record owns embed after attachment. */
    }

    json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        rc = PSPSKY_E_NOMEM;
        goto cleanup;
    }

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
        rc = PSPSKY_E_PARSE;
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

int pspsky_atp_create_post(const pspsky_transport_t *t,
                           const pspsky_session_t *sess,
                           const char *text_utf8,
                           const char *created_at_iso,
                           char *errmsg)
{
    return pspsky_atp_create_post_ex(t, sess, text_utf8, created_at_iso, NULL, 0,
                                     errmsg);
}

int pspsky_atp_create_post_ex(const pspsky_transport_t *t,
                              const pspsky_session_t *sess,
                              const char *text_utf8,
                              const char *created_at_iso,
                              const pspsky_embed_image_t *images,
                              int image_count,
                              char *errmsg)
{
    int status = 0;
    return atp_create_post_impl(t, sess, text_utf8, created_at_iso, images,
                                image_count, errmsg, &status);
}

/* Public CDN requests do not use authentication or session refresh. */
int pspsky_atp_fetch_image(const pspsky_transport_t *t,
                           const char *url,
                           unsigned char **out, size_t *out_len,
                           char *errmsg)
{
    pspsky_http_req_t  req;
    pspsky_http_resp_t resp;
    char terr[PSPSKY_HTTP_ERRMSG_MAX + 1];
    int  rc;

    if (errmsg != NULL) {
        errmsg[0] = '\0';
    }
    if (out != NULL) {
        *out = NULL;
    }
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (t == NULL || t->perform == NULL || url == NULL || url[0] == '\0' ||
        out == NULL || out_len == NULL) {
        return PSPSKY_E_ARG;
    }

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    req.method      = "GET";
    req.url         = url;
    req.timeout_sec = ATP_IMAGE_TIMEOUT_SEC;

    rc = t->perform(t->ctx, &req, &resp);

    /* The transport errmsg is not guaranteed NUL-terminated */
    memcpy(terr, resp.errmsg, sizeof(resp.errmsg));
    terr[PSPSKY_HTTP_ERRMSG_MAX] = '\0';

    if (rc != PSPSKY_OK) {
        set_errmsg(errmsg, terr[0] != '\0' ? terr : NULL);
        free(resp.body);
        return rc;
    }

    if (resp.status != 200) {
        char fallback[32];

        snprintf(fallback, sizeof(fallback), "HTTP %d", resp.status);
        set_errmsg(errmsg, fallback);
        free(resp.body);
        return PSPSKY_E_HTTP;
    }

    /* The caller owns the body; use body_len because images may contain NUL bytes. */
    *out     = (unsigned char *)resp.body;
    *out_len = resp.body_len;
    return PSPSKY_OK;
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
                                   errmsg, &status);
    }

    client_record(c, rc, status, errmsg);
    return rc;
}

int pspsky_client_create_post(pspsky_client_t *c, const char *text_utf8,
                              const char *created_at_iso)
{
    return pspsky_client_create_post_ex(c, text_utf8, created_at_iso, NULL, 0);
}

int pspsky_client_create_post_ex(pspsky_client_t *c, const char *text_utf8,
                                 const char *created_at_iso,
                                 const pspsky_embed_image_t *images,
                                 int image_count)
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
                              created_at_iso, images, image_count, errmsg, &status);
    if (rc == PSPSKY_E_AUTH_EXPIRED) {
        if (!client_try_refresh(c, &rc, errmsg, &status)) {
            client_record(c, rc, status, errmsg);
            return rc;
        }
        errmsg[0] = '\0';
        status = 0;
        rc = atp_create_post_impl(c->transport, &c->session, text_utf8,
                                  created_at_iso, images, image_count, errmsg, &status);
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
