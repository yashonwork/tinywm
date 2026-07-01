VERSION = 0.1
PREFIX = /usr/local
MANDIR = $(PREFIX)/share/man

WAYLAND = wayland-client
XKBCOMMON = xkbcommon

INCS = $(shell pkg-config --cflags $(WAYLAND) $(XKBCOMMON))
LIBS = $(shell pkg-config --libs $(WAYLAND) $(XKBCOMMON))

CPPFLAGS = -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\"
CFLAGS   = -std=c99 -pedantic -Wall -Wno-deprecated-declarations $(INCS) $(CPPFLAGS)
LDFLAGS  = $(LIBS)

CC = cc
