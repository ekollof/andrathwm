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
 * Called from resizeclient() after XConfigureWindow.
 * actual_bw is wc.border_width — the border width actually applied to the X
 * window (may differ from c->bw when singularborders collapses it to 0).
 */
void compositor_configure_window(Client *c, int actual_bw);

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
void compositor_handle_event(xcb_generic_event_t *ev);

/* Force a full-screen repaint — used after xrdb hot-reload. */
void compositor_damage_all(void);

/*
 * Called from focus() and unfocus() to dirty the border region of a client
 * so that the compositor repaints it in the correct focus colour.
 */
void compositor_focus_window(Client *c);

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

/*
 * Fill *req_base and *err_base with the GLX extension major opcode and
 * error base.  Needed by the X error handler to whitelist transient GLX
 * errors (e.g. GLXBadPixmap / GLXBadContextState) that arise when a TFP
 * pixmap is destroyed while rendering is in flight (e.g. on fullscreen).
 * Sets both to -1 if the compositor is inactive or not using GL.
 */
void compositor_glx_errors(int *req_base, int *err_base);

/*
 * Perform a compositor repaint synchronously right now, bypassing the GLib
 * idle scheduler.  Call this from synchronous event loops (movemouse /
 * resizemouse) that block the GLib main loop and prevent idle callbacks
 * from firing.
 */
void compositor_repaint_now(void);

/*
 * Called from showhide() to tell the compositor a client window has been
 * moved off-screen (hidden=1) or back on-screen (hidden=0).  Prevents the
 * compositor from painting stale window content over an empty tag.
 */
void compositor_set_hidden(Client *c, int hidden);

/*
 * Re-evaluate whether compositing should be suspended because a fullscreen
 * opaque window covers the entire monitor.  Call after focus changes and
 * after fullscreen state changes.  When suspended the compositor overlay is
 * lowered so the window renders directly to the display with zero GL
 * overhead; it is raised again automatically on resume.
 */
void compositor_check_unredirect(void);

/*
 * Called from configurenotify() in events.c after sw/sh have been updated
 * following an xrandr screen resize.  Updates the GL viewport, resizes the
 * XRender back-buffer, resets the partial-repaint damage ring (all old
 * bounding boxes are now stale), and forces a full repaint.
 */
void compositor_notify_screen_resize(void);

/*
 * Apply the XESetWireToEvent workaround for a raw X event.
 * Must be called on every event immediately after XNextEvent, before any
 * handler dispatches it.  Prevents GL/DRI2 wire-to-event hooks from
 * corrupting Xlib's sequence tracking when GLX and Xlib share a Display.
 * No-op when the GL compositor path is not active.
 */
void compositor_fix_wire_to_event(XEvent *ev);

#endif /* COMPOSITOR */
#endif /* COMPOSITOR_H */
