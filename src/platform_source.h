/* AndrathWM - X11 GSource integration
 * See LICENSE file for copyright and license details.
 *
 * Provides a GSource subclass that integrates an XCB connection into a GLib
 * main loop.  The source becomes ready when the underlying socket becomes
 * readable (X events pending) or the connection is lost.
 */

#ifndef PLATFORM_SOURCE_H
#define PLATFORM_SOURCE_H

#include <xcb/xcb.h>
#include <glib.h>

/* Opaque GSource wrapping an XCB connection. */
typedef struct XSource XSource;

/*
 * platform_source_attach - create, attach to @ctx, and release the source.
 *
 * Convenience wrapper around g_source_new() + g_source_attach() +
 * g_source_unref().  Returns the source ID (as g_source_attach() does).
 */
guint platform_source_attach(xcb_connection_t *xc, GMainContext *ctx,
    GSourceFunc callback, gpointer user_data);

/*
 * platform_source_use_gtk_main_quit - call gtk_main_quit() on X server death.
 *
 * When the X connection is lost (HUP/ERR on the fd), the dispatch function
 * will call gtk_main_quit() instead of exit(1), allowing the WM's normal
 * cleanup path to run.  Must be called after gtk_init().
 */
void platform_source_use_gtk_main_quit(void);

#endif /* PLATFORM_SOURCE_H */
