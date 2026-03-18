# AndrathWM - dynamic window manager
# See LICENSE file for copyright and license details.
# Requires GNU Make (use 'gmake' on BSD systems)

include config.mk

# Source files
SRCDIR = src
BUILDDIR = build

# Drawing backend: render_cairo_xcb.c implements the RenderBackend vtable.
# The legacy drw.c / drw_cairo.c backends have been removed.
RENDER_SRC = render_cairo_xcb.c

SRC = $(RENDER_SRC) platform_x11.c awm.c util.c menu.c dbus.c icon.c sni.c log.c \
	client.c monitor.c events.c ewmh.c systray.c spawn.c xrdb.c \
	status.c status_util.c status_components.c platform_x11_source.c \
	compositor.c compositor_egl.c compositor_xrender.c switcher.c \
	wmstate.c
SRCS = $(addprefix $(SRCDIR)/,$(SRC))
OBJ = $(addprefix $(BUILDDIR)/,$(SRC:.c=.o))

# awm-ui: separate GTK helper process (launcher + SNI menus)
UI_SRC  = awm_ui.c launcher.c icon.c log.c util.c notif.c preview.c
UI_SRCS = $(addprefix $(SRCDIR)/,$(UI_SRC))
UI_OBJ  = $(addprefix $(BUILDDIR)/ui_,$(UI_SRC:.c=.o))

all: $(BUILDDIR) compile_flags.txt awm awm-ui xidle

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Compile object files
$(BUILDDIR)/%.o: $(SRCDIR)/%.c config.h status_config.h config.mk | $(BUILDDIR)
	${CC} -c ${CFLAGS} -o $@ $<

config.h:
	cp config.def.h $@

# Link awm
awm: $(OBJ)
	${CC} -o $@ ${OBJ} ${LDFLAGS}

# Compile object files for awm-ui (prefixed with ui_ to avoid collisions)
$(BUILDDIR)/ui_%.o: $(SRCDIR)/%.c config.h status_config.h config.mk | $(BUILDDIR)
	${CC} -c ${CFLAGS} -o $@ $<

# Link awm-ui
awm-ui: $(UI_OBJ)
	${CC} -o $@ ${UI_OBJ} ${LDFLAGS} -lX11-xcb

# Build xidle (XCB screensaver — no Xlib dependency)
xidle: $(SRCDIR)/xidle.c
	${CC} -std=c11 $(shell pkg-config --cflags xcb) -o $@ $< $(shell pkg-config --libs xcb xcb-screensaver)

compile_flags.txt: config.mk
	printf '%s\n' $(INCS) $(CPPFLAGS) > $@

# Generate compile_commands.json for clangd.  Requires a clean build so
# all compile lines appear in make output.
compdb: config.h status_config.h
	$(MAKE) clean
	$(MAKE) --dry-run 2>/dev/null | grep '^${CC} -c' | python3 -c "\
import sys,json,re,os; cwd=os.getcwd(); entries=[\
{'directory':cwd,'command':re.sub(r'-o build/\S+','',l.strip()),'file':os.path.join(cwd,re.search(r'(src/\S+\.c)$$',l.strip()).group(1))}\
for l in sys.stdin if re.search(r'src/\S+\.c$$',l)]; print(json.dumps(entries,indent=2))" > compile_commands.json
	$(MAKE)

clean:
	rm -rf awm awm-ui xidle $(BUILDDIR) awm-${VERSION}.tar.gz compile_flags.txt

dist: clean
	mkdir -p awm-${VERSION}
	cp -R LICENSE THIRD_PARTY Makefile README.md config.def.h status_config.h \
		config.mk awm.1 awm.png src docs examples third_party awm-${VERSION}
	tar -cf awm-${VERSION}.tar awm-${VERSION}
	gzip awm-${VERSION}.tar
	rm -rf awm-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f awm ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/awm
	cp -f awm-ui ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/awm-ui
	cp -f xidle ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/xidle
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < awm.1 > ${DESTDIR}${MANPREFIX}/man1/awm.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/awm.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/awm\
		${DESTDIR}${PREFIX}/bin/awm-ui\
		${DESTDIR}${PREFIX}/bin/xidle\
		${DESTDIR}${MANPREFIX}/man1/awm.1

compile_flags: compile_flags.txt

# Test suite — no XCB/GTK linking; only pure-C modules.
TEST_CC     = clang
TEST_CFLAGS = -std=c11 -pedantic -Werror -Wall -I. -Isrc -Itests
TEST_SRCS   = src/status_util.c src/log.c
TEST_BINS   = build/test_status_util

# Full-flags test suite: tests that include a .c file directly and need
# all XCB/GLib/Cairo/GTK/D-Bus type definitions available, but do NOT
# actually open an X connection at runtime.
TEST_FULL_CFLAGS = -std=c11 -pedantic -Werror -Wall -I. -Isrc -Itests \
	$(shell pkg-config --cflags xcb xcb-icccm xcb-randr xcb-keysyms \
	    xcb-xinerama xcb-cursor xcb-renderutil pangocairo glib-2.0 \
	    cairo gtk+-3.0 dbus-1) \
	-DXINERAMA -DXRANDR -DCOMPOSITOR -DSTATUSNOTIFIER -DXSS \
	-DBACKEND_X11 \
	-DVERSION=\"$(VERSION)\" -D_DEFAULT_SOURCE -D_BSD_SOURCE \
	-D_XOPEN_SOURCE=700L
TEST_FULL_LIBS = $(shell pkg-config --libs xcb xcb-icccm xcb-randr \
	xcb-keysyms xcb-xinerama xcb-cursor xcb-renderutil pangocairo \
	glib-2.0 cairo)
TEST_FULL_BINS = build/test_xrdb build/test_monitor build/test_client \
	build/test_sizehints

build/test_status_util: tests/test_status_util.c $(TEST_SRCS) tests/greatest.h | $(BUILDDIR)
	$(TEST_CC) $(TEST_CFLAGS) -o $@ tests/test_status_util.c $(TEST_SRCS)

build/test_xrdb: tests/test_xrdb.c tests/greatest.h config.h | $(BUILDDIR)
	$(TEST_CC) $(TEST_FULL_CFLAGS) -o $@ tests/test_xrdb.c $(TEST_FULL_LIBS)

build/test_monitor: tests/test_monitor.c tests/greatest.h config.h | $(BUILDDIR)
	$(TEST_CC) $(TEST_FULL_CFLAGS) -o $@ tests/test_monitor.c $(TEST_FULL_LIBS)

build/test_client: tests/test_client.c tests/greatest.h config.h | $(BUILDDIR)
	$(TEST_CC) $(TEST_FULL_CFLAGS) -o $@ tests/test_client.c $(TEST_FULL_LIBS)

build/test_sizehints: tests/test_sizehints.c tests/greatest.h config.h | $(BUILDDIR)
	$(TEST_CC) $(TEST_FULL_CFLAGS) -o $@ tests/test_sizehints.c $(TEST_FULL_LIBS)

test: $(TEST_BINS) $(TEST_FULL_BINS)
	@for t in $(TEST_BINS) $(TEST_FULL_BINS); do \
		echo "Running $$t ..."; \
		$$t || exit 1; \
	done

.PHONY: all clean dist install uninstall compile_flags compdb test
