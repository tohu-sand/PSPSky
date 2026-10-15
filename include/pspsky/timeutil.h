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

/*
 * Relative time uses seconds, minutes, hours, days, then 30-day months.
 * From 360 days, use the UTC date: "YYYY/M/D" (ja) or "M/D/YYYY" (en).
 * A future time or age under 5 seconds gives "now"; post_unix < 0 gives "----".
 * bufsz must be at least 16.
 */
int pspsky_format_relative_ja(long long post_unix, long long now_unix,
                              char *buf, size_t bufsz);

int pspsky_format_relative_en(long long post_unix, long long now_unix,
                              char *buf, size_t bufsz);

/* Use the current UI language. */
int pspsky_format_relative(long long post_unix, long long now_unix,
                           char *buf, size_t bufsz);

#endif /* PSPSKY_TIMEUTIL_H */
