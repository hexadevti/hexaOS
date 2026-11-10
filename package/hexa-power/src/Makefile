# hexa-power-gui: the hexaOS power window
PKG_CONFIG ?= pkg-config
CC ?= cc
CFLAGS ?= -O2

# Kept apart from CFLAGS/LDFLAGS, which Buildroot passes on the command line
HP_CFLAGS = $(CFLAGS) -Wall -Wno-unused-parameter $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
HP_LIBS = $(shell $(PKG_CONFIG) --libs gtk+-3.0)

hexa-power-gui: hexa-power-gui.c
	$(CC) $(HP_CFLAGS) $(LDFLAGS) -o $@ $< $(HP_LIBS)

clean:
	rm -f hexa-power-gui

.PHONY: clean
