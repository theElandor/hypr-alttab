PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client)
WAYLAND_LIBS := $(shell pkg-config --libs wayland-client)
SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null || echo wayland-scanner)

PROTO := protocols/hyprland-toplevel-export-v1.xml
GEN := build/hyprland-toplevel-export-v1-client-protocol.h build/hyprland-toplevel-export-v1-protocol.c

all: build/hypr-alttab-capture

build:
	mkdir -p build

build/hyprland-toplevel-export-v1-client-protocol.h: $(PROTO) | build
	$(SCANNER) client-header $< $@

build/hyprland-toplevel-export-v1-protocol.c: $(PROTO) | build
	$(SCANNER) private-code $< $@

build/hypr-alttab-capture: capture/capture.c $(GEN)
	$(CC) $(CFLAGS) -Ibuild $(WAYLAND_CFLAGS) -o $@ capture/capture.c build/hyprland-toplevel-export-v1-protocol.c $(WAYLAND_LIBS)

install: all
	install -Dm755 build/hypr-alttab-capture $(DESTDIR)$(BINDIR)/hypr-alttab-capture
	install -Dm755 hypr-alttab $(DESTDIR)$(BINDIR)/hypr-alttab
	install -Dm755 hypr-alttab-ctl $(DESTDIR)$(BINDIR)/hypr-alttab-ctl

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/hypr-alttab-capture $(DESTDIR)$(BINDIR)/hypr-alttab $(DESTDIR)$(BINDIR)/hypr-alttab-ctl

clean:
	rm -rf build

.PHONY: all install uninstall clean
