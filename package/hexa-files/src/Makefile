# hexa-files: the hexaOS file manager
PKG_CONFIG ?= pkg-config
CC ?= cc
CFLAGS ?= -O2

# Kept apart from CFLAGS/LDFLAGS, which Buildroot passes on the command line
HF_CFLAGS = $(CFLAGS) -Wall -Wno-unused-parameter $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
HF_LIBS = $(shell $(PKG_CONFIG) --libs gtk+-3.0)

hexa-files: hexa-files.c
	$(CC) $(HF_CFLAGS) $(LDFLAGS) -o $@ $< $(HF_LIBS)

clean:
	rm -f hexa-files

.PHONY: clean
