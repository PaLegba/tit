include config.mk

CC ?= cc
CXX ?= c++
OPT ?= -O2 -g
CFLAGS = $(OPT) -std=gnu11 -D_DEFAULT_SOURCE \
         -D_NETBSD_SOURCE -D_FILE_OFFSET_BITS=64 -Ivendor -Wall -Wextra \
         -Wno-unused-parameter -Wno-pointer-sign
CXXFLAGS = $(OPT) -std=gnu++11 -Ivendor -Wall -Wextra \
           -Wno-unused-parameter
LDFLAGS ?=
LDLIBS += -lm -pthread

SRC = src/main.c src/app.c src/ui.c src/playlist.c src/tags.c \
      src/player.c src/vorbis.c src/mp4.c src/themes.c src/util.c
HDR = src/app.h src/player.h src/playlist.h src/tags.h src/themes.h \
      src/ui.h src/util.h

all: tit

# The Haiku Media Kit backend is C++ and needs the BeOS/Media Kit libs. The
# check runs in the shell so this Makefile still parses with GNU make 3.81
# (macOS) and bmake.
tit: $(SRC) $(HDR) src/haiku_audio.cpp
	@obj=; libs=; \
	if [ "$$(uname -s)" = Haiku ]; then \
		$(CXX) $(CXXFLAGS) -c -o src/haiku_audio.o src/haiku_audio.cpp || exit 1; \
		obj=src/haiku_audio.o; libs="-lbe -lmedia -lstdc++"; \
	fi; \
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $$obj $(LDLIBS) $$libs

tests/test: tests/test.c src/tags.c src/playlist.c src/themes.c src/util.c \
            src/tags.h src/playlist.h src/themes.h src/util.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test.c src/tags.c src/playlist.c \
		src/themes.c src/util.c -lm

test: tests/test
	./tests/test

install: tit
	@prefix="$(PREFIX)"; \
	if [ -z "$$prefix" ]; then \
		if [ "$$(uname -s)" = Haiku ]; then prefix="$$HOME/config/non-packaged"; \
		else prefix=/usr/local; fi; \
	fi; \
	mkdir -p "$(DESTDIR)$$prefix/bin"; \
	install -m 755 tit "$(DESTDIR)$$prefix/bin/tit"

uninstall:
	@prefix="$(PREFIX)"; \
	if [ -z "$$prefix" ]; then \
		if [ "$$(uname -s)" = Haiku ]; then prefix="$$HOME/config/non-packaged"; \
		else prefix=/usr/local; fi; \
	fi; \
	rm -f "$(DESTDIR)$$prefix/bin/tit"

clean:
	rm -f tit tests/test src/*.o

.PHONY: all test install uninstall clean
