/* Credential and preference file stored on the memory stick. */
#ifndef PSPSKY_CONFIG_H
#define PSPSKY_CONFIG_H

typedef struct {
    char identifier[256];    /* e.g. "user.bsky.social" */
    char app_password[128];
    char lang[3];            /* "ja"/"en"; empty = auto-detect */
} pspsky_config_t;

/* UTF-8 text, LF or CRLF, any line order, unknown keys ignored:
     identifier=user.bsky.social
     password=xxxx-xxxx-xxxx-xxxx
     lang=ja
   On E_PARSE a successfully read lang is kept, so a config that only overrides
   the language stays useful. */
int pspsky_config_load(const char *path, pspsky_config_t *out);

/* Writes the format above; an empty lang omits the line entirely */
int pspsky_config_save(const char *path, const pspsky_config_t *cfg);

#endif /* PSPSKY_CONFIG_H */
