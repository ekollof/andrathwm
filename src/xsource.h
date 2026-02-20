/* AndrathWM - X11 GSource integration
 * See LICENSE file for copyright and license details.
 *
 * Provides a GSource subclass that integrates an XCB connection into a GLib
 * main loop.  The source becomes ready when the underlying socket becomes
 * readable (X events pending) or the connection is lost.
 */

#ifndef XSOURCE_H
#define XSOURCE_H

#include <xcb/xcb.h>
#include <glib.h>

/* Opaque GSource wrapping an XCB connection. */
typedef struct XSource XSource;

/*
 * xsource_new - create a GSource that watches an XCB connection.
 *
 * @xc:       the XCB connection to watch
 * @callback: called each time the source dispatches; receives @user_data
 * @user_data: passed verbatim to @callback
 *
 * Returns a floating GSource reference (caller must g_source_attach() then
 * g_source_unref() it, or use xsource_attach() as a convenience).
 */
GSource *xsource_new(
    xcb_connection_t *xc, GSourceFunc callback, gpointer user_data);

/*
 * xsource_attach - create, attach to @ctx, and release the source.
 *
 * Convenience wrapper around xsource_new() + g_source_attach() +
 * g_source_unref().  Returns the source ID (as g_source_attach() does).
 */
guint xsource_attach(xcb_connection_t *xc, GMainContext *ctx,
    GSourceFunc callback, gpointer user_data);

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
