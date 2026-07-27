/*
 * PSPSky - PSP network stack init and access point connection.
 * The init order below is mandatory: modules, then net, inet, resolver, apctl.
 * Rollback and teardown must run in exactly the reverse order.
 */

#include <stdio.h>

#include <pspkernel.h>
#include <psputility.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>

#include "pspsky/error.h"
#include "pspsky/net.h"
#include "pspsky/strings.h"

/* APCTL state polling interval. */
#define POLL_INTERVAL_US 100000 /* 100ms */
#define POLL_INTERVAL_MS 100

/* Guards connect and term. */
static int g_net_inited = 0;
/* Set once connect has reached GOT_IP. */
static int g_connected = 0;

/* ----------------------------------------------------------------- init */

int pspsky_net_init(void)
{
    int err;

    if (g_net_inited) {
        return PSPSKY_OK;
    }

    err = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (err < 0) {
        printf("pspsky_net: sceUtilityLoadNetModule(COMMON) = 0x%08X\n",
               (unsigned int)err);
        return PSPSKY_E_NET_INIT;
    }

    err = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (err < 0) {
        printf("pspsky_net: sceUtilityLoadNetModule(INET) = 0x%08X\n",
               (unsigned int)err);
        goto rollback_mod_common;
    }

    err = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    if (err < 0) {
        printf("pspsky_net: sceNetInit = 0x%08X\n", (unsigned int)err);
        goto rollback_mod_inet;
    }

    err = sceNetInetInit();
    if (err < 0) {
        printf("pspsky_net: sceNetInetInit = 0x%08X\n", (unsigned int)err);
        goto rollback_net;
    }

    err = sceNetResolverInit();
    if (err < 0) {
        printf("pspsky_net: sceNetResolverInit = 0x%08X\n", (unsigned int)err);
        goto rollback_inet;
    }

    err = sceNetApctlInit(0x8000, 48);
    if (err < 0) {
        printf("pspsky_net: sceNetApctlInit = 0x%08X\n", (unsigned int)err);
        goto rollback_resolver;
    }

    g_net_inited = 1;
    g_connected = 0;
    return PSPSKY_OK;

    /* Unwind only what succeeded, in reverse order. */
rollback_resolver:
    sceNetResolverTerm();
rollback_inet:
    sceNetInetTerm();
rollback_net:
    sceNetTerm();
rollback_mod_inet:
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
rollback_mod_common:
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    return PSPSKY_E_NET_INIT;
}

/* -------------------------------------------------------------- connect */

/* Progress text, or NULL; static, so callers can dedupe by pointer. */
static const char *state_message(int state)
{
    switch (state) {
    case PSP_NET_APCTL_STATE_SCANNING:
    case PSP_NET_APCTL_STATE_JOINING:
    case PSP_NET_APCTL_STATE_EAP_AUTH:
    case PSP_NET_APCTL_STATE_KEY_EXCHANGE:
        return pspsky_tr(PSPSKY_STR_NET_CONNECTING_AP);
    case PSP_NET_APCTL_STATE_GETTING_IP:
        return pspsky_tr(PSPSKY_STR_NET_GETTING_IP);
    case PSP_NET_APCTL_STATE_GOT_IP:
        return pspsky_tr(PSPSKY_STR_NET_CONNECTED);
    default:
        return NULL;
    }
}

int pspsky_net_connect(int conf_index, int timeout_sec,
                       pspsky_net_progress_cb cb, void *user)
{
    int err;
    int last_state = -1;
    int elapsed_ms = 0;
    int timeout_ms;
    const char *last_msg = NULL;

    if (!g_net_inited) {
        printf("pspsky_net: connect before init\n");
        return PSPSKY_E_NET_CONNECT;
    }
    if (conf_index <= 0) {
        printf("pspsky_net: invalid conf_index %d\n", conf_index);
        return PSPSKY_E_NET_CONNECT;
    }
    if (timeout_sec <= 0) {
        timeout_sec = 1; /* poll for at least one second */
    }
    timeout_ms = timeout_sec * 1000;

    err = sceUtilityCheckNetParam(conf_index);
    if (err != 0) {
        printf("pspsky_net: sceUtilityCheckNetParam(%d) = 0x%08X\n",
               conf_index, (unsigned int)err);
        return PSPSKY_E_NET_CONNECT;
    }

    err = sceNetApctlConnect(conf_index);
    if (err != 0) {
        printf("pspsky_net: sceNetApctlConnect(%d) = 0x%08X\n",
               conf_index, (unsigned int)err);
        return PSPSKY_E_NET_CONNECT;
    }

    for (;;) {
        int state = PSP_NET_APCTL_STATE_DISCONNECTED;

        err = sceNetApctlGetState(&state);
        if (err != 0) {
            printf("pspsky_net: sceNetApctlGetState = 0x%08X\n",
                   (unsigned int)err);
            break;
        }

        if (state != last_state) {
            const char *msg;

            /* Falling back to DISCONNECTED mid-sequence is a failure. */
            if (state == PSP_NET_APCTL_STATE_DISCONNECTED &&
                last_state > PSP_NET_APCTL_STATE_DISCONNECTED) {
                printf("pspsky_net: apctl fell back to DISCONNECTED\n");
                break;
            }

            msg = state_message(state);
            if (cb != NULL && msg != NULL && msg != last_msg) {
                cb(msg, user);
                last_msg = msg;
            }
            last_state = state;
        }

        if (state == PSP_NET_APCTL_STATE_GOT_IP) {
            g_connected = 1;
            return PSPSKY_OK;
        }

        if (elapsed_ms >= timeout_ms) {
            printf("pspsky_net: connect timeout after %d sec (state=%d)\n",
                   timeout_sec, state);
            break;
        }

        sceKernelDelayThread(POLL_INTERVAL_US);
        elapsed_ms += POLL_INTERVAL_MS;
    }

    sceNetApctlDisconnect();
    g_connected = 0;
    return PSPSKY_E_NET_CONNECT;
}

/* ---------------------------------------------------------------- state */

int pspsky_net_is_connected(void)
{
    int state = PSP_NET_APCTL_STATE_DISCONNECTED;

    if (!g_net_inited) {
        return 0;
    }
    if (sceNetApctlGetState(&state) != 0) {
        return 0;
    }
    return (state == PSP_NET_APCTL_STATE_GOT_IP) ? 1 : 0;
}

void pspsky_net_disconnect(void)
{
    int state = PSP_NET_APCTL_STATE_DISCONNECTED;

    if (!g_net_inited) {
        return;
    }
    if (sceNetApctlGetState(&state) != 0) {
        return;
    }
    if (state == PSP_NET_APCTL_STATE_DISCONNECTED) {
        g_connected = 0;
        return; /* nothing to do when not connected */
    }

    sceNetApctlDisconnect();
    g_connected = 0;
}

/* ------------------------------------------------------------- teardown */

void pspsky_net_term(void)
{
    if (!g_net_inited) {
        return;
    }

    pspsky_net_disconnect();

    /* Reverse of init; individual failures are ignored. */
    sceNetApctlTerm();
    sceNetResolverTerm();
    sceNetInetTerm();
    sceNetTerm();
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);

    g_net_inited = 0;
    g_connected = 0;
}
