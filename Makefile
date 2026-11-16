# sd_bestspeech: BeSTspeech / Keynote Gold for Speech Dispatcher, on openbst.
#
# OPENBST is the openbst source tree. Its library is built here rather than
# by its own Makefile, with patches/ applied and with only the five French
# and English sets of tables linked.

OPENBST ?= ../openbst
BUILD   ?= build
PREFIX  ?= /usr/local
DESTDIR ?=
SPEECHD_MODULEDIR ?= $(PREFIX)/libexec/speech-dispatcher-modules
SPEECHD_CONFDIR   ?= $(PREFIX)/etc/speech-dispatcher/modules

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -Wall -Wextra
BUILDS  := 1995 1998ENG 1998FRN 2006ENG 2006FRE
empty   :=
space   := $(empty) $(empty)
BUILDRE := $(subst $(space),|,$(BUILDS))

BSTMODS := text dict lts cursors wordstream stress assemble tokenise translit \
           except numbers phrules accents pairs contour diphone frames interp \
           synth speak

SRCDIR  := $(BUILD)/openbst
BSTINC  := -I$(SRCDIR)/include
BSTOBJ  := $(addprefix $(BUILD)/bst_,$(addsuffix .o,$(BSTMODS))) \
           $(addprefix $(BUILD)/data_,$(addsuffix .o,$(BUILDS))) \
           $(BUILD)/lifted-fren.o

SPEECHD_CFLAGS  ?= $(shell pkg-config --cflags speech-dispatcher 2>/dev/null)
SPEECHD_LIBDIRS := $(shell pkg-config --libs-only-L speech-dispatcher 2>/dev/null \
                     | sed 's|-L\([^ ]*\)|-L\1 -Wl,-rpath,\1|g')
SPEECHD_LIBS    ?= $(SPEECHD_LIBDIRS) -lspeechd_module
SONIC_LIBS      ?= -l:libsonic.a

all: $(BUILD)/sd_bestspeech $(BUILD)/bstspeak

# A copy of the openbst tree with the patches applied.
$(SRCDIR)/.patched: $(wildcard patches/*.patch)
	@rm -rf $(SRCDIR) && mkdir -p $(SRCDIR)
	@cp -r $(OPENBST)/src $(OPENBST)/include $(OPENBST)/tools $(OPENBST)/tests $(SRCDIR)/
	@chmod -R u+w $(SRCDIR)
	@for p in $(sort $(wildcard patches/*.patch)); do \
	    patch -s -d $(SRCDIR) -p1 < $$p || exit 1; done
	@touch $@

$(BUILD)/bst_%.o: $(SRCDIR)/.patched
	$(CC) $(CFLAGS) -std=gnu11 -fPIC $(BSTINC) -c $(SRCDIR)/src/$*.c -o $@

$(BUILD)/data_%.o: $(SRCDIR)/.patched
	$(CC) $(CFLAGS) -std=gnu11 -fPIC $(BSTINC) -c $(SRCDIR)/src/data/$*.c -o $@

$(BUILD)/lifted-fren.o: src/lifted-fren.c $(SRCDIR)/.patched
	$(CC) $(CFLAGS) -std=gnu11 -fPIC $(BSTINC) -c $< -o $@

$(BUILD)/libbst-fren.a: $(BSTOBJ)
	$(AR) rcs $@ $^

$(BUILD)/sd_bestspeech: speechd/bestspeech.c speechd/spd_audio.h $(BUILD)/libbst-fren.a
	$(CC) $(CFLAGS) $(WARN) -std=gnu11 -Ispeechd $(BSTINC) $(SPEECHD_CFLAGS) \
	    speechd/bestspeech.c $(BUILD)/libbst-fren.a $(SPEECHD_LIBS) $(SONIC_LIBS) \
	    -lm -o $@

# openbst's own command and self-test, on the same cut-down library: the
# reference the module's audio is compared against, and the check that the
# patched library still says what the original binaries said.
$(BUILD)/bstspeak: $(BUILD)/libbst-fren.a
	$(CC) $(CFLAGS) -std=gnu11 $(BSTINC) $(SRCDIR)/tools/synth/bstspeak.c \
	    $(BUILD)/libbst-fren.a -lm -o $@

$(BUILD)/selftest: $(BUILD)/libbst-fren.a
	$(CC) $(CFLAGS) -std=gnu11 $(BSTINC) $(SRCDIR)/tools/synth/selftest.c \
	    $(BUILD)/libbst-fren.a -lm -o $@

$(BUILD)/golden-fren.txt: $(SRCDIR)/.patched
	awk -F'\t' '$$1 ~ /^($(BUILDRE))$$/' \
	    $(SRCDIR)/tests/golden.txt > $@

selftest: $(BUILD)/selftest $(BUILD)/golden-fren.txt
	$(BUILD)/selftest $(BUILD)/golden-fren.txt

test: selftest $(BUILD)/sd_bestspeech $(BUILD)/bstspeak
	python3 test/speechd.py $(BUILD)/sd_bestspeech speechd/bestspeech.conf $(BUILD)/bstspeak

install: $(BUILD)/sd_bestspeech
	install -d $(DESTDIR)$(SPEECHD_MODULEDIR) $(DESTDIR)$(SPEECHD_CONFDIR)
	install -m 755 $(BUILD)/sd_bestspeech $(DESTDIR)$(SPEECHD_MODULEDIR)/
	install -m 644 speechd/bestspeech.conf $(DESTDIR)$(SPEECHD_CONFDIR)/

clean:
	rm -rf $(BUILD)

.PHONY: all selftest test install clean
