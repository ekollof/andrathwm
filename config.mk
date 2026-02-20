# AndrathWM version
VERSION = 0.1

# Customize below to fit your system

# paths
PREFIX = /usr
MANPREFIX = ${PREFIX}/share/man

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib

# Xinerama, comment if you don't want it
XINERAMALIBS  = -lXinerama
XINERAMAFLAGS = -DXINERAMA

# RandR (modern multi-monitor support), comment if you don't want it
RANDRLIBS  = -lXrandr
RANDRFLAGS = -DXRANDR

# XScreenSaver extension (idle detection), comment if you don't want it
XSSLIBS  = -lXss
XSSFLAGS = -DXSS

# StatusNotifier/AppIndicator support (D-Bus based system tray), comment if you don't want it
SNIINC = $(shell pkg-config --cflags dbus-1 cairo gtk+-3.0 librsvg-2.0)
SNILIBS = $(shell pkg-config --libs dbus-1 cairo gtk+-3.0 librsvg-2.0)
SNIFLAGS = -DSTATUSNOTIFIER

# XCB utility libraries
XCBLIBS  = $(shell pkg-config --libs xcb-icccm xcb-randr xcb-keysyms)
XCBINC   = $(shell pkg-config --cflags xcb-icccm xcb-randr xcb-keysyms)

# Built-in XRender compositor, comment if you don't want it
COMPOSITORLIBS  = -lXcomposite -lXdamage -lXrender -lXfixes -lXext -lGL -lEGL -l:libX11-xcb.so.1 -lxcb -lxcb-render -lxcb-present
COMPOSITORFLAGS = -DCOMPOSITOR

# PangoCairo (replaces Xft/freetype for text rendering)
PANGOLIBS = $(shell pkg-config --libs pangocairo)
PANGOINC  = $(shell pkg-config --cflags pangocairo)
MANPREFIX = ${PREFIX}/man

# includes and libs
INCS = -I. -Isrc -Ithird_party -I${X11INC} ${PANGOINC} ${SNIINC} ${XCBINC}
LIBS = -L${X11LIB} -lX11 ${XINERAMALIBS} ${RANDRLIBS} ${XSSLIBS} ${PANGOLIBS} ${SNILIBS} ${COMPOSITORLIBS} ${XCBLIBS}

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
