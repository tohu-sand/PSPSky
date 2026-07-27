/*
 * PSPSky - PSP on-screen keyboard wrapper, UTF-8 <-> UCS-2 at the boundary.
 * The OSK is UCS-2 only: non-BMP text cannot round-trip, and every length here
 * is a character count, not a byte count (up to 3 UTF-8 bytes each).
 */

#include <stdio.h>
#include <string.h>

#include <pspdisplay.h>
#include <pspgu.h>
#include <psputility.h>

#include "pspsky/error.h"
#include "pspsky/osk.h"
#include "pspsky/strings.h"
#include "pspsky/utf8.h"

#define OSK_SCR_W      480
#define OSK_SCR_H      272
#define OSK_LIST_WORDS 4096   /* 16KB: the OSK frame only clears the screen */
#define OSK_BG_COLOR   0xFF201510

/* GU display list used while the OSK is up (separate from ui.c). */
static unsigned int __attribute__((aligned(16))) s_osk_list[OSK_LIST_WORDS];

/* Static so they stay off the stack. */
static unsigned short s_desc[PSPSKY_OSK_MAX_CHARS + 1];
static unsigned short s_intext[PSPSKY_OSK_MAX_CHARS + 1];
static unsigned short s_outtext[PSPSKY_OSK_MAX_CHARS + 1];

int pspsky_osk_input(const char *desc_utf8, const char *initial_utf8,
                     int max_chars, char *out_utf8, size_t out_cap)
{
    SceUtilityOskData   data;
    SceUtilityOskParams params;
    int                 rc;
    int                 done = 0;

    if (out_utf8 == NULL || out_cap == 0) {
        return PSPSKY_E_ARG;
    }
    if (max_chars < 1 || max_chars > PSPSKY_OSK_MAX_CHARS) {
        return PSPSKY_E_ARG;
    }
    out_utf8[0] = '\0';

    memset(s_desc, 0, sizeof(s_desc));
    memset(s_intext, 0, sizeof(s_intext));
    memset(s_outtext, 0, sizeof(s_outtext));

    if (desc_utf8 != NULL) {
        pspsky_utf8_to_ucs2(desc_utf8, s_desc, PSPSKY_OSK_MAX_CHARS + 1);
    }
    if (initial_utf8 != NULL) {
        pspsky_utf8_to_ucs2(initial_utf8, s_intext, max_chars + 1);
    }

    memset(&data, 0, sizeof(data));
    data.language      = (pspsky_lang_get() == PSPSKY_LANG_JA)
                             ? PSP_UTILITY_OSK_LANGUAGE_JAPANESE
                             : PSP_UTILITY_OSK_LANGUAGE_ENGLISH;
    data.lines         = 1;
    data.unk_24        = 1;
    data.inputtype     = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.desc          = s_desc;
    data.intext        = s_intext;
    data.outtextlength = max_chars + 1;   /* outtext elements, NUL included */
    data.outtextlimit  = max_chars;       /* max characters enterable       */
    data.outtext       = s_outtext;

    memset(&params, 0, sizeof(params));
    params.base.size = sizeof(params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE,
                                &params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP,
                                &params.base.buttonSwap);
    params.base.graphicsThread = 17;
    params.base.accessThread   = 19;
    params.base.fontThread     = 18;
    params.base.soundThread    = 16;
    params.datacount           = 1;
    params.data                = &data;

    rc = sceUtilityOskInitStart(&params);
    if (rc < 0) {
        printf("pspsky_osk_input: sceUtilityOskInitStart failed (0x%08X)\n",
               (unsigned int)rc);
        /* Pass the error through, but never collide with the arg-error code. */
        return (rc == PSPSKY_E_ARG) ? PSPSKY_E_IO : rc;
    }

    while (!done) {
        sceGuStart(GU_DIRECT, s_osk_list);
        sceGuClearColor(OSK_BG_COLOR);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

        switch (sceUtilityOskGetStatus()) {
        case PSP_UTILITY_DIALOG_INIT:
            break;

        case PSP_UTILITY_DIALOG_VISIBLE:
            sceUtilityOskUpdate(1);
            break;

        case PSP_UTILITY_DIALOG_QUIT:
            sceUtilityOskShutdownStart();
            break;

        case PSP_UTILITY_DIALOG_FINISHED:
            break;

        case PSP_UTILITY_DIALOG_NONE:
            done = 1;
            break;

        default:
            break;
        }

        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }

    if (data.result == PSP_UTILITY_OSK_RESULT_CANCELLED) {
        return PSPSKY_E_CANCELLED;
    }

    /* CHANGED and UNCHANGED both yield usable text. */
    s_outtext[PSPSKY_OSK_MAX_CHARS] = 0;
    rc = pspsky_ucs2_to_utf8(s_outtext, out_utf8, out_cap);
    if (rc < 0) {
        out_utf8[0] = '\0';
        return rc;
    }
    return PSPSKY_OK;
}
