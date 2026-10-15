#ifndef PSPSKY_TRANSPORT_H
#define PSPSKY_TRANSPORT_H

#include <stddef.h>

#define PSPSKY_HTTP_ERRMSG_MAX 128
#define PSPSKY_HTTP_BODY_MAX   (512 * 1024)

/*
 * Zero the whole struct before setting fields.
 * body and body_bin are mutually exclusive.
 */
typedef struct {
    const char *method;  /* "GET" or "POST" */
    const char *url;
    const char *bearer;  /* NULL = no Authorization header */
    const char *body;  /* JSON string; sent as application/json */
    const unsigned char *body_bin;  /* raw bytes, sent verbatim */
    size_t body_bin_len;
    const char *content_type;  /* required with body_bin */
    int timeout_sec;  /* 0 = implementation default */
} pspsky_http_req_t;

typedef struct {
    int    status;  /* 0 on transport failure */
    char  *body;  /* malloc'd, NUL-terminated; caller frees */
    size_t body_len;  /* excluding NUL */
    char   errmsg[PSPSKY_HTTP_ERRMSG_MAX];  /* empty on success */
} pspsky_http_resp_t;

/*
 * perform returns PSPSKY_OK for a completed exchange, including HTTP errors.
 * Transport failures return E_TLS, E_TRANSPORT, or E_NOMEM.
 */
typedef struct pspsky_transport {
    void *ctx;
    int (*perform)(void *ctx, const pspsky_http_req_t *req, pspsky_http_resp_t *resp);
} pspsky_transport_t;

#endif /* PSPSKY_TRANSPORT_H */
