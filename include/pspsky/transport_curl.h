#ifndef PSPSKY_TRANSPORT_CURL_H
#define PSPSKY_TRANSPORT_CURL_H

#include "pspsky/transport.h"

/* Reuse one CURL handle in ctx to preserve the TLS session. */
int pspsky_curl_transport_init(pspsky_transport_t *out, const char *cacert_path);

void pspsky_curl_transport_term(pspsky_transport_t *t);

#endif /* PSPSKY_TRANSPORT_CURL_H */
