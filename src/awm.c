/* See LICENSE file for copyright and license details.
 *
 * dynamic window manager is designed like any other X client as well. It is
 * driven through handling X events. In contrast to other X clients, a window
 * manager selects for SubstructureRedirectMask on the root window, to receive
 * events about window (dis-)appearance. Only one X connection at a time is
 * allowed to select for this event mask.
 *
 * The event handlers of awm are organized in an array which is accessed
 * whenever a new event has been fetched. This allows event dispatching
 * in O(1) time.
 *
 * Each child of the root window is called a client, except windows which have
 * set the override_redirect flag. Clients are organized in a linked client
 * list on each monitor, the focus history is remembered through a stack list
 * on each monitor. Each client contains a bit array to indicate the tags of a
 * client.
 *
 * Keys and tagging rules are organized as arrays and defined in config.h.
 *
 * To understand everything else, start reading main().
 */
#include <stdint.h>

#include <glib-unix.h>

#include "awm.h"
#include "client.h"
#include "events.h"
#include "ewmh.h"
#include "icon.h"
#include "launcher.h"
#include "monitor.h"
#include "spawn.h"
#include "status.h"
#include "systray.h"
#include "xrdb.h"
#include "xsource.h"
#define AWM_CONFIG_IMPL
#include "config.h"

/* variables */
Systray  *systray  = NULL;
Launcher *launcher = NULL;
char      stext[STATUS_TEXT_LEN];
int       screen;
int       sw, sh; /* X display screen geometry width, height */
int       bh;     /* bar height */
int       lrpad;  /* sum of left and right padding for text */
int (*xerrorxlib)(Display *, XErrorEvent *);
unsigned int numlockmask = 0;
static guint xsource_id  = 0; /* GLib source ID for the X11 event source */
#ifdef STATUSNOTIFIER
static guint dbus_src_id   = 0; /* GLib source ID for the D-Bus fd source */
static guint dbus_retry_id = 0; /* GLib source ID for the reconnect timer */
#endif
#ifdef XRANDR
int randrbase, rrerrbase;
#endif
void (*handler[LASTEvent])(XEvent *) = { [ButtonPress] = buttonpress,
	[ClientMessage]                                    = clientmessage,
	[ConfigureRequest]                                 = configurerequest,
	[ConfigureNotify]                                  = configurenotify,
	[DestroyNotify]                                    = destroynotify,
	[EnterNotify]                                      = enternotify,
	[Expose]                                           = expose,
	[FocusIn]                                          = focusin,
	[KeyPress]                                         = keypress,
	[MappingNotify]                                    = mappingnotify,
	[MapRequest]                                       = maprequest,
	[MotionNotify]                                     = motionnotify,
	[PropertyNotify]                                   = propertynotify,
	[ResizeRequest]                                    = resizerequest,
	[UnmapNotify]                                      = unmapnotify };
Atom              wmatom[WMLast], netatom[NetLast], xatom[XLast];
int               restart         = 0;
int               barsdirty       = 0;
Time              last_event_time = CurrentTime;
static GMainLoop *main_loop       = NULL;
Cur              *cursor[CurLast];
Clr             **scheme;
Display          *dpy;
Drw              *drw;
Monitor          *mons, *selmon;
Window            root, wmcheckwin;
Clientlist       *cl;

/* ---- compile-time invariants ---- */
_Static_assert(LENGTH(tags) <= 31,
    "Too many tags: bitmask must fit in 31 bits of unsigned int");
_Static_assert(LENGTH(tags) < sizeof(unsigned int) * 8,
    "LENGTH(tags) must be < bit-width of unsigned int to avoid UB in TAGMASK "
    "shift");
_Static_assert(sizeof(Atom) == sizeof(long),
    "Atom must equal long in size: Xlib format-32 property buffers use "
    "long[]");
_Static_assert(sizeof(Window) == sizeof(long),
    "Window (XID) must equal long in size: Xlib format-32 property buffers "
    "use long[]");
_Static_assert(sizeof(long) >= 4,
    "long must be at least 32 bits for all Xlib format-32 EWMH/ICCCM property "
    "writes");
_Static_assert(sizeof(unsigned long) >= 4,
    "unsigned long must be at least 32 bits for _NET_WM_WINDOW_OPACITY and "
    "ARGB pixel packing");
_Static_assert(sizeof(((Monitor *) 0)->tagset) / sizeof(unsigned int) == 2,
    "Monitor.tagset must have exactly 2 entries indexed by seltags in {0,1}");
_Static_assert(sizeof(((Monitor *) 0)->lt) / sizeof(const Layout *) == 2,
    "Monitor.lt must have exactly 2 entries indexed by sellt in {0,1}");
_Static_assert(ColBorder == 2,
    "ColBorder must be index 2; colors[][3] scheme array has exactly 3 "
    "per-scheme entries");

void
cleanup(void)
{
	Arg      a   = { .ui = ~0 };
	Layout   foo = { "", NULL };
	Monitor *m;
	size_t   i;

	view(&a);
	selmon->lt[selmon->sellt] = &foo;
	for (m = mons; m; m = m->next)
		while (m->cl->stack)
			unmanage(m->cl->stack, 0);
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	while (mons)
		cleanupmon(mons);

	if (showsystray) {
		XUnmapWindow(dpy, systray->win);
		XDestroyWindow(dpy, systray->win);
		free(systray);
	}
	status_cleanup();
	launcher_free(launcher);
#ifdef COMPOSITOR
	compositor_cleanup();
#endif

	for (i = 0; i < CurLast; i++)
		drw_cur_free(drw, cursor[i]);
	for (i = 0; i < LENGTH(colors); i++)
		free(scheme[i]);
	free(scheme);
	XDestroyWindow(dpy, wmcheckwin);
	drw_free(drw);
	XSync(dpy, False);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	if (xsource_id > 0) {
		g_source_remove(xsource_id);
		xsource_id = 0;
	}
#ifdef STATUSNOTIFIER
	if (dbus_retry_id > 0) {
		g_source_remove(dbus_retry_id);
		dbus_retry_id = 0;
	}
	if (dbus_src_id > 0) {
		g_source_remove(dbus_src_id);
		dbus_src_id = 0;
	}
	sni_cleanup();
#endif
}

void
quit(const Arg *arg)
{
	if (arg->i)
		restart = 1;
	if (main_loop)
		g_main_loop_quit(main_loop);
}

void
launchermenu(const Arg *arg)
{
	int x, y;

	if (!launcher)
		return;

	Monitor *m = selmon;
	x          = m->wx + (m->ww - 600) / 2;
	y          = m->wy + (m->wh - 400) / 2;
	if (x < m->wx)
		x = m->wx;
	if (y < m->wy)
		y = m->wy;

	launcher_show(launcher, x, y);
}

/* ---------------------------------------------------------------------------
 * X event dispatch callback — called by the XSource on each loop iteration
 * where X events are available.
 * ------------------------------------------------------------------------- */
static gboolean
x_dispatch_cb(gpointer user_data)
{
	XEvent ev;
	(void) user_data;

	while (XPending(dpy)) {
		XNextEvent(dpy, &ev);
#ifdef COMPOSITOR
		/* Apply the XESetWireToEvent workaround for every event before
		 * any handler sees it.  This prevents GL/DRI2 wire-to-event
		 * hooks from corrupting Xlib's sequence tracking. */
		compositor_fix_wire_to_event(&ev);
#endif
#ifdef XRANDR
		if (ev.type == randrbase + RRScreenChangeNotify) {
			XRRUpdateConfiguration(&ev);
			updategeom();
			drw_resize(drw, sw, bh);
			updatebars();
			for (Monitor *m = mons; m; m = m->next) {
				for (Client *c = m->cl->clients; c; c = c->next)
					if (c->isfullscreen)
						resizeclient(c, m->mx, m->my, m->mw, m->mh);
				resizebarwin(m);
			}
			focus(NULL);
			arrange(NULL);
		} else
#endif
#ifdef STATUSNOTIFIER
			/* Handle menu events BEFORE normal handlers if menu is visible */
			if (!sni_handle_menu_event(&ev)) {
#endif
				/* Handle launcher events if visible */
				if (launcher && launcher->visible) {
#ifdef COMPOSITOR
					compositor_handle_event(&ev);
#endif
					if (!launcher_handle_event(launcher, &ev) &&
					    ev.type < LASTEvent && handler[ev.type])
						handler[ev.type](&ev);
				} else {
#ifdef COMPOSITOR
					compositor_handle_event(&ev);
#endif
					if (ev.type < LASTEvent && handler[ev.type])
						handler[ev.type](&ev);
				}
#ifdef STATUSNOTIFIER
			}
#endif
	}

	if (barsdirty) {
		drawbars();
		updatesystray();
		barsdirty = 0;
	}

	return G_SOURCE_CONTINUE;
}

#ifdef STATUSNOTIFIER
/* Forward declaration — dbus_dispatch_cb is defined after
 * sni_attach_dbus_source */
static gboolean dbus_dispatch_cb(
    gint fd, GIOCondition condition, gpointer user_data);

/* Attach a new GUnixFDSource for the D-Bus fd to the given context.
 * Removes any previously registered source first. */
static void
sni_attach_dbus_source(GMainContext *ctx)
{
	GSource *src;
	int      fd;

	if (dbus_src_id > 0) {
		g_source_remove(dbus_src_id);
		dbus_src_id = 0;
	}

	fd = sni_get_fd();
	if (fd < 0)
		return;

	src = g_unix_fd_source_new(fd, G_IO_IN | G_IO_HUP | G_IO_ERR);
	g_source_set_callback(src, (GSourceFunc) dbus_dispatch_cb, NULL, NULL);
	dbus_src_id = g_source_attach(src, ctx);
	g_source_unref(src);
}

/* One-shot timer callback: attempt to reconnect to D-Bus after a disconnect.
 */
static gboolean
dbus_reconnect_cb(gpointer user_data)
{
	GMainContext *ctx = (GMainContext *) user_data;

	dbus_retry_id = 0; /* this one-shot timer has fired */
	awm_warn("D-Bus: attempting reconnect...");
	if (!sni_reconnect()) {
		awm_error("D-Bus: reconnect failed — will retry in 5 s");
		/* Keep retrying every 5 s until we succeed. */
		dbus_retry_id = g_timeout_add_seconds(5, dbus_reconnect_cb, ctx);
	} else {
		sni_attach_dbus_source(ctx);
		awm_warn("D-Bus: reconnected successfully");
	}

	return G_SOURCE_REMOVE; /* one-shot */
}

/* D-Bus dispatch callback — called by the GUnixFDSource when dbus_fd is
 * readable, or on HUP/ERR when the D-Bus daemon disappears. */
static gboolean
dbus_dispatch_cb(gint fd, GIOCondition condition, gpointer user_data)
{
	(void) fd;

	if (condition & (G_IO_HUP | G_IO_ERR)) {
		awm_error("D-Bus connection lost (HUP/ERR) — scheduling reconnect");
		dbus_src_id = 0; /* source is being removed by returning REMOVE */
		/* Schedule a reconnect attempt 2 s from now on the same context. */
		dbus_retry_id = g_timeout_add_seconds(
		    2, dbus_reconnect_cb, g_main_loop_get_context(main_loop));
		return G_SOURCE_REMOVE;
	}

	(void) user_data;
	sni_handle_dbus();
	return G_SOURCE_CONTINUE;
}

#endif /* STATUSNOTIFIER */

void
run(void)
{
	GMainContext *ctx;

	XSync(dpy, False);

	ctx = g_main_context_default();

	/* X11 source — wakes the loop whenever X events are pending */
	xsource_id = xsource_attach(dpy, ctx, x_dispatch_cb, NULL);

#ifdef STATUSNOTIFIER
	/* D-Bus source — use helper so reconnect can re-attach cleanly */
	sni_attach_dbus_source(ctx);
#endif

	main_loop = g_main_loop_new(ctx, FALSE);
	/* Let xsource_dispatch quit the loop cleanly on X server death
	 * instead of calling exit(1), so cleanup() can run. */
	xsource_set_quit_loop(main_loop);
	g_main_loop_run(main_loop);
	xsource_set_quit_loop(NULL);
	g_main_loop_unref(main_loop);
	main_loop = NULL;
}

void
scan(void)
{
	unsigned int      i, num;
	Window            d1, d2, *wins = NULL;
	XWindowAttributes wa;

	if (XQueryTree(dpy, root, &d1, &d2, &wins, &num)) {
		for (i = 0; i < num; i++) {
			if (!XGetWindowAttributes(dpy, wins[i], &wa) ||
			    wa.override_redirect ||
			    XGetTransientForHint(dpy, wins[i], &d1))
				continue;
			if (wa.map_state == IsViewable || getstate(wins[i]) == IconicState)
				manage(wins[i], &wa);
		}
		for (i = 0; i < num; i++) { /* now the transients */
			if (!XGetWindowAttributes(dpy, wins[i], &wa))
				continue;
			if (XGetTransientForHint(dpy, wins[i], &d1) &&
			    (wa.map_state == IsViewable ||
			        getstate(wins[i]) == IconicState))
				manage(wins[i], &wa);
		}
		if (wins)
			XFree(wins);
	}
}

void
setup(void)
{
	int                  i;
	XSetWindowAttributes wa;
	Atom                 utf8string;
	struct sigaction     sa;

	/* do not transform children into zombies when they terminate */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags   = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);

	/* clean up any zombies (inherited from .xinitrc etc) immediately */
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;

	/* init screen */
	screen = DefaultScreen(dpy);
	sw     = DisplayWidth(dpy, screen);
	sh     = DisplayHeight(dpy, screen);
	if (!(cl = (Clientlist *) calloc(1, sizeof(Clientlist))))
		die("fatal: could not malloc() %u bytes\n", sizeof(Clientlist));
	root = RootWindow(dpy, screen);
	drw  = drw_create(dpy, screen, root, sw, sh);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;
	bh    = drw->fonts->h + 2;
	updategeom();
	/* Enable RandR screen change notifications */
#ifdef XRANDR
	if (XRRQueryExtension(dpy, &randrbase, &rrerrbase)) {
		XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);
	}
#endif
	/* init atoms */
	utf8string               = XInternAtom(dpy, "UTF8_STRING", False);
	wmatom[WMProtocols]      = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmatom[WMDelete]         = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	wmatom[WMState]          = XInternAtom(dpy, "WM_STATE", False);
	wmatom[WMTakeFocus]      = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
	netatom[NetActiveWindow] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	netatom[NetSupported]    = XInternAtom(dpy, "_NET_SUPPORTED", False);
	netatom[NetSystemTray]   = XInternAtom(dpy, "_NET_SYSTEM_TRAY_S0", False);
	netatom[NetSystemTrayOP] =
	    XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	netatom[NetSystemTrayOrientation] =
	    XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
	netatom[NetSystemTrayOrientationHorz] =
	    XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION_HORZ", False);
	netatom[NetSystemTrayColors] =
	    XInternAtom(dpy, "_NET_SYSTEM_TRAY_COLORS", False);
	netatom[NetSystemTrayVisual] =
	    XInternAtom(dpy, "_NET_SYSTEM_TRAY_VISUAL", False);
	netatom[NetWMName]  = XInternAtom(dpy, "_NET_WM_NAME", False);
	netatom[NetWMIcon]  = XInternAtom(dpy, "_NET_WM_ICON", False);
	netatom[NetWMState] = XInternAtom(dpy, "_NET_WM_STATE", False);
	netatom[NetWMCheck] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	netatom[NetWMFullscreen] =
	    XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	netatom[NetWMStateDemandsAttention] =
	    XInternAtom(dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
	netatom[NetWMStateSticky] =
	    XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
	netatom[NetWMStateAbove] = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
	netatom[NetWMStateBelow] = XInternAtom(dpy, "_NET_WM_STATE_BELOW", False);
	netatom[NetWMStateHidden] =
	    XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
	netatom[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	netatom[NetWMWindowTypeDialog] =
	    XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	netatom[NetClientList] = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	netatom[NetClientListStacking] =
	    XInternAtom(dpy, "_NET_CLIENT_LIST_STACKING", False);
	netatom[NetWMDesktop] = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
	netatom[NetWMPid]     = XInternAtom(dpy, "_NET_WM_PID", False);
	netatom[NetDesktopViewport] =
	    XInternAtom(dpy, "_NET_DESKTOP_VIEWPORT", False);
	netatom[NetNumberOfDesktops] =
	    XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
	netatom[NetCurrentDesktop] =
	    XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
	netatom[NetDesktopNames] = XInternAtom(dpy, "_NET_DESKTOP_NAMES", False);
	netatom[NetWorkarea]     = XInternAtom(dpy, "_NET_WORKAREA", False);
	netatom[NetCloseWindow]  = XInternAtom(dpy, "_NET_CLOSE_WINDOW", False);
	netatom[NetMoveResizeWindow] =
	    XInternAtom(dpy, "_NET_MOVERESIZE_WINDOW", False);
	netatom[NetFrameExtents] = XInternAtom(dpy, "_NET_FRAME_EXTENTS", False);
#ifdef COMPOSITOR
	netatom[NetWMWindowOpacity] =
	    XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
	netatom[NetWMBypassCompositor] =
	    XInternAtom(dpy, "_NET_WM_BYPASS_COMPOSITOR", False);
#endif
	xatom[Manager]    = XInternAtom(dpy, "MANAGER", False);
	xatom[Xembed]     = XInternAtom(dpy, "_XEMBED", False);
	xatom[XembedInfo] = XInternAtom(dpy, "_XEMBED_INFO", False);
	/* init cursors */
	cursor[CurNormal] = drw_cur_create(drw, XC_left_ptr);
	cursor[CurResize] = drw_cur_create(drw, XC_sizing);
	cursor[CurMove]   = drw_cur_create(drw, XC_fleur);
	/* init appearance */
	scheme = ecalloc(LENGTH(colors), sizeof(Clr *));
	for (i = 0; i < LENGTH(colors); i++)
		scheme[i] = drw_scm_create(drw, colors[i], 3);
	status_init(g_main_context_default());
	/* init system tray */
	updatesystray();
	/* init bars */
	updatebars();
	updatestatus();
	/* supporting window for NetWMCheck */
	wmcheckwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMCheck], XA_WINDOW, 32,
	    PropModeReplace, (unsigned char *) &wmcheckwin, 1);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMName], utf8string, 8,
	    PropModeReplace, (unsigned char *) "awm", 3);
	XChangeProperty(dpy, root, netatom[NetWMCheck], XA_WINDOW, 32,
	    PropModeReplace, (unsigned char *) &wmcheckwin, 1);
	/* EWMH support per view */
	XChangeProperty(dpy, root, netatom[NetSupported], XA_ATOM, 32,
	    PropModeReplace, (unsigned char *) netatom, NetLast);
	setnumdesktops();
	setcurrentdesktop();
	setdesktopnames();
	setviewport();
	/* Update workarea for all monitors */
	{
		Monitor *m;
		for (m = mons; m; m = m->next)
			updateworkarea(m);
	}
	XDeleteProperty(dpy, root, netatom[NetClientList]);
	/* select events */
	wa.cursor     = cursor[CurNormal]->cursor;
	wa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
	    ButtonPressMask | PointerMotionMask | EnterWindowMask |
	    LeaveWindowMask | StructureNotifyMask | PropertyChangeMask;
	XChangeWindowAttributes(dpy, root, CWEventMask | CWCursor, &wa);
	XSelectInput(dpy, root, wa.event_mask);
	grabkeys();
	focus(NULL);
	/* Initialize icon subsystem (GTK, cache) unconditionally */
	icon_init();
#ifdef STATUSNOTIFIER
	/* Initialize StatusNotifier support */
	if (!sni_init(dpy, root, drw, scheme, sniconsize))
		awm_warn("Failed to initialize StatusNotifier support");
#endif
	/* Initialize launcher */
	launcher = launcher_create(dpy, root, drw, scheme, termcmd[0]);
#ifdef COMPOSITOR
	if (compositor_init(g_main_context_default()) < 0)
		awm_warn("compositor: init failed, running without compositing");
#endif
}

static int
xioerror(Display *d)
{
	(void) d;
	/* _XIOError: the X server closed the connection (or the socket died).
	 * This fires when the server forcibly drops our connection, e.g. due to
	 * a fatal GLX protocol error.  Log it before libc calls exit(). */
	awm_error("X IO error: X server connection lost (fatal GLX/X protocol "
	          "error likely); awm is exiting");
	/* Xlib requires this handler to not return — call exit directly. */
	exit(1);
	return 0; /* unreachable, silences -Werror=return-type */
}

int
main(int argc, char *argv[])
{
	/* Initialize logging subsystem */
	log_init("awm");

	if (argc == 2 && !strcmp("-v", argv[1]))
		die("awm-" VERSION);
	else if (argc != 1)
		die("usage: awm [-v]");
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("awm: cannot open display");
	XSetIOErrorHandler(xioerror);
	checkotherwm();
	XrmInitialize();
	loadxrdb();
	setup();
#ifdef __OpenBSD__
	if (pledge("stdio rpath proc exec unix inet", NULL) == -1)
		die("pledge");
#endif /* __OpenBSD__ */
	scan();
	if (!restart && !getenv("RESTARTED"))
		runautostart();
	/* Always re-apply Xresources after scan: on a fresh start
	 * autostart_blocking.sh may have just run `xrdb -merge`; on an
	 * execvp restart the static color strings are re-initialised to
	 * compile-time defaults so we must reload them even though
	 * RESOURCE_MANAGER is already set on the root window. */
	xrdb(NULL);
	run();
	if (restart) {
		setenv("RESTARTED", "1", 1); /* overwrite=1: always update */
		execvp(argv[0], argv);
		awm_error("execvp failed: %s", strerror(errno));
	}
	cleanup();
	log_cleanup();
	XCloseDisplay(dpy);
	return EXIT_SUCCESS;
}
