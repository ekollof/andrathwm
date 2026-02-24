#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#ifdef COMPOSITOR

#include <glib.h>
#include <cairo/cairo.h>
#include "ui_proto.h"

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
 * Schedule a deferred fullscreen bypass for client c.  Defers the
 * compositor_bypass_window(c,1) + compositor_check_unredirect() sequence by
 * one GLib main-loop iteration so the client (e.g. st) has time to process
 * the ConfigureNotify from resizeclient() and fully repaint while still
 * redirected.  The compositor paints one clean full-screen frame first, then
 * the bypass fires.
 */
void compositor_defer_fullscreen_bypass(Client *c);

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
void compositor_damage_errors(int *req_base, int *err_base);

/*
 * Always sets *req_base and *err_base to -1.
 * The EGL backend has no X extension opcodes (unlike GLX/TFP), so there are
 * no GLX error codes to whitelist.  This stub exists to avoid changing the
 * X error handler in events.c; callers that see -1 skip the whitelist check.
 */
void compositor_glx_errors(int *req_base, int *err_base);

/*
 * Fill *req_base with the X Present major opcode.
 * Needed by the X error handler to whitelist transient BadIDChoice errors
 * that arise when a Present event subscription EID is used after
 * comp_free_win() destroyed it (stale Present events racing the unsubscribe).
 * Sets to 0 if the compositor is not yet initialised or Present is absent.
 */
void compositor_present_errors(int *req_base);

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
 * Render the live GL texture for client c into a new cairo image surface
 * scaled to at most (max_w x max_h), preserving aspect ratio.
 *
 * Uses the EGL FBO path: binds cw->texture into a framebuffer object,
 * draws a scaled quad, reads pixels back via glReadPixels, and returns a
 * CAIRO_FORMAT_RGB24 image surface containing the result.
 *
 * Returns NULL if the compositor is not active, the EGL backend is not in
 * use, the client has no compositor window, or any GL operation fails.
 * The caller owns the returned surface and must cairo_surface_destroy() it.
 */
cairo_surface_t *comp_capture_thumb(Client *c, int max_w, int max_h);

/*
 * Acquire snapshot pixmaps for all visible managed windows on selmon.
 * Returns a malloc'd array of *count_out UiPreviewEntry structs on success;
 * each entry with pixmap_xid != 0 owns that pixmap and the caller must
 * return them via UI_MSG_PREVIEW_DONE so awm can call xcb_free_pixmap().
 * Returns NULL if the compositor is inactive or there are no visible clients.
 * The caller must free() the returned array.
 */
UiPreviewEntry *comp_snapshot_pixmaps(unsigned int *count_out);

#endif /* COMPOSITOR */
#endif /* COMPOSITOR_H */
