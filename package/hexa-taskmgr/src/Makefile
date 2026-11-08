# hexa-taskmgr: the hexaOS Task Manager
PKG_CONFIG ?= pkg-config
CC ?= cc
CFLAGS ?= -O2

# Kept apart from CFLAGS/LDFLAGS, which Buildroot passes on the command line
TM_CFLAGS = $(CFLAGS) -Wall -Wno-unused-parameter $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
TM_LIBS = $(shell $(PKG_CONFIG) --libs gtk+-3.0)

hexa-taskmgr: hexa-taskmgr.c
	$(CC) $(TM_CFLAGS) $(LDFLAGS) -o $@ $< $(TM_LIBS)

clean:
	rm -f hexa-taskmgr

.PHONY: clean
