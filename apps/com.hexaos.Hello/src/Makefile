PKG_CONFIG ?= pkg-config
CC ?= cc
CFLAGS ?= -O2
PREFIX ?= /usr/local

APP_CFLAGS = $(CFLAGS) -Wall -Wno-unused-parameter $(shell $(PKG_CONFIG) --cflags gtk+-3.0)
APP_LIBS = $(shell $(PKG_CONFIG) --libs gtk+-3.0)

hexa-hello: main.c
	$(CC) $(APP_CFLAGS) $(LDFLAGS) -o $@ $< $(APP_LIBS)

install: hexa-hello
	install -Dm755 hexa-hello $(DESTDIR)$(PREFIX)/bin/hexa-hello

clean:
	rm -f hexa-hello

.PHONY: install clean
