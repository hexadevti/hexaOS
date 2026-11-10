# hexa-portal: the hexaOS desktop portal (open links/files for Flatpak apps)
PKG_CONFIG ?= pkg-config
CC ?= cc
CFLAGS ?= -O2

# Kept apart from CFLAGS/LDFLAGS, which Buildroot passes on the command line
HP_CFLAGS = $(CFLAGS) -Wall -Wno-unused-parameter $(shell $(PKG_CONFIG) --cflags gio-unix-2.0)
HP_LIBS = $(shell $(PKG_CONFIG) --libs gio-unix-2.0)

hexa-portal: hexa-portal.c
	$(CC) $(HP_CFLAGS) $(LDFLAGS) -o $@ $< $(HP_LIBS)

clean:
	rm -f hexa-portal

.PHONY: clean
