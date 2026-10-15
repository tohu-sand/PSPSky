TARGET = PSPSKY

WEBP_OBJS = $(patsubst %.c,%.o,\
            $(wildcard vendor/libwebp/src/dec/*.c) \
            $(wildcard vendor/libwebp/src/dsp/*.c) \
            $(wildcard vendor/libwebp/src/utils/*.c))

OBJS = vendor/cjson/cJSON.o \
       vendor/picojpeg/picojpeg.o \
       $(WEBP_OBJS) \
       src/core/error.o src/core/strings.o src/core/utf8.o src/core/timeutil.o src/core/atproto.o \
       src/core/jpeg.o src/core/jpeg_thumb.o src/core/png.o src/core/png_thumb.o \
       src/core/webp_thumb.o \
       src/psp/net.o src/psp/transport_curl.o src/psp/config.o \
       src/psp/osk.o src/psp/ui.o src/psp/main.o

INCDIR   =
CFLAGS   = -O2 -G0 -Wall -std=gnu99 -Iinclude -Ivendor -Ivendor/libwebp -Ivendor/libwebp/src
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS  = $(CFLAGS)

LIBDIR  =
# Use RTC-based UTC for TLS certificate checks; PSP newlib time() is not UTC.
LDFLAGS = -Wl,--wrap=time
# Keep cglue before PSP stubs to preserve a contiguous PRX import table.
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
