/* libcurl (mbedTLS) implementation of pspsky_transport_t. PSP build only. */
#ifndef PSPSKY_TRANSPORT_CURL_H
#define PSPSKY_TRANSPORT_CURL_H

#include "pspsky/transport.h"

/* Keeps one CURL easy handle in ctx so the TLS session is reused across requests */
int pspsky_curl_transport_init(pspsky_transport_t *out, const char *cacert_path);

void pspsky_curl_transport_term(pspsky_transport_t *t);

#endif /* PSPSKY_TRANSPORT_CURL_H */
