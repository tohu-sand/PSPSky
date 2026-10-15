#ifndef PSPSKY_ERROR_H
#define PSPSKY_ERROR_H

/*
 * Only E_AUTH_EXPIRED permits a session refresh.
 * E_HTTP covers non-2xx responses without a more specific error.
 */
enum {
    PSPSKY_OK             =   0,
    PSPSKY_E_ARG          =  -1,
    PSPSKY_E_NOMEM        =  -2,  /* out of memory or buffer overflow */
    PSPSKY_E_TRANSPORT    =  -3,  /* DNS/TCP/timeout */
    PSPSKY_E_TLS          =  -4,  /* handshake or certificate verification */
    PSPSKY_E_HTTP         =  -5,
    PSPSKY_E_AUTH         =  -6,
    PSPSKY_E_AUTH_EXPIRED =  -7,  /* ExpiredToken */
    PSPSKY_E_PARSE        =  -8,  /* bad JSON or missing required field */
    PSPSKY_E_IO           =  -9,
    PSPSKY_E_NET_INIT     = -10,
    PSPSKY_E_NET_CONNECT  = -11,  /* APCTL connect failure or timeout */
    PSPSKY_E_CANCELLED    = -12,
    PSPSKY_E_RATELIMIT    = -13  /* HTTP 429 */
};

/* Static UTF-8 string in the current UI language; do not free */
const char *pspsky_strerror(int err);

#endif /* PSPSKY_ERROR_H */
