/* UI string table and language selection; tables live in src/core/strings.c. */
#ifndef PSPSKY_STRINGS_H
#define PSPSKY_STRINGS_H

/* One global for the whole process. PSP-specific detection (sceUtility) happens
   in main; the core only reads this. Not thread-safe. */
typedef enum {
    PSPSKY_LANG_JA = 0,   /* default */
    PSPSKY_LANG_EN = 1
} pspsky_lang_t;

/* Anything other than PSPSKY_LANG_EN is stored as PSPSKY_LANG_JA */
void pspsky_lang_set(pspsky_lang_t lang);

pspsky_lang_t pspsky_lang_get(void);

/* Values and ordering must match the tables in src/core/strings.c.
   TIME_MIN/HOUR/DAY_AGO and TRC_CONNECT_FAIL/TRC_COMM_FAIL are printf formats
   with one %d; TRC_TLS_HANDSHAKE has one %s; the rest have no specifiers. */
typedef enum {
    /* --- A. error.c: pspsky_strerror --- */
    PSPSKY_STR_ERR_OK = 0,
    PSPSKY_STR_ERR_ARG,
    PSPSKY_STR_ERR_NOMEM,
    PSPSKY_STR_ERR_TRANSPORT,
    PSPSKY_STR_ERR_TLS,
    PSPSKY_STR_ERR_HTTP,
    PSPSKY_STR_ERR_AUTH,
    PSPSKY_STR_ERR_AUTH_EXPIRED,
    PSPSKY_STR_ERR_PARSE,
    PSPSKY_STR_ERR_IO,
    PSPSKY_STR_ERR_NET_INIT,
    PSPSKY_STR_ERR_NET_CONNECT,
    PSPSKY_STR_ERR_CANCELLED,
    PSPSKY_STR_ERR_RATELIMIT,
    PSPSKY_STR_ERR_UNKNOWN,
    /* --- B. timeutil.c: relative time --- */
    PSPSKY_STR_TIME_JUST_NOW,
    PSPSKY_STR_TIME_MIN_AGO,
    PSPSKY_STR_TIME_HOUR_AGO,
    PSPSKY_STR_TIME_DAY_AGO,
    /* --- C. net.c: connection progress --- */
    PSPSKY_STR_NET_CONNECTING_AP,
    PSPSKY_STR_NET_GETTING_IP,
    PSPSKY_STR_NET_CONNECTED,
    /* --- D. ui.c --- */
    PSPSKY_STR_UI_FOOTER_TIMELINE,
    PSPSKY_STR_UI_FOOTER_PICK,
    PSPSKY_STR_UI_FOOTER_PREV,
    PSPSKY_STR_UI_HINT_COMPOSE,
    PSPSKY_STR_UI_HINT_OK,
    PSPSKY_STR_UI_HINT_YES_NO,
    PSPSKY_STR_UI_EMBED_MARK,
    PSPSKY_STR_UI_PREV_TITLE,
    PSPSKY_STR_UI_PREV_NO_IMAGE,
    PSPSKY_STR_UI_PREV_NO_IMAGE_SUB,
    PSPSKY_STR_UI_PREV_DIM_UNKNOWN,
    PSPSKY_STR_UI_PICK_EMPTY,
    PSPSKY_STR_UI_TL_EMPTY,
    PSPSKY_STR_UI_MSG_PATH_TOO_LONG,
    PSPSKY_STR_UI_MSG_DIR_OPEN_FAIL,
    /* --- E. main.c --- */
    PSPSKY_STR_CACERT_NOT_FOUND,
    PSPSKY_STR_CACERT_UNREADABLE,
    PSPSKY_STR_CA_DIAG_PREFIX,
    PSPSKY_STR_ST_NET_INIT,
    PSPSKY_STR_ST_TRANSPORT_INIT,
    PSPSKY_STR_ST_LOGGING_IN,
    PSPSKY_STR_ST_TIMELINE_LOADING,
    PSPSKY_STR_ST_LOADING_MORE,
    PSPSKY_STR_ST_POSTING,
    PSPSKY_STR_ST_UPLOADING,
    PSPSKY_STR_ST_PREVIEW_PREP,
    PSPSKY_STR_OSK_HANDLE,
    PSPSKY_STR_OSK_PASSWORD,
    PSPSKY_STR_OSK_COMPOSE,
    PSPSKY_STR_OSK_ALT,
    PSPSKY_STR_MSG_HANDLE_EMPTY,
    PSPSKY_STR_MSG_HANDLE_TOO_LONG,
    PSPSKY_STR_MSG_PASSWORD_EMPTY,
    PSPSKY_STR_MSG_PASSWORD_TOO_LONG,
    PSPSKY_STR_MSG_CFG_SAVE_FAIL,
    PSPSKY_STR_CONFIRM_RETRY,
    PSPSKY_STR_MSG_TIME_FAIL,
    PSPSKY_STR_MSG_POSTED,
    PSPSKY_STR_MSG_IMG_UNSUPPORTED,
    PSPSKY_STR_MSG_IMG_TOO_LARGE,
    PSPSKY_STR_MSG_IMG_READ_FAIL,
    PSPSKY_STR_MSG_TEXT_TOO_LONG,
    PSPSKY_STR_CONFIRM_QUIT,
    PSPSKY_STR_MSG_SESSION_EXPIRED_RELOGIN,
    PSPSKY_STR_CONFIRM_ABORT_QUIT,
    /* --- F. transport_curl.c: errmsg --- */
    PSPSKY_STR_TRC_CACERT_READ_PRE,
    PSPSKY_STR_TRC_PATH_UNSET,
    PSPSKY_STR_TRC_CERT_VERIFY,
    PSPSKY_STR_TRC_TLS_HANDSHAKE,
    PSPSKY_STR_TRC_CONNECT_FAIL,
    PSPSKY_STR_TRC_COMM_FAIL,
    PSPSKY_STR_TRC_RESP_TOO_LARGE,

    PSPSKY_STR_COUNT
} pspsky_str_id_t;

/* Static UTF-8 in the current language. The same (language, id) always yields
   the same pointer, so callers may compare pointers to suppress duplicates. */
const char *pspsky_tr(pspsky_str_id_t id);

/* Language-pinned lookup, for tests and timeutil's _ja/_en entry points */
const char *pspsky_tr_lang(pspsky_lang_t lang, pspsky_str_id_t id);

#endif /* PSPSKY_STRINGS_H */
