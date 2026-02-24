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
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/memfd.h>

#include <glib-unix.h>
#include <gtk/gtk.h>

#include "awm.h"
#include "client.h"
#include "events.h"
#include "ewmh.h"
#include "icon.h"
#include "monitor.h"
#include "spawn.h"
#include "status.h"
#include "switcher.h"
#include "systray.h"
#include "ui_proto.h"
#include "xrdb.h"
#include "xsource.h"
#define AWM_CONFIG_IMPL
#include "config.h"

/* variables */
Systray     *systray          = NULL;
static pid_t ui_pid           = -1; /* awm-ui child process */
static int   ui_fd            = -1; /* socket fd to awm-ui */
int          launcher_visible = 0;  /* 1 while the launcher window is open */
xcb_window_t launcher_xwin    = 0;  /* X window ID sent by awm-ui on startup */
static GMainContext *ui_ctx = NULL; /* GMainContext used by run() — kept for
                                     * the respawn timer callback */
char         stext[STATUS_TEXT_LEN];
int          screen;
int          sw, sh;             /* X display screen geometry width, height */
int          bh;                 /* bar height */
int          lrpad;              /* sum of left and right padding for text */
double       ui_dpi      = 96.0; /* resolved screen DPI */
double       ui_scale    = 1.0;  /* ui_dpi / 96.0 */
unsigned int ui_borderpx = 1;    /* borderpx * ui_scale — set in setup() */
unsigned int ui_snap     = 32;   /* snap     * ui_scale — set in setup() */
unsigned int ui_iconsize = 16;   /* iconsize * ui_scale — set in setup() */
unsigned int ui_gappx    = 5;    /* gappx[0] * ui_scale — set in setup() */
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
	[XCB_BUTTON_PRESS]      = buttonpress,
	[XCB_CLIENT_MESSAGE]    = clientmessage,
	[XCB_CONFIGURE_REQUEST] = configurerequest,
	[XCB_CONFIGURE_NOTIFY]  = configurenotify,
	[XCB_DESTROY_NOTIFY]    = destroynotify,
	[XCB_ENTER_NOTIFY]      = enternotify,
	[XCB_LEAVE_NOTIFY]      = leavenotify,
	[XCB_EXPOSE]            = expose,
	[XCB_FOCUS_IN]          = focusin,
	[XCB_KEY_PRESS]         = keypress,
	[XCB_KEY_RELEASE]       = keyrelease,
	[XCB_MAPPING_NOTIFY]    = mappingnotify,
	[XCB_MAP_REQUEST]       = maprequest,
	[XCB_MOTION_NOTIFY]     = motionnotify,
	[XCB_PROPERTY_NOTIFY]   = propertynotify,
	[XCB_RESIZE_REQUEST]    = resizerequest,
	[XCB_UNMAP_NOTIFY]      = unmapnotify,
};
xcb_atom_t         wmatom[WMLast], netatom[NetLast], xatom[XLast];
xcb_atom_t         utf8string_atom; /* UTF8_STRING — used in setup() */
int                restart         = 0;
int                barsdirty       = 0;
xcb_timestamp_t    last_event_time = XCB_CURRENT_TIME;
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
	free(cl);
	cl = NULL;

	if (showsystray) {
		Client *ic = systray->icons;
		while (ic) {
			Client *next = ic->next;
			free(ic);
			ic = next;
		}
		if (systray->colormap)
			xcb_free_colormap(xc, systray->colormap);
		xcb_unmap_window(xc, systray->win);
		xcb_destroy_window(xc, systray->win);
		free(systray);
	}
	status_cleanup();
	/* Signal awm-ui to exit and reap it.
	 * setup() sets SA_NOCLDWAIT so children are auto-reaped; just kill and
	 * give the process a moment to exit — no waitpid needed. */
	if (ui_pid > 0) {
		kill(ui_pid, SIGTERM);
		ui_pid = -1;
	}
	if (ui_fd >= 0) {
		close(ui_fd);
		ui_fd = -1;
	}
#ifdef COMPOSITOR
	compositor_cleanup();
#endif
	switcher_cleanup();

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
	gtk_main_quit();
}

/* -------------------------------------------------------------------------
 * IPC helpers — awm → awm-ui
 * ---------------------------------------------------------------------- */

/* Send a fixed-size inline payload to awm-ui.  Returns 0 on success. */
static int
ui_send_inline(UiMsgType type, const void *payload, uint32_t len)
{
	uint8_t buf[sizeof(UiMsgHeader) + UI_MSG_MAX_PAYLOAD];
	ssize_t n;

	if (ui_fd < 0)
		return -1;
	if (len > UI_MSG_MAX_PAYLOAD) {
		awm_error("ui_send_inline: payload too large (%u)", len);
		return -1;
	}
	{
		UiMsgHeader *hdr = (UiMsgHeader *) buf;
		hdr->type        = (uint32_t) type;
		hdr->payload_len = len;
		if (len > 0 && payload)
			memcpy(buf + sizeof(UiMsgHeader), payload, len);
	}
	n = send(ui_fd, buf, sizeof(UiMsgHeader) + len, MSG_NOSIGNAL);
	if (n < 0) {
		awm_error("ui_send_inline: send: %s", strerror(errno));
		return -1;
	}
	return 0;
}

/* Broadcast current selmon workarea geometry to awm-ui so it can position
 * popups correctly.  Called once on startup and again on monitor changes. */
void
ui_send_monitor_geom(void)
{
	UiMonitorGeomPayload p;

	if (!selmon || ui_fd < 0)
		return;
	p.wx = (int32_t) selmon->wx;
	p.wy = (int32_t) selmon->wy;
	p.ww = (int32_t) selmon->ww;
	p.wh = (int32_t) selmon->wh;
	ui_send_inline(UI_MSG_MONITOR_GEOM, &p, sizeof(p));
}

/* Send current color scheme and bar font to awm-ui so notification popups
 * can match the WM visual theme.  Called once on startup and after every
 * xrdb reload. */
void
ui_send_theme(void)
{
	UiThemePayload p;

	if (ui_fd < 0 || !scheme)
		return;

	p.norm_fg[0] = scheme[SchemeNorm][ColFg].r;
	p.norm_fg[1] = scheme[SchemeNorm][ColFg].g;
	p.norm_fg[2] = scheme[SchemeNorm][ColFg].b;
	p.norm_fg[3] = scheme[SchemeNorm][ColFg].a;

	p.norm_bg[0] = scheme[SchemeNorm][ColBg].r;
	p.norm_bg[1] = scheme[SchemeNorm][ColBg].g;
	p.norm_bg[2] = scheme[SchemeNorm][ColBg].b;
	p.norm_bg[3] = scheme[SchemeNorm][ColBg].a;

	p.norm_bd[0] = scheme[SchemeNorm][ColBorder].r;
	p.norm_bd[1] = scheme[SchemeNorm][ColBorder].g;
	p.norm_bd[2] = scheme[SchemeNorm][ColBorder].b;
	p.norm_bd[3] = scheme[SchemeNorm][ColBorder].a;

	p.sel_fg[0] = scheme[SchemeSel][ColFg].r;
	p.sel_fg[1] = scheme[SchemeSel][ColFg].g;
	p.sel_fg[2] = scheme[SchemeSel][ColFg].b;
	p.sel_fg[3] = scheme[SchemeSel][ColFg].a;

	p.sel_bg[0] = scheme[SchemeSel][ColBg].r;
	p.sel_bg[1] = scheme[SchemeSel][ColBg].g;
	p.sel_bg[2] = scheme[SchemeSel][ColBg].b;
	p.sel_bg[3] = scheme[SchemeSel][ColBg].a;

	p.sel_bd[0] = scheme[SchemeSel][ColBorder].r;
	p.sel_bd[1] = scheme[SchemeSel][ColBorder].g;
	p.sel_bd[2] = scheme[SchemeSel][ColBorder].b;
	p.sel_bd[3] = scheme[SchemeSel][ColBorder].a;

	p.font[0] = '\0';
	if (fonts[0])
		snprintf(p.font, sizeof(p.font), "%s", fonts[0]);

	p.dpi = ui_dpi;

	ui_send_inline(UI_MSG_THEME, &p, sizeof(p));
}

/* Send a bulk SHM message to awm-ui.  Creates an anonymous SHM fd, writes
 * shm_size bytes from base into it, and transmits the fd via SCM_RIGHTS.
 * The message header has type=type and payload_len=shm_size.
 * Returns 0 on success, -1 on failure.
 *
 * We prefer memfd_create(2) for the anonymous fd: it is truly nameless,
 * never appears in /dev/shm, and avoids the PID-based shm_open name that
 * could collide if the process is restarted under the same PID.  If
 * memfd_create is not available at runtime we fall back to a shm_open name
 * that includes both the PID and a call-site sequence counter. */
static int
ui_send_shm(UiMsgType type, const void *base, size_t shm_size)
{
	int                 shm_fd = -1;
	void               *mapped = MAP_FAILED;
	int                 ret    = -1;
	static unsigned int seq    = 0;

	/* Prefer memfd_create — anonymous, no name-collision risk */
#ifdef __NR_memfd_create
	shm_fd = memfd_create("awm-preview", MFD_CLOEXEC);
#endif
	if (shm_fd < 0) {
		/* Fallback: shm_open with a name that includes pid + seq counter */
		char name[48];
		snprintf(
		    name, sizeof(name), "/awm-preview-%d-%u", (int) getpid(), seq++);
		shm_fd = shm_open(name, O_CREAT | O_RDWR | O_TRUNC, 0600);
		if (shm_fd < 0) {
			awm_error("ui_send_shm: shm_open: %s", strerror(errno));
			goto out;
		}
		shm_unlink(name); /* unlink immediately; fd keeps it alive */
	}

	if (ftruncate(shm_fd, (off_t) shm_size) < 0) {
		awm_error("ui_send_shm: ftruncate: %s", strerror(errno));
		goto out;
	}

	mapped =
	    mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (mapped == MAP_FAILED) {
		awm_error("ui_send_shm: mmap: %s", strerror(errno));
		goto out;
	}
	memcpy(mapped, base, shm_size);
	munmap(mapped, shm_size);
	mapped = MAP_FAILED;

	/* Build SCM_RIGHTS message */
	{
		UiMsgHeader     hdr;
		struct iovec    iov;
		struct msghdr   mhdr;
		uint8_t         cmsgbuf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr *cm;
		ssize_t         n;

		hdr.type        = (uint32_t) type;
		hdr.payload_len = (uint32_t) shm_size;

		iov.iov_base = &hdr;
		iov.iov_len  = sizeof(hdr);

		memset(&mhdr, 0, sizeof(mhdr));
		mhdr.msg_iov        = &iov;
		mhdr.msg_iovlen     = 1;
		mhdr.msg_control    = cmsgbuf;
		mhdr.msg_controllen = sizeof(cmsgbuf);

		cm             = CMSG_FIRSTHDR(&mhdr);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type  = SCM_RIGHTS;
		cm->cmsg_len   = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &shm_fd, sizeof(int));

		n = sendmsg(ui_fd, &mhdr, MSG_NOSIGNAL);
		if (n < 0) {
			awm_error("ui_send_shm: sendmsg: %s", strerror(errno));
			goto out;
		}
	}
	ret = 0;

out:
	if (shm_fd >= 0)
		close(shm_fd);
	return ret;
}

/* Build and send a UI_MSG_PREVIEW_SHOW message for bar hover on Monitor m.
 * Acquires fresh snapshot pixmaps from the compositor and transmits them
 * to awm-ui via SHM + SCM_RIGHTS. */
void
bar_hover_enter(Monitor *m)
{
#ifdef COMPOSITOR
	UiPreviewEntry      *entries;
	unsigned int         count;
	UiPreviewShowPayload hdr;
	size_t               shm_size;
	uint8_t             *shm_buf;

	if (ui_fd < 0 || !m)
		return;

	entries = comp_snapshot_pixmaps(&count);
	if (!entries || count == 0) {
		free(entries);
		return;
	}

	shm_size = sizeof(UiPreviewShowPayload) + count * sizeof(UiPreviewEntry);
	shm_buf  = malloc(shm_size);
	if (!shm_buf) {
		/* Free any snapshot pixmaps we just acquired */
		{
			unsigned int k;
			for (k = 0; k < count; k++)
				if (entries[k].pixmap_xid)
					xcb_free_pixmap(xc, (xcb_pixmap_t) entries[k].pixmap_xid);
		}
		free(entries);
		return;
	}

	/* anchor_x/y: centre of the hovered bar window */
	hdr.anchor_x = (int32_t) (m->mx + m->ww / 2);
	hdr.anchor_y = (int32_t) (m->by + bh / 2);
	hdr.count    = count;
	memcpy(shm_buf, &hdr, sizeof(hdr));
	memcpy(shm_buf + sizeof(hdr), entries, count * sizeof(UiPreviewEntry));
	free(entries);

	if (ui_send_shm(UI_MSG_PREVIEW_SHOW, shm_buf, shm_size) < 0) {
		/* Send failed — free the snapshot pixmaps ourselves */
		UiPreviewEntry *ep = (UiPreviewEntry *) (shm_buf + sizeof(hdr));
		unsigned int    k;
		for (k = 0; k < count; k++)
			if (ep[k].pixmap_xid)
				xcb_free_pixmap(xc, (xcb_pixmap_t) ep[k].pixmap_xid);
	}
	free(shm_buf);
#else
	(void) m;
#endif
}

/* Send UI_MSG_PREVIEW_HIDE to awm-ui when the pointer leaves the bar. */
void
bar_hover_leave(void)
{
	ui_send_inline(UI_MSG_PREVIEW_HIDE, NULL, 0);
}

/* Keybind handler: trigger the window preview popup via bar_hover_enter. */
void
preview_show_keybind(const Arg *arg)
{
	(void) arg;
	bar_hover_enter(selmon);
}

void
launchermenu(const Arg *arg)
{
	UiMsgHeader           hdr;
	UiLauncherShowPayload p;
	uint8_t buf[sizeof(UiMsgHeader) + sizeof(UiLauncherShowPayload)];
	(void) arg;

	if (ui_fd < 0)
		return;

	Monitor *m = selmon;
	p.x        = (int32_t) (m->wx + (m->ww - 420) / 2);
	p.y        = (int32_t) (m->wy + (m->wh - 400) / 2);
	if (p.x < (int32_t) m->wx)
		p.x = (int32_t) m->wx;
	if (p.y < (int32_t) m->wy)
		p.y = (int32_t) m->wy;

	hdr.type        = (uint32_t) UI_MSG_LAUNCHER_SHOW;
	hdr.payload_len = sizeof(p);
	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), &p, sizeof(p));

	if (send(ui_fd, buf, sizeof(buf), MSG_NOSIGNAL) < 0) {
		awm_error("launchermenu: send: %s", strerror(errno));
	} else {
		launcher_visible = 1;
		/* awm-ui owns X focus for the launcher: launcher_show() calls
		 * gdk_display_sync() then gdk_window_focus() on its own GDK
		 * connection, which guarantees the window is mapped before
		 * XSetInputFocus is sent — no cross-process race. */
	}
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

		/* XCB delivers async errors as packets with response_type == 0 */
		if (ev->response_type == 0) {
			xcb_error_handler((xcb_generic_error_t *) ev);
			free(ev);
			continue;
		}

#ifdef XRANDR
		if (type == (uint8_t) (randrbase + XCB_RANDR_SCREEN_CHANGE_NOTIFY)) {
			/* Update virtual screen dimensions from the event payload before
			 * calling updategeom(), mirroring what configurenotify() does
			 * for the root ConfigureNotify path. */
			{
				xcb_randr_screen_change_notify_event_t *rrev =
				    (xcb_randr_screen_change_notify_event_t *) ev;
				sw = (int) rrev->width;
				sh = (int) rrev->height;
			}
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
			for (Monitor *m = mons; m; m = m->next)
				updateworkarea(m);
#ifdef COMPOSITOR
			compositor_notify_screen_resize();
#endif
			ui_send_monitor_geom();
		} else
#endif
		{
#ifdef COMPOSITOR
			compositor_handle_event(ev);
#endif
			if (type < LASTEvent && handler[type])
				handler[type](ev);
		}
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
	if (dbus_retry_id > 0)
		return; /* already scheduled */

	dbus_retry_id =
	    g_timeout_add_seconds(1, dbus_reconnect_cb, g_main_context_default());
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
		if (dbus_retry_id > 0) {
			g_source_remove(dbus_retry_id);
			dbus_retry_id = 0;
		}
		dbus_retry_id = g_timeout_add_seconds(
		    2, dbus_reconnect_cb, g_main_context_default());
		return G_SOURCE_REMOVE;
	}

	(void) user_data;
	sni_handle_dbus();
	return G_SOURCE_CONTINUE;
}

#endif /* STATUSNOTIFIER */

/* -------------------------------------------------------------------------
 * awm-ui helper process — fork/socket infrastructure
 * ---------------------------------------------------------------------- */

static int ui_spawn(GMainContext *ctx); /* forward declaration */

/* Timer callback: respawn awm-ui after a brief delay */
static gboolean
ui_respawn_cb(gpointer data)
{
	(void) data;
	if (ui_fd >= 0 || ui_pid > 0) {
		/* Already respawned by another path */
		return G_SOURCE_REMOVE;
	}
	awm_info("awm: respawning awm-ui");
	if (ui_spawn(ui_ctx) < 0)
		awm_warn("awm: failed to respawn awm-ui");
	return G_SOURCE_REMOVE;
}

/* GSource for reading messages from awm-ui over ui_fd */
typedef struct {
	GSource source;
	GPollFD pfd;
} UiSource;

static gboolean
ui_source_prepare(GSource *src, gint *timeout)
{
	(void) src;
	*timeout = -1;
	return FALSE;
}

static gboolean
ui_source_check(GSource *src)
{
	UiSource *s = (UiSource *) src;
	return (s->pfd.revents & (G_IO_IN | G_IO_HUP | G_IO_ERR)) != 0;
}

/* Handle a single message received from awm-ui */
static void
ui_handle_message(UiMsgType type, const uint8_t *payload, uint32_t len)
{
	switch (type) {
	case UI_MSG_LAUNCHER_EXEC: {
		/* payload is a NUL-terminated command string */
		launcher_visible = 0;
		if (len == 0)
			break;
		char   cmd[4096];
		size_t cmdlen = len < sizeof(cmd) ? len : sizeof(cmd) - 1;
		memcpy(cmd, payload, cmdlen);
		cmd[cmdlen] = '\0';
		awm_debug("awm: exec from ui: %s", cmd);

		pid_t epid = fork();
		if (epid < 0) {
			awm_error("awm: fork for exec: %s", strerror(errno));
		} else if (epid == 0) {
			setsid();
			execlp("sh", "sh", "-c", cmd, NULL);
			_exit(1);
		}
		break;
	}
	case UI_MSG_LAUNCHER_DISMISSED:
		launcher_visible = 0;
		break;
	case UI_MSG_LAUNCHER_READY: {
		/* awm-ui sends this once after the launcher window is realized,
		 * giving us its X window ID so we can call xcb_set_input_focus
		 * directly on our own connection (avoids the _NET_ACTIVE_WINDOW
		 * redirect race).  Store it for use in launchermenu(). */
		if (len < sizeof(UiLauncherReadyPayload))
			break;
		UiLauncherReadyPayload p;
		memcpy(&p, payload, sizeof(p));
		launcher_xwin = (xcb_window_t) p.xwin;
		awm_debug("awm: launcher ready, xwin=0x%x", (unsigned) launcher_xwin);
		break;
	}
	case UI_MSG_PREVIEW_FOCUS: {
		/* awm-ui reports that the user clicked a preview card. */
		if (len < sizeof(UiPreviewFocusPayload))
			break;
		{
			UiPreviewFocusPayload fp;
			Client               *c;
			memcpy(&fp, payload, sizeof(fp));
			c = wintoclient((xcb_window_t) fp.xwin);
			if (c) {
				if (c->mon != selmon) {
					unfocus(selmon->sel, 0);
					selmon = c->mon;
				}
				if (!ISVISIBLE(c, c->mon)) {
					Arg a = { .ui = c->tags };
					view(&a);
				}
				focus(c);
				xcb_warp_pointer(xc, XCB_WINDOW_NONE, c->win, 0, 0, 0, 0,
				    (int16_t) (c->w / 2), (int16_t) (c->h / 2));
				xcb_flush(xc);
			}
		}
		break;
	}
	case UI_MSG_PREVIEW_DONE: {
		/* awm-ui is finished with the snapshot pixmaps; free them. */
		if (len < sizeof(UiPreviewDonePayload))
			break;
		{
			UiPreviewDonePayload dp;
			uint32_t             k;
			memcpy(&dp, payload, sizeof(dp));
			for (k = 0; k < dp.count && k < 32; k++) {
				if (dp.xids[k])
					xcb_free_pixmap(xc, (xcb_pixmap_t) dp.xids[k]);
			}
			xcb_flush(xc);
		}
		break;
	}
	default:
		awm_warn("awm: unknown message from awm-ui: type=%u", (unsigned) type);
		break;
	}
}

static gboolean
ui_source_dispatch(GSource *src, GSourceFunc cb, gpointer data)
{
	(void) cb;
	(void) data;
	UiSource *s = (UiSource *) src;

	if (s->pfd.revents & (G_IO_HUP | G_IO_ERR)) {
		awm_warn("awm: awm-ui socket closed — scheduling respawn");
		close(ui_fd);
		ui_fd            = -1;
		ui_pid           = -1;
		launcher_xwin    = 0;
		launcher_visible = 0;
		g_timeout_add(2000, ui_respawn_cb, NULL);
		return G_SOURCE_REMOVE;
	}

	uint8_t buf[sizeof(UiMsgHeader) + UI_MSG_MAX_PAYLOAD];
	ssize_t n = recv(ui_fd, buf, sizeof(buf), 0);
	if (n <= 0) {
		awm_warn("awm: awm-ui recv: %s", n == 0 ? "EOF" : strerror(errno));
		close(ui_fd);
		ui_fd            = -1;
		ui_pid           = -1;
		launcher_xwin    = 0;
		launcher_visible = 0;
		g_timeout_add(2000, ui_respawn_cb, NULL);
		return G_SOURCE_REMOVE;
	}
	if ((size_t) n < sizeof(UiMsgHeader))
		return G_SOURCE_CONTINUE;

	UiMsgHeader hdr;
	memcpy(&hdr, buf, sizeof(hdr));
	if (hdr.payload_len > UI_MSG_MAX_PAYLOAD)
		return G_SOURCE_CONTINUE;
	if ((size_t) n < sizeof(UiMsgHeader) + hdr.payload_len)
		return G_SOURCE_CONTINUE;

	ui_handle_message(
	    (UiMsgType) hdr.type, buf + sizeof(UiMsgHeader), hdr.payload_len);
	return G_SOURCE_CONTINUE;
}

static GSourceFuncs ui_source_funcs = {
	ui_source_prepare,
	ui_source_check,
	ui_source_dispatch,
	NULL,
	NULL,
	NULL,
};

/* Spawn awm-ui.  Creates a socketpair, forks, passes the child fd via
 * argv[1].  Returns 0 on success, -1 on failure. */
static int
ui_spawn(GMainContext *ctx)
{
	int fds[2];

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) < 0) {
		awm_error("ui_spawn: socketpair: %s", strerror(errno));
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		awm_error("ui_spawn: fork: %s", strerror(errno));
		close(fds[0]);
		close(fds[1]);
		return -1;
	}

	if (pid == 0) {
		/* child — close the parent end */
		close(fds[0]);

		/* Close the XCB connection fd so awm's X connection doesn't
		 * leak into awm-ui.  awm-ui uses GDK's own X connection. */
		if (xc)
			close(xcb_get_file_descriptor(xc));

		/* Force GDK to use the X11 backend.  If awm is running inside
		 * a Wayland session WAYLAND_DISPLAY is set in the environment
		 * and GTK 3 would otherwise prefer the Wayland backend, causing
		 * awm-ui to connect to the outer compositor rather than to the
		 * X display we are managing (e.g. Xephyr).  Clearing
		 * WAYLAND_DISPLAY and setting GDK_BACKEND=x11 ensures awm-ui
		 * always opens on the same X display as awm. */
		unsetenv("WAYLAND_DISPLAY");
		setenv("GDK_BACKEND", "x11", 1);

		/* Tell GTK the real screen DPI so that GtkLabel, GtkSearchEntry
		 * and all other Pango-backed widgets render text at the correct
		 * physical size.  GDK_DPI_SCALE is a float multiplier applied on
		 * top of the DPI GDK detects at gtk_init() time; setting it to
		 * ui_dpi/96.0 normalises whatever GDK would have picked to the
		 * DPI we resolved ourselves via Xft.dpi / RandR. */
		{
			char dpi_scale_str[32];
			snprintf(
			    dpi_scale_str, sizeof(dpi_scale_str), "%.6g", ui_dpi / 96.0);
			setenv("GDK_DPI_SCALE", dpi_scale_str, 1);
		}

		/* Build argv: awm-ui <child_fd> */
		char fd_str[32];
		snprintf(fd_str, sizeof(fd_str), "%d", fds[1]);

		/* Look for awm-ui in PATH */
		execlp("awm-ui", "awm-ui", fd_str, NULL);

		awm_error("ui_spawn: exec awm-ui failed: %s", strerror(errno));
		_exit(1);
	}

	/* parent */
	close(fds[1]);
	ui_fd  = fds[0];
	ui_pid = pid;

	/* Mark the parent end close-on-exec so it is automatically closed when
	 * awm restarts via execvp().  Without this the fd survives into the new
	 * image and the old awm-ui stays alive with a dangling socket. */
	{
		int flags = fcntl(ui_fd, F_GETFD);
		if (flags >= 0)
			fcntl(ui_fd, F_SETFD, flags | FD_CLOEXEC);
	}

	/* Register the parent end with GLib */
	UiSource *src =
	    (UiSource *) g_source_new(&ui_source_funcs, sizeof(UiSource));
	src->pfd.fd      = ui_fd;
	src->pfd.events  = G_IO_IN | G_IO_HUP | G_IO_ERR;
	src->pfd.revents = 0;
	g_source_add_poll((GSource *) src, &src->pfd);
	g_source_attach((GSource *) src, ctx);
	g_source_unref((GSource *) src);

	awm_debug("awm: awm-ui spawned (pid=%d, fd=%d)", (int) ui_pid, ui_fd);

	/* Inform awm-ui of the initial monitor geometry so it can position
	 * popups before any geometry-change events arrive. */
	ui_send_monitor_geom();
	/* Send initial color scheme and font so popups match the WM theme. */
	ui_send_theme();
	return 0;
}

void
run(void)
{
	GMainContext *ctx;

	xflush();

	ctx    = g_main_context_default();
	ui_ctx = ctx; /* store for respawn timer callback */

	/* X11 source — wakes the loop whenever X events are pending */
	xsource_id = xsource_attach(xc, ctx, x_dispatch_cb, NULL);

#ifdef STATUSNOTIFIER
	/* D-Bus source — use helper so reconnect can re-attach cleanly */
	sni_attach_dbus_source(ctx);
#endif

	/* Spawn awm-ui helper (launcher + SNI menus).  Non-fatal if absent. */
	if (ui_spawn(ctx) < 0)
		awm_warn(
		    "awm: failed to spawn awm-ui — launcher and SNI menus disabled");

	/* Let xsource_dispatch quit cleanly on X server death
	 * instead of calling exit(1), so cleanup() can run. */
	xsource_use_gtk_main_quit();
	gtk_main();
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
			/* Skip XEMBED clients (systray icons reparented back to
			 * root when the systray container was destroyed on restart) */
			xcb_get_property_reply_t *xer       = xcb_get_property_reply(xc,
			          xcb_get_property(xc, 0, wins[i],
			              (xcb_atom_t) xatom[XembedInfo], XCB_ATOM_ANY, 0, 2),
			          NULL);
			int                       is_xembed = xer && xer->length > 0;
			free(xer);
			if (is_xembed)
				continue;
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
			/* Skip XEMBED clients here too */
			xcb_get_property_reply_t *xer       = xcb_get_property_reply(xc,
			          xcb_get_property(xc, 0, wins[i],
			              (xcb_atom_t) xatom[XembedInfo], XCB_ATOM_ANY, 0, 2),
			          NULL);
			int                       is_xembed = xer && xer->length > 0;
			free(xer);
			if (is_xembed)
				continue;
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
		{ &netatom[NetWMWindowTypeDock], "_NET_WM_WINDOW_TYPE_DOCK" },
		{ &netatom[NetWMWindowTypeToolbar], "_NET_WM_WINDOW_TYPE_TOOLBAR" },
		{ &netatom[NetWMWindowTypeUtility], "_NET_WM_WINDOW_TYPE_UTILITY" },
		{ &netatom[NetWMWindowTypeSplash], "_NET_WM_WINDOW_TYPE_SPLASH" },
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
	/* Scale pixel geometry constants by the resolved DPI factor */
	ui_borderpx = (unsigned int) (borderpx * ui_scale + 0.5);
	ui_snap     = (unsigned int) (snap * ui_scale + 0.5);
	ui_iconsize = (unsigned int) (iconsize * ui_scale + 0.5);
	ui_gappx    = (unsigned int) (gappx[0] * ui_scale + 0.5);
	if (ui_borderpx < 1 && borderpx > 0)
		ui_borderpx = 1;
	if (ui_gappx < 1 && gappx[0] > 0)
		ui_gappx = 1;
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
	if (!sni_init(xc, xc, drw->xcb_visual, root, drw, scheme,
	        (unsigned int) (sniconsize * ui_scale + 0.5)))
		awm_warn("Failed to initialize StatusNotifier support");
#endif
#ifdef COMPOSITOR
	if (compositor_init(g_main_context_default()) < 0)
		awm_warn("compositor: init failed, running without compositing");
#endif
	switcher_init();
}

/* -------------------------------------------------------------------------
 * DPI resolution
 * Three-level chain (evaluated in order):
 *   1. Xft.dpi from RESOURCE_MANAGER  (user-authoritative)
 *   2. RandR output physical size      (EDID fallback)
 *   3. 96.0                            (safe default)
 * ---------------------------------------------------------------------- */

#ifdef XRANDR
/*
 * Query the physical size of the first active RandR output and derive DPI.
 * Returns the calculated DPI, or 0.0 if unavailable.
 */
static double
randr_probe_dpi(xcb_connection_t *conn, int scr_num)
{
	xcb_screen_iterator_t                           sit;
	xcb_window_t                                    root_win;
	xcb_randr_get_screen_resources_current_cookie_t src;
	xcb_randr_get_screen_resources_current_reply_t *sr;
	xcb_randr_crtc_t                               *crtcs;
	int                                             i;
	double                                          dpi = 0.0;

	sit = xcb_setup_roots_iterator(xcb_get_setup(conn));
	for (i = 0; i < scr_num; i++)
		xcb_screen_next(&sit);
	root_win = sit.data->root;

	src = xcb_randr_get_screen_resources_current(conn, root_win);
	sr  = xcb_randr_get_screen_resources_current_reply(conn, src, NULL);
	if (!sr)
		return 0.0;

	crtcs = xcb_randr_get_screen_resources_current_crtcs(sr);

	for (i = 0; i < (int) sr->num_crtcs && dpi == 0.0; i++) {
		xcb_randr_get_crtc_info_cookie_t cic;
		xcb_randr_get_crtc_info_reply_t *ci;
		xcb_randr_output_t              *outputs;

		cic = xcb_randr_get_crtc_info(conn, crtcs[i], XCB_CURRENT_TIME);
		ci  = xcb_randr_get_crtc_info_reply(conn, cic, NULL);
		if (!ci || ci->num_outputs == 0 || ci->width == 0) {
			free(ci);
			continue;
		}

		outputs = xcb_randr_get_crtc_info_outputs(ci);
		{
			xcb_randr_get_output_info_cookie_t oic;
			xcb_randr_get_output_info_reply_t *oi;

			oic =
			    xcb_randr_get_output_info(conn, outputs[0], XCB_CURRENT_TIME);
			oi = xcb_randr_get_output_info_reply(conn, oic, NULL);
			if (oi && oi->mm_width > 0) {
				/* pixels-per-inch from horizontal axis */
				dpi = (double) ci->width / ((double) oi->mm_width / 25.4);
			}
			free(oi);
		}
		free(ci);
	}

	free(sr);
	return dpi;
}
#endif /* XRANDR */

/*
 * Resolve ui_dpi and ui_scale using the three-level chain.
 * Must be called after loadxrdb() and after xc/screen are valid.
 */
static void
resolve_dpi(void)
{
	double d = 0.0;

	/* Level 1: Xft.dpi from RESOURCE_MANAGER */
	if (xrdb_dpi > 0.0) {
		d = xrdb_dpi;
		awm_info("DPI: using Xft.dpi = %.0f from RESOURCE_MANAGER", d);
	}

#ifdef XRANDR
	/* Level 2: RandR physical size */
	if (d <= 0.0) {
		d = randr_probe_dpi(xc, screen);
		if (d > 0.0)
			awm_info("DPI: using RandR physical size = %.1f", d);
	}
#endif /* XRANDR */

	/* Level 3: safe default */
	if (d <= 0.0) {
		d = 96.0;
		awm_info("DPI: falling back to 96");
	}

	ui_dpi   = d;
	ui_scale = d / 96.0;
}

int
main(int argc, char *argv[])
{
	int no_autostart = 0;

	/* Initialize logging subsystem */
	log_init("awm");

	if (argc == 2 && !strcmp("-v", argv[1]))
		die("awm-" VERSION);
	else if (argc == 2 && !strcmp("-s", argv[1]))
		no_autostart = 1;
	else if (argc != 1)
		die("usage: awm [-v] [-s]");
	if (!setlocale(LC_CTYPE, ""))
		fputs("warning: no locale support\n", stderr);
	xc = xcb_connect(NULL, &screen);
	if (!xc || xcb_connection_has_error(xc))
		die("awm: cannot open X display");
	/* Initialise GTK on the same X display.  GTK uses the DISPLAY env var
	 * which must already be set (awm opens xc successfully just above).
	 * We force GDK_BACKEND=x11 so GTK doesn't pick Wayland when run inside
	 * a nested session. */
	unsetenv("WAYLAND_DISPLAY");
	setenv("GDK_BACKEND", "x11", 1);
	gtk_init(&argc, &argv);
	checkotherwm();
	loadxrdb();
	resolve_dpi();
	setup();
#ifdef __OpenBSD__
	if (pledge("stdio rpath proc exec unix inet", NULL) == -1)
		die("pledge");
#endif /* __OpenBSD__ */
	scan();
	if (!no_autostart && !restart && !getenv("RESTARTED"))
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
		/* Terminate awm-ui before exec so the new awm image spawns a fresh
		 * one.  cleanup() is skipped on restart so we must do this here.
		 * SA_NOCLDWAIT means no waitpid needed. */
		if (ui_pid > 0) {
			kill(ui_pid, SIGTERM);
			ui_pid = -1;
		}
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
