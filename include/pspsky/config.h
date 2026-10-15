#ifndef PSPSKY_CONFIG_H
#define PSPSKY_CONFIG_H

typedef struct {
    char identifier[256];
    char app_password[128];
    char lang[3];  /* "ja"/"en"; empty = auto-detect */
    int  show_images;  /* 0 = timeline thumbnails disabled */
} pspsky_config_t;

/*
 * UTF-8 key=value lines; LF or CRLF, any order, unknown keys ignored.
 * Keys: identifier, password, lang (ja/en), images (off).
 * Images default to enabled; only "off" disables them.
 * Parsed preferences are retained on E_PARSE, including missing credentials.
 */
int pspsky_config_load(const char *path, pspsky_config_t *out);

/* Omit lang when empty and images when enabled. */
int pspsky_config_save(const char *path, const pspsky_config_t *cfg);

#endif /* PSPSKY_CONFIG_H */
