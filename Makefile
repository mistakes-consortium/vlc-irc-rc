PREFIX = /usr
LD = ld
CC = cc
INSTALL = install
CFLAGS = -g -O2 -Wall -Wextra
LDFLAGS =
VLC_PLUGIN_CFLAGS := $(shell pkg-config --cflags vlc-plugin)
VLC_PLUGIN_LIBS := $(shell pkg-config --libs vlc-plugin)

libdir = $(PREFIX)/lib64
plugindir = $(libdir)/vlc/plugins

override CC += -std=gnu99
override CPPFLAGS += -DPIC -I. -Isrc
override CFLAGS += -fPIC
override LDFLAGS += -Wl,-no-undefined,-z,defs

override CFLAGS += $(VLC_PLUGIN_CFLAGS)
override LDFLAGS += $(VLC_PLUGIN_LIBS)

TARGETS = libircrc_plugin.so

all: libircrc_plugin.so

install: all
	mkdir -p -- $(DESTDIR)$(plugindir)/misc
	$(INSTALL) --mode 0755 libircrc_plugin.so $(DESTDIR)$(plugindir)/misc

install-strip:
	$(MAKE) install INSTALL="$(INSTALL) -s"

uninstall:
	rm -f $(plugindir)/misc/libircrc_plugin.so

clean:
	rm -f -- libircrc_plugin.so src/*.o

mostlyclean: clean

SOURCES = ircrc.c

$(SOURCES:%.c=src/%.o): %: src/ircrc.c

libircrc_plugin.so: $(SOURCES:%.c=src/%.o)
	$(CC) $(LDFLAGS) -shared -o $@ $^

.PHONY: all install install-strip uninstall clean mostlyclean
