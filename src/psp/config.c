/* Credentials are stored in plaintext; use a Bluesky app password. */

#include <stdio.h>
#include <string.h>

#include "pspsky/config.h"
#include "pspsky/error.h"

#define CFG_LINE_MAX 1024

#define KEY_IDENTIFIER "identifier"
#define KEY_PASSWORD   "password"
#define KEY_LANG       "lang"
#define KEY_IMAGES     "images"

/* Load failures must leave a fully initialized config. */
static void set_defaults(pspsky_config_t *out)
{
    memset(out, 0, sizeof *out);
    out->show_images = 1;
}

static void strip_eol(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

/* Returns 0 rather than truncating when value does not fit. */
static int copy_value(char *dst, size_t cap, const char *value)
{
    size_t n = strlen(value);

    if (n + 1 > cap) {
        return 0;
    }
    memcpy(dst, value, n + 1);
    return 1;
}

int pspsky_config_load(const char *path, pspsky_config_t *out)
{
    FILE *fp;
    char  line[CFG_LINE_MAX];
    int   first_line = 1;
    int   bad        = 0;

    if (path == NULL || out == NULL) {
        return PSPSKY_E_ARG;
    }
    set_defaults(out);

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return PSPSKY_E_IO;
    }

    while (fgets(line, (int)sizeof line, fp) != NULL) {
        char *p = line;
        char *eq;

        /* Discard the overlong remainder so it is not parsed as another line. */
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            int ch;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
                /* discard */
            }
            bad = 1;
        }

        if (first_line &&
            (unsigned char)p[0] == 0xEF &&
            (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) {
            p += 3;
        }
        first_line = 0;

        strip_eol(p);

        eq = strchr(p, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';

        if (strcmp(p, KEY_IDENTIFIER) == 0) {
            if (!copy_value(out->identifier, sizeof out->identifier, eq + 1)) {
                bad = 1;
            }
        } else if (strcmp(p, KEY_PASSWORD) == 0) {
            if (!copy_value(out->app_password, sizeof out->app_password,
                            eq + 1)) {
                bad = 1;
            }
        } else if (strcmp(p, KEY_LANG) == 0) {
            if (strcmp(eq + 1, "ja") == 0 || strcmp(eq + 1, "en") == 0) {
                memcpy(out->lang, eq + 1, 3);
            }
        } else if (strcmp(p, KEY_IMAGES) == 0) {
            if (strcmp(eq + 1, "off") == 0) {
                out->show_images = 0;
            } else if (strcmp(eq + 1, "on") == 0) {
                out->show_images = 1;
            }
        }
    }

    if (ferror(fp)) {
        fclose(fp);
        set_defaults(out);
        return PSPSKY_E_IO;
    }
    fclose(fp);

    if (bad || out->identifier[0] == '\0' || out->app_password[0] == '\0') {
        /* Keep lang and images even when the credentials are unusable. */
        memset(out->identifier, 0, sizeof out->identifier);
        memset(out->app_password, 0, sizeof out->app_password);
        return PSPSKY_E_PARSE;
    }
    return PSPSKY_OK;
}

int pspsky_config_save(const char *path, const pspsky_config_t *cfg)
{
    FILE *fp;
    int   n;

    if (path == NULL || cfg == NULL) {
        return PSPSKY_E_ARG;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return PSPSKY_E_IO;
    }

    /* The precision keeps fprintf in bounds if the field is not terminated. */
    n = fprintf(fp, KEY_IDENTIFIER "=%.*s\n" KEY_PASSWORD "=%.*s\n",
                (int)(sizeof cfg->identifier - 1), cfg->identifier,
                (int)(sizeof cfg->app_password - 1), cfg->app_password);
    if (n < 0) {
        fclose(fp);
        return PSPSKY_E_IO;
    }

    if (cfg->lang[0] != '\0') {
        n = fprintf(fp, KEY_LANG "=%.*s\n",
                    (int)(sizeof cfg->lang - 1), cfg->lang);
        if (n < 0) {
            fclose(fp);
            return PSPSKY_E_IO;
        }
    }

    /* On is the default, so only the disabled state needs a line. */
    if (cfg->show_images == 0) {
        n = fprintf(fp, KEY_IMAGES "=off\n");
        if (n < 0) {
            fclose(fp);
            return PSPSKY_E_IO;
        }
    }

    if (fclose(fp) != 0) {
        return PSPSKY_E_IO;
    }
    return PSPSKY_OK;
}
