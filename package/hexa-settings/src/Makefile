# hexa-settings: the hexaOS Settings app
PKG_CONFIG ?= pkg-config
CC ?= cc
CFLAGS ?= -O2

# Kept apart from CFLAGS/LDFLAGS, which Buildroot passes on the command line
HS_CFLAGS = $(CFLAGS) -Wall -Wno-unused-parameter $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
HS_LIBS = $(shell $(PKG_CONFIG) --libs gtk+-3.0)

hexa-settings: hexa-settings.c
	$(CC) $(HS_CFLAGS) $(LDFLAGS) -o $@ $< $(HS_LIBS)

clean:
	rm -f hexa-settings

.PHONY: clean
