/* AndrathWM - X11 GSource integration
 * See LICENSE file for copyright and license details.
 */

#include "xsource.h"

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <glib.h>

/* GMainLoop to quit on X server death; set via xsource_set_quit_loop(). */
static GMainLoop *xsource_quit_loop = NULL;

/* Internal structure — GSource must be the first member so that the
 * GLib vtable functions can cast between GSource* and XSource*. */
struct XSource {
	GSource  source;
	Display *dpy;
	GPollFD  pollfd;
};

/* -------------------------------------------------------------------------
 * GSource vtable
 * -------------------------------------------------------------------------
 * prepare:  check whether the source is immediately ready.
 *           Returns TRUE (and sets *timeout to 0) if X events are already
 *           buffered in the Xlib queue so we don't block in poll().
 */
static gboolean
xsource_prepare(GSource *src, gint *timeout)
{
	XSource *xs = (XSource *) src;

	*timeout = -1; /* block indefinitely unless we have pending events */

	if (XPending(xs->dpy)) {
		*timeout = 0;
		return TRUE;
	}
	return FALSE;
}

/*
 * check:  called after poll() returns; decide whether to dispatch.
 *         We dispatch if either the socket became readable, Xlib has
 *         already buffered events, or the connection has been lost
 *         (HUP/ERR), so we can handle X server death promptly.
 */
static gboolean
xsource_check(GSource *src)
{
	XSource *xs = (XSource *) src;

	return (xs->pollfd.revents & (G_IO_IN | G_IO_HUP | G_IO_ERR)) ||
	    XPending(xs->dpy);
}

/*
 * dispatch:  invoke the user callback.
 *            If the X connection has been lost (HUP or ERR), exit
 *            immediately — the Display is non-recoverable and continuing
 *            would deadlock inside Xlib.
 *            The callback receives the user_data pointer; returning
 *            G_SOURCE_REMOVE unregisters the source, G_SOURCE_CONTINUE
 *            keeps it alive (normal case).
 */
static gboolean
xsource_dispatch(GSource *src, GSourceFunc callback, gpointer user_data)
{
	XSource *xs = (XSource *) src;

	if (xs->pollfd.revents & (G_IO_HUP | G_IO_ERR)) {
		if (xsource_quit_loop) {
			/* Quit cleanly so cleanup() and XCloseDisplay() can run. */
			g_main_loop_quit(xsource_quit_loop);
			return G_SOURCE_REMOVE;
		}
		exit(1);
	}

	if (!callback)
		return G_SOURCE_REMOVE;

	return callback(user_data);
}

static GSourceFuncs xsource_funcs = {
	xsource_prepare, xsource_check, xsource_dispatch, NULL, /* finalize */
	NULL, /* closure_callback */
	NULL, /* closure_marshal */
};

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

GSource *
xsource_new(Display *dpy, GSourceFunc callback, gpointer user_data)
{
	GSource *src;
	XSource *xs;

	src = g_source_new(&xsource_funcs, sizeof(XSource));
	xs  = (XSource *) src;

	xs->dpy            = dpy;
	xs->pollfd.fd      = xcb_get_file_descriptor(XGetXCBConnection(dpy));
	xs->pollfd.events  = G_IO_IN | G_IO_HUP | G_IO_ERR;
	xs->pollfd.revents = 0;

	g_source_add_poll(src, &xs->pollfd);
	g_source_set_callback(src, callback, user_data, NULL);

	return src;
}

guint
xsource_attach(
    Display *dpy, GMainContext *ctx, GSourceFunc callback, gpointer user_data)
{
	GSource *src;
	guint    id;

	src = xsource_new(dpy, callback, user_data);
	id  = g_source_attach(src, ctx);
	g_source_unref(src);

	return id;
}

void
xsource_set_quit_loop(GMainLoop *loop)
{
	xsource_quit_loop = loop;
}
