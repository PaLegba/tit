include config.mk

PREFIX ?= /usr/local
CC ?= cc
OPT ?= -O2 -g
CFLAGS = $(OPT) -std=gnu11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
         -D_NETBSD_SOURCE -D_FILE_OFFSET_BITS=64 -Ivendor -Wall -Wextra \
         -Wno-unused-parameter -Wno-pointer-sign
LDFLAGS ?=
LDLIBS += -lm -pthread

SRC = src/main.c src/app.c src/ui.c src/playlist.c src/tags.c \
      src/player.c src/themes.c src/util.c
HDR = src/app.h src/player.h src/playlist.h src/tags.h src/themes.h \
      src/ui.h src/util.h

all: tit

tit: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

tests/test: tests/test.c src/tags.c src/playlist.c src/themes.c src/util.c \
            src/tags.h src/playlist.h src/themes.h src/util.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test.c src/tags.c src/playlist.c \
		src/themes.c src/util.c -lm

test: tests/test
	./tests/test

install: tit
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 tit $(DESTDIR)$(PREFIX)/bin/tit

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/tit

clean:
	rm -f tit tests/test src/*.o

.PHONY: all test install uninstall clean
