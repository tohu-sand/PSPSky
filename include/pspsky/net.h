/* PSP network stack bring-up and access point connection. PSP-only. */
#ifndef PSPSKY_NET_H
#define PSPSKY_NET_H

/* Connection progress reported to the UI (UTF-8) */
typedef void (*pspsky_net_progress_cb)(const char *msg_utf8, void *user);

/* Unwinds everything already initialized if any stage fails */
int pspsky_net_init(void);

/* Connects APCTL profile conf_index (1-based) and polls until GOT_IP; cb may be NULL */
int pspsky_net_connect(int conf_index, int timeout_sec,
                       pspsky_net_progress_cb cb, void *user);

int  pspsky_net_is_connected(void); /* 1 = GOT_IP */
void pspsky_net_disconnect(void);   /* no-op if not connected */
void pspsky_net_term(void);         /* tears down in reverse order */

#endif /* PSPSKY_NET_H */
