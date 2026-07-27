/* AT Protocol / Bluesky client API. Portable C99; HTTP is injected via
   pspsky_transport_t. */
#ifndef PSPSKY_ATPROTO_H
#define PSPSKY_ATPROTO_H

#include "pspsky/transport.h"

#define PSPSKY_HOST_URL        "https://bsky.social"
#define PSPSKY_MAX_JWT         2048
#define PSPSKY_MAX_DID         256
#define PSPSKY_MAX_HANDLE      256
#define PSPSKY_MAX_DISPLAYNAME 192
#define PSPSKY_MAX_TEXT        1024
#define PSPSKY_MAX_CREATEDAT   40
#define PSPSKY_MAX_CURSOR      128
#define PSPSKY_MAX_POSTS       20    /* max posts per getTimeline call */
#define PSPSKY_ERRMSG_MAX      160
#define PSPSKY_MAX_THUMB_URL   256
#define PSPSKY_MAX_LINK_RANGES 8

#define PSPSKY_MAX_BLOB_CID    256
#define PSPSKY_MAX_BLOB_MIME   64
#define PSPSKY_MAX_ALT         512
#define PSPSKY_IMAGE_MAX_BYTES 2000000  /* uploadBlob limit */

typedef struct {
    char access_jwt[PSPSKY_MAX_JWT];
    char refresh_jwt[PSPSKY_MAX_JWT];
    char did[PSPSKY_MAX_DID];
    char handle[PSPSKY_MAX_HANDLE];
} pspsky_session_t;

typedef struct {
    char display_name[PSPSKY_MAX_DISPLAYNAME]; /* empty = UI shows the handle */
    char handle[PSPSKY_MAX_HANDLE];
    char text[PSPSKY_MAX_TEXT];                /* UTF-8, truncated on a code point boundary */
    char created_at[PSPSKY_MAX_CREATEDAT];
    long long created_at_unix;                 /* UTC epoch seconds; -1 if unparsable */
    int  has_embed;                            /* 1 = record.embed present; contents never read */
    char thumb_url[PSPSKY_MAX_THUMB_URL];      /* first image thumb URL; empty = none */
    int  img_count;                            /* images in the embed view; 0 = none */
    int  img_aspect_w;                         /* aspectRatio of the first image; 0 = unknown */
    int  img_aspect_h;
    /* byte ranges into text (UTF-8) marking link facets; sorted by start */
    unsigned short link_start[PSPSKY_MAX_LINK_RANGES];
    unsigned short link_end[PSPSKY_MAX_LINK_RANGES];
    int  link_count;
} pspsky_post_t;

/* ~36KB, keep off the stack */
typedef struct {
    pspsky_post_t posts[PSPSKY_MAX_POSTS];
    int  count;
    char cursor[PSPSKY_MAX_CURSOR]; /* empty = no next page */
} pspsky_timeline_t;

/* Rebuilt verbatim into the record as
   {"$type":"blob","ref":{"$link":cid},"mimeType":...,"size":...} */
typedef struct {
    char      cid[PSPSKY_MAX_BLOB_CID];
    char      mime_type[PSPSKY_MAX_BLOB_MIME];
    long long size;
} pspsky_blob_t;

typedef struct {
    pspsky_blob_t blob;
    char alt[PSPSKY_MAX_ALT];       /* empty = ALT omitted */
    int  aspect_w;                  /* aspectRatio added only when both are > 0 */
    int  aspect_h;
} pspsky_embed_image_t;

/* --- Low-level API: one function = one request, no retries ---
   errmsg: optional PSPSKY_ERRMSG_MAX buffer receiving the server or transport message */
int pspsky_atp_create_session(const pspsky_transport_t *t,
                              const char *identifier,
                              const char *app_password,
                              pspsky_session_t *out,
                              char *errmsg);
int pspsky_atp_refresh_session(const pspsky_transport_t *t,
                               pspsky_session_t *sess,
                               char *errmsg);
/* limit clamped to [1, PSPSKY_MAX_POSTS]; NULL or empty cursor = newest */
int pspsky_atp_get_timeline(const pspsky_transport_t *t,
                            const pspsky_session_t *sess,
                            int limit,
                            const char *cursor,
                            pspsky_timeline_t *out,
                            char *errmsg);
/* Compatibility wrapper for create_post_ex with image = NULL */
int pspsky_atp_create_post(const pspsky_transport_t *t,
                           const pspsky_session_t *sess,
                           const char *text_utf8,
                           const char *created_at_iso,
                           char *errmsg);

/* GET url with no auth and a 15 s timeout. On HTTP 200 hands over the
   malloc'd body (caller frees); any other status maps to E_HTTP. */
int pspsky_atp_fetch_image(const pspsky_transport_t *t,
                           const char *url,
                           unsigned char **out, size_t *out_len,
                           char *errmsg);

/* POST com.atproto.repo.uploadBlob; data is sent verbatim as mime_type */
int pspsky_atp_upload_blob(const pspsky_transport_t *t,
                           const pspsky_session_t *sess,
                           const unsigned char *data, size_t len,
                           const char *mime_type,
                           pspsky_blob_t *out,
                           char *errmsg);

/* image non-NULL adds record.embed = app.bsky.embed.images with one entry */
int pspsky_atp_create_post_ex(const pspsky_transport_t *t,
                              const pspsky_session_t *sess,
                              const char *text_utf8,
                              const char *created_at_iso,
                              const pspsky_embed_image_t *image,
                              char *errmsg);

/* --- High-level client: on E_AUTH_EXPIRED, refresh and retry exactly once ---
   ~5KB, keep off the stack */
typedef struct {
    const pspsky_transport_t *transport;
    pspsky_session_t session;
    int  last_http_status;
    char last_error_msg[PSPSKY_ERRMSG_MAX]; /* empty on success */
} pspsky_client_t;

int pspsky_client_login(pspsky_client_t *c, const pspsky_transport_t *t,
                        const char *identifier, const char *app_password);
int pspsky_client_get_timeline(pspsky_client_t *c, int limit,
                               const char *cursor, pspsky_timeline_t *out);
/* Compatibility wrapper for create_post_ex with image = NULL */
int pspsky_client_create_post(pspsky_client_t *c, const char *text_utf8,
                              const char *created_at_iso);
int pspsky_client_upload_blob(pspsky_client_t *c, const unsigned char *data,
                              size_t len, const char *mime_type,
                              pspsky_blob_t *out);
int pspsky_client_create_post_ex(pspsky_client_t *c, const char *text_utf8,
                                 const char *created_at_iso,
                                 const pspsky_embed_image_t *image);

#endif /* PSPSKY_ATPROTO_H */
