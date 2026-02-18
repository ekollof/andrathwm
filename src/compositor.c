/* compositor.c — built-in XRender compositor for awm
 *
 * Architecture:
 *   - XCompositeRedirectSubwindows(root, CompositeRedirectManual) captures
 *     all root children into server-side pixmaps.
 *   - An overlay window (XCompositeGetOverlayWindow) sits above everything
 *     and is the sole painting target.
 *   - XDamage tracks which windows have changed since the last repaint.
 *   - On damage, a single GLib G_PRIORITY_DEFAULT_IDLE source is scheduled.
 *     It paints all windows bottom-to-top onto a back-buffer Picture, then
 *     blits the back-buffer onto the overlay with XRender.
 *   - Double-buffering (back → overlay) eliminates tearing.
 *   - 256 pre-built 1×1 solid alpha Pictures are used as opacity masks to
 *     avoid allocating a new Picture per window per frame.
 *
 * Compile-time guard: the entire file is dead code unless -DCOMPOSITOR.
 */

#ifdef COMPOSITOR

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include <glib.h>

#include "awm.h"
#include "log.h"
#include "compositor.h"

/* -------------------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------------- */

typedef struct CompWin {
	Window          win;
	Client         *client;  /* NULL for override_redirect windows        */
	Pixmap          pixmap;  /* XCompositeNameWindowPixmap result          */
	Picture         picture; /* XRenderCreatePicture on pixmap             */
	Damage          damage;
	int             x, y, w, h, bw; /* last known geometry                 */
	int             depth;   /* window depth                               */
	int             argb;    /* depth == 32                                */
	double          opacity; /* 0.0 – 1.0                                  */
	int             redirected; /* 0 = bypass (fullscreen/bypass-compositor)*/
	struct CompWin *next;
} CompWin;

/* -------------------------------------------------------------------------
 * Module state (all static, no global pollution)
 * ---------------------------------------------------------------------- */

static struct {
	int           active;
	Window        overlay;
	Picture       target; /* XRenderPicture on overlay           */
	Pixmap        back_pixmap;
	Picture       back;            /* XRenderPicture on back_pixmap       */
	Picture       alpha_pict[256]; /* pre-built 1×1 RepeatNormal solids   */
	int           damage_ev_base;
	int           damage_err_base;
	int           xfixes_ev_base;
	int           xfixes_err_base;
	guint         repaint_id; /* GLib idle source id, 0 = none       */
	XserverRegion dirty;      /* accumulated dirty region            */
	CompWin      *windows;
	GMainContext *ctx;
} comp;

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */

static void     comp_add_by_xid(Window w);
static CompWin *comp_find_by_xid(Window w);
static CompWin *comp_find_by_client(Client *c);
static void     comp_free_win(CompWin *cw);
static void     comp_refresh_pixmap(CompWin *cw);
static void     schedule_repaint(void);
static gboolean comp_repaint_idle(gpointer data);
static Picture  make_alpha_picture(double a);

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Suppress X errors for a single call — needed when windows disappear between
 * XQueryTree and our processing of the list. */
static int
comp_xerror_ignore(Display *d, XErrorEvent *e)
{
	(void) d;
	(void) e;
	return 0;
}

static int (*prev_xerror)(Display *, XErrorEvent *) = NULL;

static void
xerror_push_ignore(void)
{
	prev_xerror = XSetErrorHandler(comp_xerror_ignore);
}

static void
xerror_pop(void)
{
	XSync(dpy, False);
	XSetErrorHandler(prev_xerror);
	prev_xerror = NULL;
}

/* -------------------------------------------------------------------------
 * Alpha picture cache
 * ---------------------------------------------------------------------- */

static Picture
make_alpha_picture(double a)
{
	XRenderPictFormat       *fmt;
	XRenderPictureAttributes pa;
	Pixmap                   pix;
	Picture                  pic;
	XRenderColor             col;

	fmt       = XRenderFindStandardFormat(dpy, PictStandardA8);
	pix       = XCreatePixmap(dpy, root, 1, 1, 8);
	pa.repeat = RepeatNormal;
	pic       = XRenderCreatePicture(dpy, pix, fmt, CPRepeat, &pa);
	col.alpha = (unsigned short) (a * 0xffff);
	col.red = col.green = col.blue = 0;
	XRenderFillRectangle(dpy, PictOpSrc, pic, &col, 0, 0, 1, 1);
	XFreePixmap(dpy, pix);
	return pic;
}

/* -------------------------------------------------------------------------
 * compositor_init
 * ---------------------------------------------------------------------- */

int
compositor_init(GMainContext *ctx)
{
	int                      comp_ev, comp_err, render_ev, render_err;
	int                      xfixes_major = 2, xfixes_minor = 0;
	int                      damage_major = 1, damage_minor = 1;
	int                      i;
	XRenderPictFormat       *fmt;
	XRenderPictureAttributes pa;
	XserverRegion            empty;

	memset(&comp, 0, sizeof(comp));
	comp.ctx = ctx;

	/* --- Check required extensions ----------------------------------------
	 */

	if (!XCompositeQueryExtension(dpy, &comp_ev, &comp_err)) {
		awm_warn("compositor: XComposite extension not available");
		return -1;
	}
	{
		int major = 0, minor = 2;
		XCompositeQueryVersion(dpy, &major, &minor);
		if (major < 0 || (major == 0 && minor < 2)) {
			awm_warn("compositor: XComposite >= 0.2 required (got %d.%d)",
			    major, minor);
			return -1;
		}
	}

	if (!XDamageQueryExtension(
	        dpy, &comp.damage_ev_base, &comp.damage_err_base)) {
		awm_warn("compositor: XDamage extension not available");
		return -1;
	}
	XDamageQueryVersion(dpy, &damage_major, &damage_minor);

	if (!XFixesQueryExtension(
	        dpy, &comp.xfixes_ev_base, &comp.xfixes_err_base)) {
		awm_warn("compositor: XFixes extension not available");
		return -1;
	}
	XFixesQueryVersion(dpy, &xfixes_major, &xfixes_minor);

	if (!XRenderQueryExtension(dpy, &render_ev, &render_err)) {
		awm_warn("compositor: XRender extension not available");
		return -1;
	}

	/* --- Redirect all root children ---------------------------------------
	 */

	xerror_push_ignore();
	XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);
	XSync(dpy, False);
	xerror_pop();

	/* If another compositor is running, XCompositeRedirectSubwindows raises
	 * BadAccess.  We treat any error here as "already composited, back off."
	 */

	/* --- Overlay window ---------------------------------------------------
	 */

	comp.overlay = XCompositeGetOverlayWindow(dpy, root);
	if (!comp.overlay) {
		awm_warn("compositor: failed to get overlay window");
		XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
		return -1;
	}

	/* Make the overlay click-through by giving it an empty input shape */
	empty = XFixesCreateRegion(dpy, NULL, 0);
	XFixesSetWindowShapeRegion(dpy, comp.overlay, ShapeInput, 0, 0, empty);
	XFixesDestroyRegion(dpy, empty);

	/* --- Create target picture on overlay ---------------------------------
	 */

	fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
	pa.subwindow_mode = IncludeInferiors;
	comp.target =
	    XRenderCreatePicture(dpy, comp.overlay, fmt, CPSubwindowMode, &pa);

	/* --- Create back-buffer pixmap + picture ------------------------------
	 */

	comp.back_pixmap = XCreatePixmap(dpy, root, (unsigned int) sw,
	    (unsigned int) sh, (unsigned int) DefaultDepth(dpy, screen));
	comp.back =
	    XRenderCreatePicture(dpy, comp.back_pixmap, fmt, CPSubwindowMode, &pa);

	/* --- Pre-build alpha pictures -----------------------------------------
	 */

	for (i = 0; i < 256; i++)
		comp.alpha_pict[i] = make_alpha_picture((double) i / 255.0);

	/* --- Dirty region (starts as full screen) -----------------------------
	 */

	{
		XRectangle full = { 0, 0, (unsigned short) sw, (unsigned short) sh };
		comp.dirty      = XFixesCreateRegion(dpy, &full, 1);
	}

	/* --- Scan existing windows --------------------------------------------
	 */

	{
		Window       root_ret, parent_ret;
		Window      *children  = NULL;
		unsigned int nchildren = 0;
		unsigned int j;

		if (XQueryTree(
		        dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
			for (j = 0; j < nchildren; j++)
				comp_add_by_xid(children[j]);
			if (children)
				XFree(children);
		}
	}

	comp.active = 1;

	/* Raise overlay so it sits above all windows */
	XRaiseWindow(dpy, comp.overlay);
	XMapWindow(dpy, comp.overlay);

	schedule_repaint();

	awm_debug(
	    "compositor: initialised (damage_ev_base=%d)", comp.damage_ev_base);
	return 0;
}

/* -------------------------------------------------------------------------
 * compositor_cleanup
 * ---------------------------------------------------------------------- */

void
compositor_cleanup(void)
{
	int      i;
	CompWin *cw, *next;

	if (!comp.active)
		return;

	/* Cancel pending repaint */
	if (comp.repaint_id) {
		g_source_remove(comp.repaint_id);
		comp.repaint_id = 0;
	}

	/* Free all tracked windows */
	for (cw = comp.windows; cw; cw = next) {
		next = cw->next;
		comp_free_win(cw);
		free(cw);
	}
	comp.windows = NULL;

	/* Free alpha pictures */
	for (i = 0; i < 256; i++) {
		if (comp.alpha_pict[i])
			XRenderFreePicture(dpy, comp.alpha_pict[i]);
	}

	/* Free back buffer */
	if (comp.back)
		XRenderFreePicture(dpy, comp.back);
	if (comp.back_pixmap)
		XFreePixmap(dpy, comp.back_pixmap);

	/* Free target picture and release overlay */
	if (comp.target)
		XRenderFreePicture(dpy, comp.target);
	if (comp.overlay)
		XCompositeReleaseOverlayWindow(dpy, comp.overlay);

	/* Free dirty region */
	if (comp.dirty)
		XFixesDestroyRegion(dpy, comp.dirty);

	/* Unredirect subwindows */
	XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);

	XFlush(dpy);
	comp.active = 0;
}

/* -------------------------------------------------------------------------
 * Window tracking — internal helpers
 * ---------------------------------------------------------------------- */

static CompWin *
comp_find_by_xid(Window w)
{
	CompWin *cw;
	for (cw = comp.windows; cw; cw = cw->next)
		if (cw->win == w)
			return cw;
	return NULL;
}

static CompWin *
comp_find_by_client(Client *c)
{
	CompWin *cw;
	for (cw = comp.windows; cw; cw = cw->next)
		if (cw->client == c)
			return cw;
	return NULL;
}

static void
comp_free_win(CompWin *cw)
{
	if (cw->damage) {
		XDamageDestroy(dpy, cw->damage);
		cw->damage = 0;
	}
	if (cw->picture) {
		XRenderFreePicture(dpy, cw->picture);
		cw->picture = None;
	}
	if (cw->pixmap) {
		XFreePixmap(dpy, cw->pixmap);
		cw->pixmap = None;
	}
}

static void
comp_refresh_pixmap(CompWin *cw)
{
	XRenderPictFormat       *fmt;
	XRenderPictureAttributes pa;

	if (cw->picture) {
		XRenderFreePicture(dpy, cw->picture);
		cw->picture = None;
	}
	if (cw->pixmap) {
		XFreePixmap(dpy, cw->pixmap);
		cw->pixmap = None;
	}

	xerror_push_ignore();
	cw->pixmap = XCompositeNameWindowPixmap(dpy, cw->win);
	XSync(dpy, False);
	xerror_pop();

	if (!cw->pixmap)
		return;

	{
		Window       root_ret;
		int          x, y;
		unsigned int w, h, bw, depth;
		if (!XGetGeometry(
		        dpy, cw->pixmap, &root_ret, &x, &y, &w, &h, &bw, &depth)) {
			XFreePixmap(dpy, cw->pixmap);
			cw->pixmap = None;
			return;
		}
	}

	xerror_push_ignore();
	fmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
	if (cw->argb)
		fmt = XRenderFindStandardFormat(dpy, PictStandardARGB32);

	pa.subwindow_mode = IncludeInferiors;
	cw->picture =
	    XRenderCreatePicture(dpy, cw->pixmap, fmt, CPSubwindowMode, &pa);
	XSync(dpy, False);
	xerror_pop();
}

/* Add window by X ID (used during init scan and on MapNotify). */
static void
comp_add_by_xid(Window w)
{
	XWindowAttributes wa;
	CompWin          *cw;

	/* Already tracked? */
	if (comp_find_by_xid(w))
		return;

	/* Skip the overlay itself */
	if (w == comp.overlay)
		return;

	xerror_push_ignore();
	int ok = XGetWindowAttributes(dpy, w, &wa);
	XSync(dpy, False);
	xerror_pop();

	if (!ok)
		return;

	/* Skip InputOnly windows */
	if (wa.class == InputOnly)
		return;

	/* Skip unmapped windows that aren't viewable */
	if (wa.map_state != IsViewable)
		return;

	cw = (CompWin *) calloc(1, sizeof(CompWin));
	if (!cw)
		return;

	cw->win        = w;
	cw->x          = wa.x;
	cw->y          = wa.y;
	cw->w          = wa.width;
	cw->h          = wa.height;
	cw->bw         = wa.border_width;
	cw->depth      = wa.depth;
	cw->argb       = (wa.depth == 32);
	cw->opacity    = 1.0;
	cw->redirected = 1;

	/* Find matching Client (may be NULL for override_redirect windows) */
	{
		Client  *c;
		Monitor *m;
		cw->client = NULL;
		for (m = mons; m; m = m->next)
			for (c = cl->clients; c; c = c->next)
				if (c->win == w) {
					cw->client = c;
					break;
				}
		if (cw->client)
			cw->opacity = cw->client->opacity;
	}

	comp_refresh_pixmap(cw);

	/* Create damage tracker */
	if (cw->pixmap) {
		xerror_push_ignore();
		cw->damage = XDamageCreate(dpy, w, XDamageReportNonEmpty);
		XSync(dpy, False);
		xerror_pop();
	}

	/* Prepend — we'll sort by Z-order via XQueryTree at paint time */
	cw->next     = comp.windows;
	comp.windows = cw;
}

/* -------------------------------------------------------------------------
 * Public API — called from WM core
 * ---------------------------------------------------------------------- */

void
compositor_add_window(Client *c)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	/* Window may already have been picked up by MapNotify */
	cw = comp_find_by_xid(c->win);
	if (cw) {
		cw->client  = c;
		cw->opacity = c->opacity;
		return;
	}

	comp_add_by_xid(c->win);

	/* Attach client pointer */
	cw = comp_find_by_xid(c->win);
	if (cw) {
		cw->client  = c;
		cw->opacity = c->opacity;
	}

	schedule_repaint();
}

void
compositor_remove_window(Client *c)
{
	CompWin *cw, *prev;

	if (!comp.active || !c)
		return;

	prev = NULL;
	for (cw = comp.windows; cw; cw = cw->next) {
		if (cw->client == c || cw->win == c->win) {
			if (prev)
				prev->next = cw->next;
			else
				comp.windows = cw->next;
			comp_free_win(cw);
			free(cw);
			schedule_repaint();
			return;
		}
		prev = cw;
	}
}

void
compositor_configure_window(Client *c)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	cw->x  = c->x - c->bw;
	cw->y  = c->y - c->bw;
	cw->w  = c->w;
	cw->h  = c->h;
	cw->bw = c->bw;

	if (cw->redirected)
		comp_refresh_pixmap(cw);

	schedule_repaint();
}

void
compositor_bypass_window(Client *c, int bypass)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	if (bypass == cw->redirected == 0)
		return; /* already in desired state */

	xerror_push_ignore();
	if (bypass) {
		XCompositeUnredirectWindow(dpy, c->win, CompositeRedirectManual);
		cw->redirected = 0;
		comp_free_win(cw); /* release stale picture */
	} else {
		XCompositeRedirectWindow(dpy, c->win, CompositeRedirectManual);
		cw->redirected = 1;
		comp_refresh_pixmap(cw);
		if (cw->pixmap && !cw->damage)
			cw->damage = XDamageCreate(dpy, c->win, XDamageReportNonEmpty);
	}
	XSync(dpy, False);
	xerror_pop();

	schedule_repaint();
}

void
compositor_set_opacity(Client *c, unsigned long raw)
{
	CompWin *cw;

	if (!comp.active || !c)
		return;

	cw = comp_find_by_client(c);
	if (!cw)
		return;

	cw->opacity = (raw == 0) ? 0.0 : (double) raw / (double) 0xFFFFFFFFUL;
	c->opacity  = cw->opacity;
	schedule_repaint();
}

void
compositor_damage_all(void)
{
	XRectangle full;

	if (!comp.active)
		return;

	full.x = full.y = 0;
	full.width      = (unsigned short) sw;
	full.height     = (unsigned short) sh;
	XFixesSetRegion(dpy, comp.dirty, &full, 1);
	schedule_repaint();
}

void
compositor_raise_overlay(void)
{
	if (!comp.active)
		return;
	XRaiseWindow(dpy, comp.overlay);
}

/* -------------------------------------------------------------------------
 * Event handler
 * ---------------------------------------------------------------------- */

void
compositor_handle_event(XEvent *ev)
{
	if (!comp.active)
		return;

	if (ev->type == comp.damage_ev_base + XDamageNotify) {
		XDamageNotifyEvent *dev = (XDamageNotifyEvent *) ev;
		XserverRegion       r;
		XRectangle          rect;

		/* Subtract the damage so the server resets the dirty state */
		XDamageSubtract(dpy, dev->damage, None, None);

		/* Union the damaged rect into comp.dirty */
		rect.x      = (short) dev->area.x;
		rect.y      = (short) dev->area.y;
		rect.width  = (unsigned short) dev->area.width;
		rect.height = (unsigned short) dev->area.height;
		r           = XFixesCreateRegion(dpy, &rect, 1);
		XFixesUnionRegion(dpy, comp.dirty, comp.dirty, r);
		XFixesDestroyRegion(dpy, r);

		schedule_repaint();
		return;
	}

	if (ev->type == MapNotify) {
		XMapEvent *mev = (XMapEvent *) ev;
		if (mev->event == root)
			comp_add_by_xid(mev->window);
		schedule_repaint();
		return;
	}

	if (ev->type == UnmapNotify) {
		XUnmapEvent *uev = (XUnmapEvent *) ev;
		CompWin     *cw  = comp_find_by_xid(uev->window);
		if (cw && !cw->client) {
			/* Override-redirect window unmapped — remove from tracking */
			CompWin *prev = NULL, *cur;
			for (cur = comp.windows; cur; cur = cur->next) {
				if (cur == cw) {
					if (prev)
						prev->next = cw->next;
					else
						comp.windows = cw->next;
					comp_free_win(cw);
					free(cw);
					break;
				}
				prev = cur;
			}
		}
		schedule_repaint();
		return;
	}

	if (ev->type == ConfigureNotify) {
		XConfigureEvent *cev = (XConfigureEvent *) ev;
		CompWin         *cw  = comp_find_by_xid(cev->window);
		if (cw) {
			cw->x  = cev->x;
			cw->y  = cev->y;
			cw->w  = cev->width;
			cw->h  = cev->height;
			cw->bw = cev->border_width;
			if (cw->redirected)
				comp_refresh_pixmap(cw);
			schedule_repaint();
		}
		return;
	}

	if (ev->type == DestroyNotify) {
		XDestroyWindowEvent *dev  = (XDestroyWindowEvent *) ev;
		CompWin             *prev = NULL, *cw;
		for (cw = comp.windows; cw; cw = cw->next) {
			if (cw->win == dev->window) {
				if (prev)
					prev->next = cw->next;
				else
					comp.windows = cw->next;
				comp_free_win(cw);
				free(cw);
				schedule_repaint();
				return;
			}
			prev = cw;
		}
		return;
	}
}

/* -------------------------------------------------------------------------
 * Repaint scheduler
 * ---------------------------------------------------------------------- */

static void
schedule_repaint(void)
{
	if (!comp.active || comp.repaint_id)
		return;

	comp.repaint_id = g_idle_add_full(
	    G_PRIORITY_DEFAULT_IDLE, comp_repaint_idle, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * Repaint — the core paint loop
 * ---------------------------------------------------------------------- */

static gboolean
comp_repaint_idle(gpointer data)
{
	Window       root_ret, parent_ret;
	Window      *children  = NULL;
	unsigned int nchildren = 0, i;
	XRenderColor bg_color  = { 0, 0, 0, 0xffff }; /* opaque black default */
	(void) data;

	comp.repaint_id = 0;

	if (!comp.active)
		return G_SOURCE_REMOVE;

	/* --- 1. Clip back-buffer to dirty region ------------------------------
	 */
	XFixesSetPictureClipRegion(dpy, comp.back, 0, 0, comp.dirty);

	/* --- 2. Clear dirty area on back-buffer -------------------------------
	 */
	XRenderFillRectangle(dpy, PictOpSrc, comp.back, &bg_color, 0, 0,
	    (unsigned int) sw, (unsigned int) sh);

	/* --- 3. Walk root children bottom-to-top (XQueryTree Z-order) ---------
	 */
	if (!XQueryTree(
	        dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
		XFixesSetPictureClipRegion(dpy, comp.back, 0, 0, None);
		return G_SOURCE_REMOVE;
	}

	for (i = 0; i < nchildren; i++) {
		CompWin *cw;
		int      alpha_idx;
		Picture  mask;

		if (children[i] == comp.overlay)
			continue;

		cw = comp_find_by_xid(children[i]);
		if (!cw || !cw->redirected || cw->picture == None)
			continue;

		/* Determine alpha mask */
		alpha_idx = (int) (cw->opacity * 255.0 + 0.5);
		if (alpha_idx < 0)
			alpha_idx = 0;
		if (alpha_idx > 255)
			alpha_idx = 255;

		if (cw->argb || alpha_idx < 255) {
			mask = comp.alpha_pict[alpha_idx];
			XRenderComposite(dpy, PictOpOver, cw->picture, mask, comp.back, 0,
			    0,    /* src x, y */
			    0, 0, /* mask x, y */
			    cw->x, cw->y, (unsigned int) (cw->w + 2 * cw->bw),
			    (unsigned int) (cw->h + 2 * cw->bw));
		} else {
			XRenderComposite(dpy, PictOpSrc, cw->picture, None, comp.back, 0,
			    0, 0, 0, cw->x, cw->y, (unsigned int) (cw->w + 2 * cw->bw),
			    (unsigned int) (cw->h + 2 * cw->bw));
		}
	}

	if (children)
		XFree(children);

	/* --- 4. Clip overlay target to dirty region then blit back → screen ---
	 */
	XFixesSetPictureClipRegion(dpy, comp.target, 0, 0, comp.dirty);
	XRenderComposite(dpy, PictOpSrc, comp.back, None, comp.target, 0, 0, 0, 0,
	    0, 0, (unsigned int) sw, (unsigned int) sh);

	/* --- 5. Reset dirty region --------------------------------------------
	 */
	XFixesSetRegion(dpy, comp.dirty, NULL, 0);

	/* Remove clip restrictions */
	XFixesSetPictureClipRegion(dpy, comp.back, 0, 0, None);
	XFixesSetPictureClipRegion(dpy, comp.target, 0, 0, None);

	XFlush(dpy);

	return G_SOURCE_REMOVE;
}

#endif /* COMPOSITOR */

/* Satisfy ISO C99: a translation unit must contain at least one declaration.
 */
typedef int compositor_translation_unit_nonempty;
