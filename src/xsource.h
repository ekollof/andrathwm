/* AndrathWM - X11 GSource integration
 * See LICENSE file for copyright and license details.
 *
 * Provides a GSource subclass that integrates an Xlib Display connection
 * into a GLib main loop.  The source becomes ready when X events are
 * buffered (XPending > 0) or when the underlying socket becomes readable.
 */

#ifndef XSOURCE_H
#define XSOURCE_H

#include <X11/Xlib.h>
#include <glib.h>

/* Opaque GSource wrapping an Xlib Display connection. */
typedef struct XSource XSource;

/*
 * xsource_new - create a GSource that watches a Display connection.
 *
 * @dpy:      the Xlib Display to watch
 * @callback: called each time the source dispatches; receives @user_data
 * @user_data: passed verbatim to @callback
 *
 * Returns a floating GSource reference (caller must g_source_attach() then
 * g_source_unref() it, or use xsource_attach() as a convenience).
 */
GSource *xsource_new(Display *dpy, GSourceFunc callback, gpointer user_data);

/*
 * xsource_attach - create, attach to @ctx, and release the source.
 *
 * Convenience wrapper around xsource_new() + g_source_attach() +
 * g_source_unref().  Returns the source ID (as g_source_attach() does).
 */
guint xsource_attach(
    Display *dpy, GMainContext *ctx, GSourceFunc callback, gpointer user_data);

/*
 * xsource_set_quit_loop - register the GMainLoop to quit on X server death.
 *
 * When the X connection is lost (HUP/ERR on the fd), the dispatch function
 * will call g_main_loop_quit(@loop) instead of exit(1), allowing the WM's
 * normal cleanup path to run.  Must be called after g_main_loop_new().
 * Pass NULL to revert to the exit(1) fallback.
 */
void xsource_set_quit_loop(GMainLoop *loop);

#endif /* XSOURCE_H */
