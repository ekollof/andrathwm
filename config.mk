# AndrathWM version
VERSION = 0.1

# Customize below to fit your system

# paths
PREFIX = /usr
MANPREFIX = ${PREFIX}/share/man

# Xinerama, comment if you don't want it
XINERAMALIBS  =
XINERAMAFLAGS = -DXINERAMA

# RandR (modern multi-monitor support), comment if you don't want it
RANDRLIBS  =
RANDRFLAGS = -DXRANDR

# XScreenSaver extension (idle detection), comment if you don't want it
# xidle uses xcb-screensaver directly; no Xss link needed for awm itself
XSSLIBS  =
XSSFLAGS = -DXSS

# StatusNotifier/AppIndicator support (D-Bus based system tray), comment if you don't want it
SNIINC = $(shell pkg-config --cflags dbus-1 cairo gtk+-3.0 librsvg-2.0)
SNILIBS = $(shell pkg-config --libs dbus-1 cairo gtk+-3.0 librsvg-2.0)
SNIFLAGS = -DSTATUSNOTIFIER

# XCB utility libraries
XCBLIBS  = $(shell pkg-config --libs xcb-icccm xcb-randr xcb-keysyms xcb-xinerama xcb-cursor xcb-renderutil) -lxkbcommon
XCBINC   = $(shell pkg-config --cflags xcb-icccm xcb-randr xcb-keysyms xcb-xinerama xcb-cursor xcb-renderutil)

# Built-in XRender compositor, comment if you don't want it
# All compositor I/O goes through XCB — no Xlib compositor libs needed
COMPOSITORLIBS  = -lGL -lEGL -lxcb -lxcb-render -lxcb-present -lxcb-composite -lxcb-damage -lxcb-xfixes -lxcb-shape -lxcb-render-util
COMPOSITORFLAGS = -DCOMPOSITOR

# PangoCairo (replaces Xft/freetype for text rendering)
PANGOLIBS = $(shell pkg-config --libs pangocairo)
PANGOINC  = $(shell pkg-config --cflags pangocairo)
MANPREFIX = ${PREFIX}/man

# includes and libs
INCS = -I. -Isrc -Ithird_party ${PANGOINC} ${SNIINC} ${XCBINC}
LIBS = ${XINERAMALIBS} ${RANDRLIBS} ${XSSLIBS} ${PANGOLIBS} ${SNILIBS} ${COMPOSITORLIBS} ${XCBLIBS}

# flags
CPPFLAGS = -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_XOPEN_SOURCE=700L -DVERSION=\"${VERSION}\" ${XINERAMAFLAGS} ${RANDRFLAGS} ${XSSFLAGS} ${SNIFLAGS} ${COMPOSITORFLAGS}
# Uncomment to enable debug logging (icon rendering, etc.):
#CPPFLAGS += -DAWM_DEBUG
#CFLAGS   = -g -std=c99 -pedantic -Wall -O0 ${INCS} ${CPPFLAGS}
CFLAGS   = -g3 -std=c11 -pedantic -Werror -Wall -Wno-deprecated-declarations -Os ${INCS} ${CPPFLAGS}
LDFLAGS  = ${LIBS}

# Solaris
#CFLAGS = -fast ${INCS} -DVERSION=\"${VERSION}\"
#LDFLAGS = ${LIBS}

# compiler and linker
CC = clang
