TARGET = PSPSKY
OBJS = vendor/cjson/cJSON.o \
       vendor/picojpeg/picojpeg.o \
       src/core/error.o src/core/strings.o src/core/utf8.o src/core/timeutil.o src/core/atproto.o \
       src/core/jpeg.o src/core/jpeg_thumb.o src/core/png.o src/core/png_thumb.o \
       src/psp/net.o src/psp/transport_curl.o src/psp/config.o \
       src/psp/osk.o src/psp/ui.o src/psp/main.o

INCDIR   =
CFLAGS   = -O2 -G0 -Wall -std=gnu99 -Iinclude -Ivendor
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

LIBDIR  =
# mbedTLS checks certificate dates with time(), but the PSP newlib time() does
# not return true UTC, so valid certificates are rejected (curl:60). --wrap
# redirects it to __wrap_time() in src/psp/main.c, which reads the RTC.
LDFLAGS = -Wl,--wrap=time
# -lcglue must precede the PSP stub libraries. Linked last, its sce* stubs are
# pulled in over two passes and land discontiguous in .sceStub.text, corrupting
# the import table and producing a PRX that will not load.
LIBS = -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz \
       -lintrafont \
       -lcglue \
       -lpspgu -lpspdisplay -lpspge -lpspctrl -lpsprtc -lpsppower -lpsputility \
       -lpspnet -lpspnet_apctl -lpspnet_inet -lpspnet_resolver -lm

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = PSPSky
# XMB icon (144x80) and selection background (480x272).
PSP_EBOOT_ICON  = ICON0.PNG
PSP_EBOOT_PIC1  = PIC1.PNG
PSP_FW_VERSION  = 660
BUILD_PRX       = 1

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
