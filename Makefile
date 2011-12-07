#
# Makefile for a Video Disk Recorder plugin
#
# $Id$

# The official name of this plugin.
# This name will be used in the '-P...' option of VDR to load the plugin.
# By default the main source file also carries this name.
# IMPORTANT: the presence of this macro is important for the Make.config
# file. So it must be defined, even if it is not used here!
#
PLUGIN = softhddevice

### The version number of this plugin (taken from the main source file):

VERSION = $(shell grep 'static const char \* const VERSION *=' $(PLUGIN).cpp | awk '{ print $$8 }' | sed -e 's/[";]//g')

### The C++ compiler and options:

CXX      ?= g++
CXXFLAGS ?= -g -O3 -W -Wall -Wextra -Woverloaded-virtual -fPIC
override CXXFLAGS += $(DEFINES) $(INCLUDES)
CFLAGS   ?=	-g -O3 -W -Wall -Wextra -Winit-self \
		-Wdeclaration-after-statement -fPIC
#CFLAGS	+=	-Werror
override CFLAGS   +=	$(DEFINES) $(INCLUDES) \
		$(shell pkg-config --cflags alsa libavcodec libavformat)
override LDFLAGS  += -lrt \
	$(shell pkg-config --libs alsa libavcodec libavformat) \
	`pkg-config --libs x11 x11-xcb xcb xcb-xv xcb-shm xcb-dpms xcb-atom\
		xcb-screensaver xcb-randr xcb-glx xcb-icccm xcb-keysyms`\
	`pkg-config --libs gl glu` \
	`pkg-config --libs vdpau` \
	`pkg-config --libs libva-x11 libva-glx libva`

### The directory environment:

VDRDIR = ../../..
LIBDIR = ../../lib
TMPDIR = /tmp

### Make sure that necessary options are included:

include $(VDRDIR)/Make.global

### Allow user defined options to overwrite defaults:

-include $(VDRDIR)/Make.config

### The version number of VDR's plugin API (taken from VDR's "config.h"):

APIVERSION = $(shell sed -ne '/define APIVERSION/s/^.*"\(.*\)".*$$/\1/p' $(VDRDIR)/config.h)

### The name of the distribution archive:

ARCHIVE = $(PLUGIN)-$(VERSION)
PACKAGE = vdr-$(ARCHIVE)

### Includes and Defines (add further entries here):

INCLUDES += -I$(VDRDIR)/include

DEFINES += -D_GNU_SOURCE -DPLUGIN_NAME_I18N='"$(PLUGIN)"'

### The object files (add further files here):

OBJS = $(PLUGIN).o softhddev.o video.o audio.o codec.o ringbuffer.o
SRCS = $(wildcard $(OBJS:.o=.c)) $(PLUGIN).cpp

### The main target:

all: libvdr-$(PLUGIN).so i18n

### Implicit rules:
#
#%.o: %.cpp
#	$(CXX) $(CXXFLAGS) -c $(DEFINES) $(INCLUDES) $<

### Dependencies:

MAKEDEP = $(CC) -MM -MG
DEPFILE = .dependencies
$(DEPFILE): Makefile
	@$(MAKEDEP) $(DEFINES) $(INCLUDES) $(SRCS) >$@

-include $(DEPFILE)

### Internationalization (I18N):

PODIR     = po
LOCALEDIR = $(VDRDIR)/locale
I18Npo    = $(wildcard $(PODIR)/*.po)
I18Nmsgs  = $(addprefix $(LOCALEDIR)/, $(addsuffix /LC_MESSAGES/vdr-$(PLUGIN).mo, $(notdir $(foreach file, $(I18Npo), $(basename $(file))))))
I18Npot   = $(PODIR)/$(PLUGIN).pot

%.mo: %.po
	msgfmt -c -o $@ $<

$(I18Npot): $(wildcard *.cpp)  $(wildcard *.c)
	xgettext -C -cTRANSLATORS --no-wrap --no-location -k -ktr -ktrNOOP \
	-k_ -k_N --package-name=VDR --package-version=$(VDRVERSION) \
	--msgid-bugs-address='<see README>' -o $@ $^

%.po: $(I18Npot)
	msgmerge -U --no-wrap --no-location --backup=none -q $@ $<
	@touch $@

$(I18Nmsgs): $(LOCALEDIR)/%/LC_MESSAGES/vdr-$(PLUGIN).mo: $(PODIR)/%.mo
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: i18n
i18n: $(I18Nmsgs) $(I18Npot)

### Targets:

libvdr-$(PLUGIN).so: $(OBJS) Makefile
	$(CXX) $(CXXFLAGS) -shared $(OBJS) -o $@ $(LDFLAGS)
	@cp --remove-destination $@ $(LIBDIR)/$@.$(APIVERSION)

dist: $(I18Npo) clean
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@mkdir $(TMPDIR)/$(ARCHIVE)
	@cp -a * $(TMPDIR)/$(ARCHIVE)
	@tar czf $(PACKAGE).tgz -C $(TMPDIR) $(ARCHIVE)
	@-rm -rf $(TMPDIR)/$(ARCHIVE)
	@echo Distribution package created as $(PACKAGE).tgz

clean:
	@-rm -f $(OBJS) $(DEPFILE) *.so *.tgz core* *~ $(PODIR)/*.mo $(PODIR)/*.pot

install:	libvdr-$(PLUGIN).so
	cp --remove-destination libvdr-$(PLUGIN).so \
		/usr/lib/vdr/plugins/libvdr-$(PLUGIN).so.$(APIVERSION)

HDRS=	$(wildcard *.h)

indent:
	for i in $(wildcard $(OBJS:.o=.c)) $(HDRS); do \
		indent $$i; unexpand -a $$i > $$i.up; mv $$i.up $$i; \
	done

video_test: video.c
	$(CC) -DVIDEO_TEST -DVERSION='"$(VERSION)"' $(CFLAGS) $(LDFLAGS) $(LIBS) \
	-O0 -g -o $@ $<
