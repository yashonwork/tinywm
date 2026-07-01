# river-wm - dwm-like window manager for River Wayland compositor
# See LICENSE file for copyright and license details.

include config.mk

PROTO = protocol
RIVER_XML = $(PROTO)/river-window-management-v1.xml \
            $(PROTO)/river-xkb-bindings-v1.xml

GEN_HEADERS = river-window-management-v1-client-protocol.h \
              river-xkb-bindings-v1-client-protocol.h

GEN_SOURCES = river-window-management-v1-protocol.c \
              river-xkb-bindings-v1-protocol.c

GEN_OBJ = $(GEN_SOURCES:.c=.o)

OBJ = river-wm.o layout.o $(GEN_OBJ)

all: river-wm

river-wm: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c config.h config.mk $(GEN_HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Generate protocol headers and C stubs from Wayland XML
river-window-management-v1-client-protocol.h: $(PROTO)/river-window-management-v1.xml
	wayland-scanner client-header $< $@

river-window-management-v1-protocol.c: $(PROTO)/river-window-management-v1.xml
	wayland-scanner private-code $< $@

river-xkb-bindings-v1-client-protocol.h: $(PROTO)/river-xkb-bindings-v1.xml
	wayland-scanner client-header $< $@

river-xkb-bindings-v1-protocol.c: $(PROTO)/river-xkb-bindings-v1.xml
	wayland-scanner private-code $< $@

config.h:
	cp config.def.h $@

clean:
	rm -f river-wm $(OBJ) $(GEN_HEADERS) $(GEN_SOURCES)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f river-wm $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/river-wm

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/river-wm

.PHONY: all clean install uninstall
