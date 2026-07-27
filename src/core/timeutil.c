/* Time conversion utilities. Everything is UTC, computed with Howard Hinnant's
 * days_from_civil / civil_from_days: timegm is not portable and mktime and
 * localtime depend on a local timezone the PSP does not reliably provide.
 */

#include <stdio.h>

#include "pspsky/timeutil.h"
#include "pspsky/error.h"
#include "pspsky/strings.h"

#define SECS_PER_DAY  86400LL

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* C99's / truncates toward zero, so negative epochs need real floor division */
static long long floor_div(long long a, long long b)
{
    long long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static int is_digit_c(int c)
{
    return (c >= '0' && c <= '9');
}

/* Day number (1970-01-01 = 0) -> year/month/day. Inverse of days_from_civil */
static void civil_from_days(long long z, int *out_y, int *out_m, int *out_d)
{
    long long era, doe, yoe, y, doy, mp, d, m;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;                                     /* [0, 146096] */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399]    */
    y   = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              /* [0, 365]    */
    mp  = (5 * doy + 2) / 153;                                  /* [0, 11]     */
    d   = doy - (153 * mp + 2) / 5 + 1;                         /* [1, 31]     */
    m   = mp + (mp < 10 ? 3 : -9);                              /* [1, 12]     */
    if (m <= 2) {
        y++;
    }

    *out_y = (int)y;
    *out_m = (int)m;
    *out_d = (int)d;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

long long pspsky_civil_to_unix(int year, int month, int day,
                               int hour, int min, int sec)
{
    long long y = (long long)year;
    long long m = (long long)month;
    long long d = (long long)day;
    long long era, yoe, doy, doe, days;

    if (m <= 2) {
        y -= 1;
    }
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                        /* [0, 399]    */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;       /* [0, 365]    */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                /* [0, 146096] */
    days = era * 146097 + doe - 719468;

    return days * SECS_PER_DAY
         + (long long)hour * 3600LL
         + (long long)min * 60LL
         + (long long)sec;
}

long long pspsky_iso8601_parse(const char *s)
{
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
    int consumed = 0;
    const char *p;
    long long t;

    if (s == NULL) {
        return -1;
    }
    /* sscanf would accept leading whitespace and a sign, so check strictly first */
    if (!is_digit_c((unsigned char)s[0])) {
        return -1;
    }
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n",
               &y, &mo, &d, &hh, &mi, &ss, &consumed) != 6) {
        return -1;
    }
    if (consumed <= 0) {
        return -1;
    }
    /* Range validation */
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31) {
        return -1;
    }
    if (hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 59) {
        return -1;
    }

    p = s + consumed;

    /* Skip fractional seconds (truncated) */
    if (*p == '.') {
        p++;
        if (!is_digit_c((unsigned char)*p)) {
            return -1;
        }
        while (is_digit_c((unsigned char)*p)) {
            p++;
        }
    }

    t = pspsky_civil_to_unix(y, mo, d, hh, mi, ss);

    if (*p == 'Z' || *p == 'z') {
        if (p[1] != '\0') {
            return -1;
        }
        return t;
    }

    if (*p == '+' || *p == '-') {
        char sign = *p;
        const char *q = p + 1;
        int oh = 0, om = 0;
        int n2 = 0;
        long long off;

        if (!is_digit_c((unsigned char)q[0])) {
            return -1;
        }
        if (sscanf(q, "%2d:%2d%n", &oh, &om, &n2) != 2) {
            return -1;
        }
        if (n2 <= 0 || q[n2] != '\0') {
            return -1;
        }
        if (oh < 0 || oh > 23 || om < 0 || om > 59) {
            return -1;
        }
        off = (long long)oh * 3600LL + (long long)om * 60LL;
        /* Convert the written local time to UTC */
        t = (sign == '+') ? (t - off) : (t + off);
        return t;
    }

    /* Anything that is neither Z nor an offset, including a bare NUL, is invalid */
    return -1;
}

int pspsky_iso8601_format(long long unix_utc, char *buf, size_t bufsz)
{
    long long days, secs;
    int y = 0, mo = 0, d = 0;
    int hh, mi, ss;

    if (buf == NULL || bufsz < 25) {
        return PSPSKY_E_ARG;
    }

    days = floor_div(unix_utc, SECS_PER_DAY);
    secs = unix_utc - days * SECS_PER_DAY;   /* [0, 86399] */
    civil_from_days(days, &y, &mo, &d);

    hh = (int)(secs / 3600);
    mi = (int)((secs / 60) % 60);
    ss = (int)(secs % 60);

    snprintf(buf, bufsz, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
             y, mo, d, hh, mi, ss);
    return PSPSKY_OK;
}

/* Bucket logic is language independent; only the wording is looked up */
static int format_relative_lang(pspsky_lang_t lang,
                                long long post_unix, long long now_unix,
                                char *buf, size_t bufsz)
{
    long long diff;

    if (buf == NULL || bufsz < 16) {
        return PSPSKY_E_ARG;
    }

    if (post_unix < 0) {
        snprintf(buf, bufsz, "----");
        return PSPSKY_OK;
    }

    diff = now_unix - post_unix;

    if (diff < 60LL) {
        /* Future-dated posts (negative diff) also count as "just now" */
        snprintf(buf, bufsz, "%s", pspsky_tr_lang(lang, PSPSKY_STR_TIME_JUST_NOW));
    } else if (diff < 3600LL) {
        snprintf(buf, bufsz, pspsky_tr_lang(lang, PSPSKY_STR_TIME_MIN_AGO),
                 (int)(diff / 60LL));
    } else if (diff < 86400LL) {
        snprintf(buf, bufsz, pspsky_tr_lang(lang, PSPSKY_STR_TIME_HOUR_AGO),
                 (int)(diff / 3600LL));
    } else if (diff < 7LL * 86400LL) {
        snprintf(buf, bufsz, pspsky_tr_lang(lang, PSPSKY_STR_TIME_DAY_AGO),
                 (int)(diff / 86400LL));
    } else {
        int y = 0, mo = 0, d = 0;
        civil_from_days(floor_div(post_unix, SECS_PER_DAY), &y, &mo, &d);
        snprintf(buf, bufsz, "%04d/%02d/%02d", y, mo, d);
    }

    return PSPSKY_OK;
}

int pspsky_format_relative_ja(long long post_unix, long long now_unix,
                              char *buf, size_t bufsz)
{
    return format_relative_lang(PSPSKY_LANG_JA, post_unix, now_unix,
                                buf, bufsz);
}

int pspsky_format_relative_en(long long post_unix, long long now_unix,
                              char *buf, size_t bufsz)
{
    return format_relative_lang(PSPSKY_LANG_EN, post_unix, now_unix,
                                buf, bufsz);
}

int pspsky_format_relative(long long post_unix, long long now_unix,
                           char *buf, size_t bufsz)
{
    return format_relative_lang(pspsky_lang_get(), post_unix, now_unix,
                                buf, bufsz);
}
