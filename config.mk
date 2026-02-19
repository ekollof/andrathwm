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

# Built-in XRender compositor, comment if you don't want it
COMPOSITORLIBS  = -lXcomposite -lXdamage -lXrender -lXfixes -lXext -lGL -lEGL -l:libX11-xcb.so.1
COMPOSITORFLAGS = -DCOMPOSITOR

# freetype
FREETYPELIBS = -lfontconfig -lXft
FREETYPEINC = /usr/include/freetype2
# OpenBSD (uncomment)
#FREETYPEINC = ${X11INC}/freetype2
MANPREFIX = ${PREFIX}/man

# includes and libs
INCS = -I. -Isrc -Ithird_party -I${X11INC} -I${FREETYPEINC} ${SNIINC}
LIBS = -L${X11LIB} -lX11 ${XINERAMALIBS} ${RANDRLIBS} ${XSSLIBS} ${FREETYPELIBS} ${SNILIBS} ${COMPOSITORLIBS}

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
