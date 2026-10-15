#include "pspsky/error.h"
#include "pspsky/strings.h"

const char *pspsky_strerror(int err)
{
    switch (err) {
    case PSPSKY_OK:
        return pspsky_tr(PSPSKY_STR_ERR_OK);
    case PSPSKY_E_ARG:
        return pspsky_tr(PSPSKY_STR_ERR_ARG);
    case PSPSKY_E_NOMEM:
        return pspsky_tr(PSPSKY_STR_ERR_NOMEM);
    case PSPSKY_E_TRANSPORT:
        return pspsky_tr(PSPSKY_STR_ERR_TRANSPORT);
    case PSPSKY_E_TLS:
        return pspsky_tr(PSPSKY_STR_ERR_TLS);
    case PSPSKY_E_HTTP:
        return pspsky_tr(PSPSKY_STR_ERR_HTTP);
    case PSPSKY_E_AUTH:
        return pspsky_tr(PSPSKY_STR_ERR_AUTH);
    case PSPSKY_E_AUTH_EXPIRED:
        return pspsky_tr(PSPSKY_STR_ERR_AUTH_EXPIRED);
    case PSPSKY_E_PARSE:
        return pspsky_tr(PSPSKY_STR_ERR_PARSE);
    case PSPSKY_E_IO:
        return pspsky_tr(PSPSKY_STR_ERR_IO);
    case PSPSKY_E_NET_INIT:
        return pspsky_tr(PSPSKY_STR_ERR_NET_INIT);
    case PSPSKY_E_NET_CONNECT:
        return pspsky_tr(PSPSKY_STR_ERR_NET_CONNECT);
    case PSPSKY_E_CANCELLED:
        return pspsky_tr(PSPSKY_STR_ERR_CANCELLED);
    case PSPSKY_E_RATELIMIT:
        return pspsky_tr(PSPSKY_STR_ERR_RATELIMIT);
    default:
        return pspsky_tr(PSPSKY_STR_ERR_UNKNOWN);
    }
}
