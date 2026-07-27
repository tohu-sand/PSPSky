/* PSP on-screen keyboard text input. PSP-only. */
#ifndef PSPSKY_OSK_H
#define PSPSKY_OSK_H

#include <stddef.h>

#define PSPSKY_OSK_MAX_CHARS 512

/* Input language follows pspsky_lang_get; max_chars counts UCS-2 characters.
   Runs GU frames internally, so it may only be called after pspsky_ui_init(). */
int pspsky_osk_input(const char *desc_utf8, const char *initial_utf8,
                     int max_chars, char *out_utf8, size_t out_cap);

#endif /* PSPSKY_OSK_H */
