#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#ifdef COMPOSITOR

#include <glib.h>

/*
 * compositor.h is included from awm.h *after* the Client typedef/struct,
 * so Client is fully defined here.
 */

/*
 * Initialise the compositor.  Must be called after the X connection is open,
 * all monitors are configured, and the bar windows exist.
 *
 * Returns  0 on success.
 * Returns -1 on non-fatal failure (another compositor already running, or a
 *            required extension is missing) — the WM continues without
 *            compositing in that case.
 */
int compositor_init(GMainContext *ctx);

/* Tear down: free all pictures/pixmaps, release overlay, give back redirect.
 */
void compositor_cleanup(void);

/* Called from manage() after the client window is mapped. */
void compositor_add_window(Client *c);

/* Called from unmanage() before free(c). */
void compositor_remove_window(Client *c);

/*
 * Called from resizeclient() after XConfigureWindow — the window's pixmap
 * must be re-acquired because the old one is now stale.
 */
void compositor_configure_window(Client *c);

/*
 * bypass == 1 : stop redirecting this window (fullscreen / bypass-compositor)
 * bypass == 0 : resume redirecting
 */
void compositor_bypass_window(Client *c, int bypass);

/*
 * Called from propertynotify() when _NET_WM_WINDOW_OPACITY changes.
 * raw is the raw 32-bit property value (0 = fully transparent, 0xFFFFFFFF =
 * opaque).
 */
void compositor_set_opacity(Client *c, unsigned long raw);

/*
 * Feed an X event to the compositor.  Called from x_dispatch_cb() for every
 * event before the normal handler[] dispatch so DamageNotify, MapNotify and
 * ConfigureNotify can be intercepted.
 */
void compositor_handle_event(XEvent *ev);

/* Force a full-screen repaint — used after xrdb hot-reload. */
void compositor_damage_all(void);

/* Raise the compositor overlay above all windows — called after unfullscreen.
 */
void compositor_raise_overlay(void);

/*
 * Fill *req_base and *err_base with the XRender major opcode and error base
 * (needed by the X error handler to whitelist transient XRender errors that
 * arise from GL windows closing while a repaint is in flight).
 * Sets both to -1 if the compositor is not active.
 */
void compositor_xrender_errors(int *req_base, int *err_base);

/*
 * Fill *err_base with the XDamage error base (BadDamage = err_base + 0).
 * Needed by the X error handler to whitelist transient XDamage errors.
 * Sets to -1 if the compositor is not active.
 */
void compositor_damage_errors(int *err_base);

#endif /* COMPOSITOR */
#endif /* COMPOSITOR_H */
