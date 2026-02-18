# AndrathWM - dynamic window manager
# See LICENSE file for copyright and license details.

include config.mk

# Source files
SRCDIR = src
BUILDDIR = build

SRC = drw.c awm.c util.c menu.c dbus.c icon.c sni.c log.c \
	client.c monitor.c events.c ewmh.c systray.c spawn.c xrdb.c \
	status.c status_util.c status_components.c launcher.c xsource.c
SRCS = $(addprefix $(SRCDIR)/,$(SRC))
OBJ = $(addprefix $(BUILDDIR)/,$(SRC:.c=.o))

all: $(BUILDDIR) compile_flags.txt awm xidle

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

# Build xidle
xidle: $(SRCDIR)/xidle.c
	${CC} -o $@ $< -lX11 ${XSSLIBS}

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
	rm -rf awm xidle $(BUILDDIR) awm-${VERSION}.tar.gz compile_flags.txt

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
	cp -f xidle ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/xidle
	mkdir -p ${DESTDIR}${MANPREFIX}/man1
	sed "s/VERSION/${VERSION}/g" < awm.1 > ${DESTDIR}${MANPREFIX}/man1/awm.1
	chmod 644 ${DESTDIR}${MANPREFIX}/man1/awm.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/awm\
		${DESTDIR}${PREFIX}/bin/xidle\
		${DESTDIR}${MANPREFIX}/man1/awm.1

compile_flags: compile_flags.txt

.PHONY: all clean dist install uninstall compile_flags compdb
