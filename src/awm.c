/* See LICENSE file for copyright and license details.
 *
 * dynamic window manager is designed like any other X client as well. It is
 * driven through handling X events. In contrast to other X clients, a window
 * manager selects for XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT on the root window,
 * to receive events about window (dis-)appearance. Only one X connection at a
 * time is allowed to select for this event mask.
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
void (*handler[LASTEvent])(xcb_generic_event_t *) = {
	[ButtonPress]      = buttonpress,
	[ClientMessage]    = clientmessage,
	[ConfigureRequest] = configurerequest,
	[ConfigureNotify]  = configurenotify,
	[DestroyNotify]    = destroynotify,
	[EnterNotify]      = enternotify,
	[Expose]           = expose,
	[FocusIn]          = focusin,
	[KeyPress]         = keypress,
	[MappingNotify]    = mappingnotify,
	[MapRequest]       = maprequest,
	[MotionNotify]     = motionnotify,
	[PropertyNotify]   = propertynotify,
	[ResizeRequest]    = resizerequest,
	[UnmapNotify]      = unmapnotify,
};
xcb_atom_t         wmatom[WMLast], netatom[NetLast], xatom[XLast];
static xcb_atom_t  utf8string_atom; /* UTF8_STRING — used in setup() */
int                restart         = 0;
int                barsdirty       = 0;
xcb_timestamp_t    last_event_time = XCB_CURRENT_TIME;
static GMainLoop  *main_loop       = NULL;
Cur               *cursor[CurLast];
Clr              **scheme;
xcb_connection_t  *xc;
Drw               *drw;
Monitor           *mons, *selmon;
xcb_window_t       root, wmcheckwin;
Clientlist        *cl;
xcb_key_symbols_t *keysyms;

/* ---- compile-time invariants ---- */
_Static_assert(LENGTH(tags) <= 31,
    "Too many tags: bitmask must fit in 31 bits of unsigned int");
_Static_assert(LENGTH(tags) < sizeof(unsigned int) * 8,
    "LENGTH(tags) must be < bit-width of unsigned int to avoid UB in TAGMASK "
    "shift");
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
	{

		xcb_ungrab_key(xc, XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);
	}
	while (mons)
		cleanupmon(mons);

	if (showsystray) {

		xcb_unmap_window(xc, systray->win);
		xcb_destroy_window(xc, systray->win);
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
	xcb_destroy_window(xc, wmcheckwin);
	drw_free(drw);
	xcb_key_symbols_free(keysyms);
	keysyms = NULL;
	xflush();
	xcb_set_input_focus(xc, XCB_INPUT_FOCUS_POINTER_ROOT,
	    XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
	xcb_delete_property(xc, root, netatom[NetActiveWindow]);
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
	xcb_generic_event_t *ev;
	(void) user_data;

	while ((ev = xcb_poll_for_event(xc))) {
		uint8_t type = ev->response_type & ~0x80;
#ifdef XRANDR
		if (type == (uint8_t) (randrbase + XCB_RANDR_SCREEN_CHANGE_NOTIFY)) {
			/* XCB randr handles screen change — no XRRUpdateConfiguration
			 * needed since we don't use libXrandr data structures. */
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
			if (!sni_handle_menu_event(ev)) {
#endif
				/* Handle launcher events if visible */
				if (launcher && launcher->visible) {
#ifdef COMPOSITOR
					compositor_handle_event(ev);
#endif
					if (!launcher_handle_event(launcher, ev) &&
					    type < LASTEvent && handler[type])
						handler[type](ev);
				} else {
#ifdef COMPOSITOR
					compositor_handle_event(ev);
#endif
					if (type < LASTEvent && handler[type])
						handler[type](ev);
				}
#ifdef STATUSNOTIFIER
			}
#endif
		free(ev);
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
static gboolean dbus_reconnect_cb(gpointer user_data);

/* Called from sni.c when NameOwnerChanged reveals our watcher name was stolen.
 * Schedules a reconnect on the GLib main context (idempotent: no-op if a
 * retry is already in flight). */
void
sni_schedule_reconnect(void)
{
	GMainContext *ctx;

	if (dbus_retry_id > 0)
		return; /* already scheduled */

	ctx           = g_main_loop_get_context(main_loop);
	dbus_retry_id = g_timeout_add_seconds(1, dbus_reconnect_cb, ctx);
	awm_warn("D-Bus: name-loss detected — reconnect scheduled in 1 s");
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

	xflush();

	ctx = g_main_context_default();

	/* X11 source — wakes the loop whenever X events are pending */
	xsource_id = xsource_attach(xc, ctx, x_dispatch_cb, NULL);

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
	unsigned int i;

	xcb_query_tree_cookie_t ck = xcb_query_tree(xc, root);
	xcb_query_tree_reply_t *tr = xcb_query_tree_reply(xc, ck, NULL);
	if (!tr)
		return;

	int           num  = xcb_query_tree_children_length(tr);
	xcb_window_t *wins = xcb_query_tree_children(tr);

	/* first pass: non-transients */
	for (i = 0; i < (unsigned int) num; i++) {
		xcb_get_window_attributes_cookie_t wck =
		    xcb_get_window_attributes(xc, wins[i]);
		xcb_get_window_attributes_reply_t *wr =
		    xcb_get_window_attributes_reply(xc, wck, NULL);
		if (!wr)
			continue;
		int override  = wr->override_redirect;
		int map_state = wr->map_state;
		free(wr);
		xcb_window_t trans = XCB_WINDOW_NONE;
		if (override ||
		    xcb_icccm_get_wm_transient_for_reply(
		        xc, xcb_icccm_get_wm_transient_for(xc, wins[i]), &trans, NULL))
			continue;
		if (map_state == XCB_MAP_STATE_VIEWABLE ||
		    getstate(wins[i]) == XCB_ICCCM_WM_STATE_ICONIC) {
			xcb_get_geometry_cookie_t gck = xcb_get_geometry(xc, wins[i]);
			xcb_get_geometry_reply_t *gr =
			    xcb_get_geometry_reply(xc, gck, NULL);
			if (gr) {
				manage(wins[i], gr);
				free(gr);
			}
		}
	}
	/* second pass: transients */
	for (i = 0; i < (unsigned int) num; i++) {
		xcb_get_window_attributes_cookie_t wck =
		    xcb_get_window_attributes(xc, wins[i]);
		xcb_get_window_attributes_reply_t *wr =
		    xcb_get_window_attributes_reply(xc, wck, NULL);
		if (!wr)
			continue;
		int map_state = wr->map_state;
		free(wr);
		xcb_window_t trans = XCB_WINDOW_NONE;
		if (xcb_icccm_get_wm_transient_for_reply(xc,
		        xcb_icccm_get_wm_transient_for(xc, wins[i]), &trans, NULL) &&
		    (map_state == XCB_MAP_STATE_VIEWABLE ||
		        getstate(wins[i]) == XCB_ICCCM_WM_STATE_ICONIC)) {
			xcb_get_geometry_cookie_t gck = xcb_get_geometry(xc, wins[i]);
			xcb_get_geometry_reply_t *gr =
			    xcb_get_geometry_reply(xc, gck, NULL);
			if (gr) {
				manage(wins[i], gr);
				free(gr);
			}
		}
	}
	free(tr);
}

/* Batch-intern all atoms using async XCB cookies on the connection that Xlib
 * already owns.  All requests are sent in one go before any reply is read, so
 * the round-trip cost is that of a single request instead of N sequential
 * ones.  The two COMPOSITOR-only atoms are included unconditionally in the
 * table (their slots in netatom[] exist regardless of the build flag); we just
 * always populate them so the table stays flat and branch-free. */
static void
intern_atoms(void)
{
	/* Each entry maps one atom name to the Atom* that should receive it. */
	static const struct {
		xcb_atom_t *dest;
		const char *name;
	} tbl[] = {
		{ &utf8string_atom, "UTF8_STRING" },
		{ &wmatom[WMProtocols], "WM_PROTOCOLS" },
		{ &wmatom[WMDelete], "WM_DELETE_WINDOW" },
		{ &wmatom[WMState], "WM_STATE" },
		{ &wmatom[WMTakeFocus], "WM_TAKE_FOCUS" },
		{ &netatom[NetActiveWindow], "_NET_ACTIVE_WINDOW" },
		{ &netatom[NetSupported], "_NET_SUPPORTED" },
		{ &netatom[NetSystemTray], "_NET_SYSTEM_TRAY_S0" },
		{ &netatom[NetSystemTrayOP], "_NET_SYSTEM_TRAY_OPCODE" },
		{ &netatom[NetSystemTrayOrientation], "_NET_SYSTEM_TRAY_ORIENTATION" },
		{ &netatom[NetSystemTrayOrientationHorz],
		    "_NET_SYSTEM_TRAY_ORIENTATION_HORZ" },
		{ &netatom[NetSystemTrayColors], "_NET_SYSTEM_TRAY_COLORS" },
		{ &netatom[NetSystemTrayVisual], "_NET_SYSTEM_TRAY_VISUAL" },
		{ &netatom[NetWMName], "_NET_WM_NAME" },
		{ &netatom[NetWMIcon], "_NET_WM_ICON" },
		{ &netatom[NetWMState], "_NET_WM_STATE" },
		{ &netatom[NetWMCheck], "_NET_SUPPORTING_WM_CHECK" },
		{ &netatom[NetWMFullscreen], "_NET_WM_STATE_FULLSCREEN" },
		{ &netatom[NetWMStateDemandsAttention],
		    "_NET_WM_STATE_DEMANDS_ATTENTION" },
		{ &netatom[NetWMStateSticky], "_NET_WM_STATE_STICKY" },
		{ &netatom[NetWMStateAbove], "_NET_WM_STATE_ABOVE" },
		{ &netatom[NetWMStateBelow], "_NET_WM_STATE_BELOW" },
		{ &netatom[NetWMStateHidden], "_NET_WM_STATE_HIDDEN" },
		{ &netatom[NetWMWindowType], "_NET_WM_WINDOW_TYPE" },
		{ &netatom[NetWMWindowTypeDialog], "_NET_WM_WINDOW_TYPE_DIALOG" },
		{ &netatom[NetClientList], "_NET_CLIENT_LIST" },
		{ &netatom[NetClientListStacking], "_NET_CLIENT_LIST_STACKING" },
		{ &netatom[NetWMDesktop], "_NET_WM_DESKTOP" },
		{ &netatom[NetWMPid], "_NET_WM_PID" },
		{ &netatom[NetDesktopViewport], "_NET_DESKTOP_VIEWPORT" },
		{ &netatom[NetNumberOfDesktops], "_NET_NUMBER_OF_DESKTOPS" },
		{ &netatom[NetCurrentDesktop], "_NET_CURRENT_DESKTOP" },
		{ &netatom[NetDesktopNames], "_NET_DESKTOP_NAMES" },
		{ &netatom[NetWorkarea], "_NET_WORKAREA" },
		{ &netatom[NetCloseWindow], "_NET_CLOSE_WINDOW" },
		{ &netatom[NetMoveResizeWindow], "_NET_MOVERESIZE_WINDOW" },
		{ &netatom[NetFrameExtents], "_NET_FRAME_EXTENTS" },
		{ &netatom[NetWMWindowOpacity], "_NET_WM_WINDOW_OPACITY" },
		{ &netatom[NetWMBypassCompositor], "_NET_WM_BYPASS_COMPOSITOR" },
		{ &xatom[Manager], "MANAGER" },
		{ &xatom[Xembed], "_XEMBED" },
		{ &xatom[XembedInfo], "_XEMBED_INFO" },
	};
	static const int N = (int) (sizeof tbl / sizeof tbl[0]);

	xcb_intern_atom_cookie_t *cookies;
	xcb_intern_atom_reply_t  *reply;
	int                       i;

	cookies = ecalloc((size_t) N, sizeof *cookies);

	/* Fire all requests — no round-trip yet. */
	for (i = 0; i < N; i++) {
		uint16_t nlen = (uint16_t) strlen(tbl[i].name);
		cookies[i]    = xcb_intern_atom(xc, 0, nlen, tbl[i].name);
	}

	/* Collect replies — one round-trip covers all. */
	for (i = 0; i < N; i++) {
		reply = xcb_intern_atom_reply(xc, cookies[i], NULL);
		if (reply) {
			*tbl[i].dest = reply->atom;
			free(reply);
		}
	}

	free(cookies);
}

void
setup(void)
{
	int              i;
	struct sigaction sa;

	/* do not transform children into zombies when they terminate */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags   = SA_NOCLDSTOP | SA_NOCLDWAIT | SA_RESTART;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);

	/* clean up any zombies (inherited from .xinitrc etc) immediately */
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;

	/* init screen — screen number comes from xcb_connect() second arg */
	{
		xcb_screen_iterator_t sit =
		    xcb_setup_roots_iterator(xcb_get_setup(xc));
		int i;
		for (i = 0; i < screen; i++)
			xcb_screen_next(&sit);
		sw   = (int) sit.data->width_in_pixels;
		sh   = (int) sit.data->height_in_pixels;
		root = sit.data->root;
	}
	if (!(cl = (Clientlist *) calloc(1, sizeof(Clientlist))))
		die("fatal: could not malloc() %u bytes\n", sizeof(Clientlist));
	/* drw uses a dedicated bare xcb_connection_t (opened inside drw_create)
	 * for all cairo rendering, keeping its XCB traffic off xc. */
	drw = drw_create(xc, screen, root, sw, sh);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;
	bh    = drw->fonts->h + 2;
	updategeom();
	/* Enable RandR screen change notifications */
#ifdef XRANDR
	{
		const xcb_query_extension_reply_t *ext =
		    xcb_get_extension_data(xc, &xcb_randr_id);
		if (ext && ext->present) {
			randrbase = ext->first_event;
			rrerrbase = ext->first_error;
			xcb_randr_select_input(
			    xc, root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
		}
	}
#endif
	/* init atoms — all interned in a single async XCB batch */
	intern_atoms();
	/* init key symbols table */
	keysyms = xcb_key_symbols_alloc(xc);
	/* init cursors */
	cursor[CurNormal] = drw_cur_create(drw, 68);  /* XC_left_ptr */
	cursor[CurResize] = drw_cur_create(drw, 120); /* XC_sizing */
	cursor[CurMove]   = drw_cur_create(drw, 52);  /* XC_fleur */
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
	{
		wmcheckwin = xcb_generate_id(xc);
		xcb_create_window(xc, XCB_COPY_FROM_PARENT, wmcheckwin, root, 0, 0, 1,
		    1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0,
		    NULL);
	}
	{
		uint32_t win32 = (uint32_t) wmcheckwin;

		xcb_change_property(xc, XCB_PROP_MODE_REPLACE, wmcheckwin,
		    netatom[NetWMCheck], XCB_ATOM_WINDOW, 32, 1, &win32);
		xcb_change_property(xc, XCB_PROP_MODE_REPLACE, wmcheckwin,
		    netatom[NetWMName], utf8string_atom, 8, 3, "awm");
		xcb_change_property(xc, XCB_PROP_MODE_REPLACE, root,
		    netatom[NetWMCheck], XCB_ATOM_WINDOW, 32, 1, &win32);

		/* EWMH support per view — netatom[] is Atom=unsigned long, need
		 * uint32_t array for XCB format-32 */
		{
			xcb_atom_t supported[NetLast];
			int        k;
			for (k = 0; k < NetLast; k++)
				supported[k] = (xcb_atom_t) netatom[k];
			xcb_change_property(xc, XCB_PROP_MODE_REPLACE, root,
			    netatom[NetSupported], XCB_ATOM_ATOM, 32, NetLast, supported);
		}

		xcb_delete_property(xc, root, netatom[NetClientList]);
	}
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
	/* select events */
	{

		uint32_t evmask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		    XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS |
		    XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_ENTER_WINDOW |
		    XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		    XCB_EVENT_MASK_PROPERTY_CHANGE;
		xcb_change_window_attributes(xc, root, XCB_CW_EVENT_MASK, &evmask);
		uint32_t cur = (uint32_t) cursor[CurNormal]->cursor;
		xcb_change_window_attributes(xc, root, XCB_CW_CURSOR, &cur);
	}
	grabkeys();
	focus(NULL);
	/* Initialize icon subsystem (GTK, cache) unconditionally */
	icon_init();
#ifdef STATUSNOTIFIER
	/* Initialize StatusNotifier support */
	if (!sni_init(xc, drw->cairo_xcb, drw->xcb_visual, root, drw, scheme,
	        sniconsize))
		awm_warn("Failed to initialize StatusNotifier support");
#endif
	/* Initialize launcher */
	launcher =
	    launcher_create(xc, root, scheme, fonts, LENGTH(fonts), termcmd[0]);
#ifdef COMPOSITOR
	if (compositor_init(g_main_context_default()) < 0)
		awm_warn("compositor: init failed, running without compositing");
#endif
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
	if (!setlocale(LC_CTYPE, ""))
		fputs("warning: no locale support\n", stderr);
	xc = xcb_connect(NULL, &screen);
	if (!xc || xcb_connection_has_error(xc))
		die("awm: cannot open X display");
	checkotherwm();
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
#ifdef STATUSNOTIFIER
		/* Release D-Bus name and connection before exec so the new process
		 * image can claim org.kde.StatusNotifierWatcher immediately.
		 * Without this, the libdbus shared connection fd survives across
		 * execvp and the bus still sees the name as owned, causing
		 * sni_init() in the new image to fail with "not primary owner". */
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
		execvp(argv[0], argv);
		awm_error("execvp failed: %s", strerror(errno));
	}
	cleanup();
	log_cleanup();
	xcb_disconnect(xc);
	return EXIT_SUCCESS;
}
