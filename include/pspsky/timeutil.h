/* UTC time conversion and relative-time formatting. Portable C99, libc only. */
#ifndef PSPSKY_TIMEUTIL_H
#define PSPSKY_TIMEUTIL_H

#include <stddef.h>

/* Proleptic Gregorian (UTC) -> epoch seconds; no range checking */
long long pspsky_civil_to_unix(int year, int month, int day,
                               int hour, int min, int sec);

/* "YYYY-MM-DDTHH:MM:SS[.fff](Z|+-HH:MM)" -> epoch seconds; -1 on failure */
long long pspsky_iso8601_parse(const char *s);

/* epoch seconds -> "YYYY-MM-DDTHH:MM:SS.000Z"; bufsz >= 25 */
int pspsky_iso8601_format(long long unix_utc, char *buf, size_t bufsz);

/* Buckets on diff = now - post: <60s, <60m, <24h, <7d, else "YYYY/MM/DD" (UTC).
   post_unix < 0 gives "----". Wording comes from PSPSKY_STR_TIME_*; bufsz >= 16. */
int pspsky_format_relative_ja(long long post_unix, long long now_unix,
                              char *buf, size_t bufsz);

int pspsky_format_relative_en(long long post_unix, long long now_unix,
                              char *buf, size_t bufsz);

/* Dispatches to _ja / _en by the current UI language; the UI calls this one */
int pspsky_format_relative(long long post_unix, long long now_unix,
                           char *buf, size_t bufsz);

#endif /* PSPSKY_TIMEUTIL_H */
