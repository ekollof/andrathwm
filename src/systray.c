/* AndrathWM - XEMBED systray functions
 * See LICENSE file for copyright and license details. */

#include "systray.h"
#include "awm.h"
#include "client.h"
#include "ewmh.h"
#include "monitor.h"
#include "spawn.h"
#include "xrdb.h"
#include "config.h"

/* Convert a Clr (XftColor) to a 32-bit ARGB pixel suitable for use with
 * the systray's ARGB visual and colormap.  XftColor already carries the
 * pre-multiplied XRenderColor channels populated by XftColorAllocName /
 * XftColorAllocValue — no X roundtrip needed. */
unsigned long
clr_to_argb(Clr *clr)
{
	/* XRenderColor channels are 16-bit (0–65535); shift to 8-bit and pack. */
	return 0xFF000000UL | ((unsigned long) (clr->color.red >> 8) << 16) |
	    ((unsigned long) (clr->color.green >> 8) << 8) |
	    ((unsigned long) (clr->color.blue >> 8));
}

unsigned int
getsystraywidth(void)
{
	unsigned int w = 0;
	Client      *i;
	if (showsystray)
		for (i = systray->icons; i; w += i->w + systrayspacing, i = i->next)
			;
	return w ? w + systrayspacing : 1;
}

void
removesystrayicon(Client *i)
{
	Client **ii;

	if (!showsystray || !i)
		return;
	for (ii = &systray->icons; *ii && *ii != i; ii = &(*ii)->next)
		;
	if (ii)
		*ii = i->next;
	freeicon(i);
	free(i);
}

void
updatesystrayicongeom(Client *i, int w, int h)
{
	if (i) {
		i->h = bh;
		if (w == h)
			i->w = bh;
		else if (h == bh)
			i->w = w;
		else
			i->w = (int) ((float) bh * ((float) w / (float) h));
		applysizehints(i, &(i->x), &(i->y), &(i->w), &(i->h), False);
		/* force icons into the systray dimensions if they don't want to */
		if (i->h > bh) {
			if (i->w == i->h)
				i->w = bh;
			else
				i->w = (int) ((float) bh * ((float) i->w / (float) i->h));
			i->h = bh;
		}
	}
}

void
updatesystrayiconstate(Client *i, XPropertyEvent *ev)
{
	long flags;

	if (!showsystray || !i || ev->atom != xatom[XembedInfo] ||
	    !(flags = getembedinfo(i)))
		return;

	/* Per XEMBED spec: track XEMBED_MAPPED flag and map/unmap accordingly. */
	if (flags & XEMBED_MAPPED && !i->tags) {
		i->tags = 1;
		{
			xcb_connection_t *xc    = XGetXCBConnection(dpy);
			uint32_t          above = XCB_STACK_MODE_ABOVE;
			xcb_map_window(xc, i->win);
			xcb_configure_window(
			    xc, i->win, XCB_CONFIG_WINDOW_STACK_MODE, &above);
		}
		setclientstate(i, NormalState);
	} else if (!(flags & XEMBED_MAPPED) && i->tags) {
		i->tags = 0;
		xcb_unmap_window(XGetXCBConnection(dpy), i->win);
		setclientstate(i, WithdrawnState);
	}
}

void
updatesystrayiconcolors(void)
{
	uint32_t colors[12];
	uint32_t r, g, b;

	if (!showsystray || !systray)
		return;

	/* XftColor already carries XRenderColor channels — no X roundtrip. */
	r = (uint32_t) scheme[SchemeNorm][ColFg].color.red;
	g = (uint32_t) scheme[SchemeNorm][ColFg].color.green;
	b = (uint32_t) scheme[SchemeNorm][ColFg].color.blue;

	/* foreground color from bar scheme */
	colors[0] = r;
	colors[1] = g;
	colors[2] = b;
	/* use same for error/warning/success - simple approach */
	colors[3] = colors[6] = colors[9] = r;
	colors[4] = colors[7] = colors[10] = g;
	colors[5] = colors[8] = colors[11] = b;
	xcb_change_property(XGetXCBConnection(dpy), XCB_PROP_MODE_REPLACE,
	    systray->win, (xcb_atom_t) netatom[NetSystemTrayColors],
	    XCB_ATOM_CARDINAL, 32, 12, colors);
}

void
updatesystray(void)
{
	Client      *i;
	Monitor     *m  = systraytomon(NULL);
	unsigned int x  = m->mx + m->mw;
	unsigned int sw = TEXTW(stext) - lrpad + systrayspacing;
	unsigned int w  = 1;

	if (!showsystray)
		return;
	if (systrayonleft)
		x -= sw + lrpad / 2;
	if (!systray) {
		/* init systray */
		if (!(systray = (Systray *) calloc(1, sizeof(Systray))))
			die("fatal: could not malloc() %u bytes\n", sizeof(Systray));

		/* Prefer a 32-bit ARGB visual so XEMBED clients that use one
		 * (e.g. nm-applet, pasystray) share the same visual depth and
		 * colormap context.  Walk the XCB screen's allowed depths to
		 * find a 32-bit TrueColor visual; fall back to the default
		 * screen visual if none is available. */
		{
			xcb_connection_t     *xc    = XGetXCBConnection(dpy);
			const xcb_setup_t    *setup = xcb_get_setup(xc);
			xcb_screen_iterator_t si    = xcb_setup_roots_iterator(setup);
			/* Advance to our screen number */
			for (int s = DefaultScreen(dpy); s > 0; s--)
				xcb_screen_next(&si);
			xcb_screen_t *scr = si.data;

			xcb_visualid_t       found_vis = 0;
			xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(scr);
			for (; di.rem && !found_vis; xcb_depth_next(&di)) {
				if (di.data->depth != 32)
					continue;
				xcb_visualtype_iterator_t vi =
				    xcb_depth_visuals_iterator(di.data);
				for (; vi.rem; xcb_visualtype_next(&vi)) {
					if (vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
						found_vis = vi.data->visual_id;
						break;
					}
				}
			}

			if (found_vis) {
				systray->visual_id = found_vis;
				systray->colormap  = xcb_generate_id(xc);
				xcb_create_colormap(xc, XCB_COLORMAP_ALLOC_NONE,
				    systray->colormap, root, found_vis);
			} else {
				systray->visual_id = scr->root_visual;
				systray->colormap  = scr->default_colormap;
			}
		}

		{
			xcb_connection_t *xc = XGetXCBConnection(dpy);
			uint32_t          bgpix =
			    (uint32_t) clr_to_argb(&scheme[SchemeNorm][ColBg]);
			uint32_t evmask = ButtonPressMask | ExposureMask;
			uint32_t one    = 1; /* override_redirect */
			uint32_t cmap   = (uint32_t) systray->colormap;
			uint32_t border = 0;
			/* CW values must be in ascending bit-position order:
			 * CWBackPixel(2), CWBorderPixel(4), CWOverrideRedirect(512),
			 * CWColormap(8192), CWEventMask(2048) — sorted: back(2),
			 * border(4), event(2048), override(512), colormap(8192).
			 * XCB mask bits: BACK_PIXEL=2, BORDER_PIXEL=4,
			 * OVERRIDE_REDIRECT=512, EVENT_MASK=2048, COLORMAP=8192.
			 * Ascending order: BACK_PIXEL, BORDER_PIXEL, OVERRIDE_REDIRECT,
			 * EVENT_MASK, COLORMAP */
			uint32_t cw_vals[5] = { bgpix, border, one, evmask, cmap };
			systray->win        = xcb_generate_id(xc);
			xcb_create_window(xc, 32, systray->win, root, (int16_t) x,
			    (int16_t) m->by, (uint16_t) w, (uint16_t) bh, 0,
			    XCB_WINDOW_CLASS_INPUT_OUTPUT, systray->visual_id,
			    XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
			        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK |
			        XCB_CW_COLORMAP,
			    cw_vals);
			/* SubstructureNotifyMask for icon embed events */
			{
				uint32_t mask = SubstructureNotifyMask;
				xcb_change_window_attributes(
				    xc, systray->win, XCB_CW_EVENT_MASK, &mask);
			}
			/* _NET_SYSTEM_TRAY_ORIENTATION */
			{
				uint32_t horz =
				    (uint32_t) netatom[NetSystemTrayOrientationHorz];
				xcb_change_property(xc, XCB_PROP_MODE_REPLACE, systray->win,
				    (xcb_atom_t) netatom[NetSystemTrayOrientation],
				    XCB_ATOM_CARDINAL, 32, 1, &horz);
			}
			/* _NET_SYSTEM_TRAY_VISUAL */
			{
				uint32_t vis_id = (uint32_t) systray->visual_id;
				xcb_change_property(xc, XCB_PROP_MODE_REPLACE, systray->win,
				    (xcb_atom_t) netatom[NetSystemTrayVisual],
				    XCB_ATOM_VISUALID, 32, 1, &vis_id);
			}
			updatesystrayiconcolors();
			/* XMapRaised equivalent */
			{
				uint32_t above = XCB_STACK_MODE_ABOVE;
				xcb_map_window(xc, systray->win);
				xcb_configure_window(
				    xc, systray->win, XCB_CONFIG_WINDOW_STACK_MODE, &above);
			}
		}
		XSetSelectionOwner(
		    dpy, netatom[NetSystemTray], systray->win, CurrentTime);
		if (XGetSelectionOwner(dpy, netatom[NetSystemTray]) == systray->win) {
			sendevent(root, xatom[Manager], StructureNotifyMask, CurrentTime,
			    netatom[NetSystemTray], systray->win, 0, 0);
			xflush(dpy);
		} else {
			awm_error("Unable to obtain system tray window");
			free(systray);
			systray = NULL;
			return;
		}
	}
	{
		xcb_connection_t *xc = XGetXCBConnection(dpy);
		for (w = 0, i = systray->icons; i; i = i->next) {
			if (!i->issni) {
				uint32_t bg =
				    (uint32_t) clr_to_argb(&scheme[SchemeNorm][ColBg]);
				xcb_change_window_attributes(
				    xc, i->win, XCB_CW_BACK_PIXEL, &bg);
			}
			/* XMapRaised equivalent */
			{
				uint32_t above = XCB_STACK_MODE_ABOVE;
				xcb_map_window(xc, i->win);
				xcb_configure_window(
				    xc, i->win, XCB_CONFIG_WINDOW_STACK_MODE, &above);
			}
			w += systrayspacing;
			i->x = w;
			{
				uint32_t xywh[4] = {
					(uint32_t) (int32_t) i->x,
					0,
					(uint32_t) i->w,
					(uint32_t) i->h,
				};
				xcb_configure_window(xc, i->win,
				    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
				        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
				    xywh);
			}
			w += i->w;
			if (i->mon != m)
				i->mon = m;
		}
		w = w ? w + systrayspacing : 1;
		x -= w;
		/* Move/resize systray container */
		{
			uint32_t xywh[4] = {
				(uint32_t) (int32_t) x,
				(uint32_t) (int32_t) m->by,
				(uint32_t) w,
				(uint32_t) bh,
			};
			xcb_configure_window(xc, systray->win,
			    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
			        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
			    xywh);
		}
		/* Stack systray above barwin */
		{
			uint32_t stack_vals[3] = {
				(uint32_t) m->barwin, /* sibling */
				XCB_STACK_MODE_ABOVE, /* stack_mode */
			};
			xcb_configure_window(xc, systray->win,
			    XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
			    stack_vals);
		}
		xcb_map_window(xc, systray->win);
		xcb_map_subwindows(xc, systray->win);
	}
	/* Flush buffered requests to the X server without blocking for
	 * completion.  xcb_flush avoids the round-trip overhead of XSync/XFlush.
	 * The window background is filled automatically from background_pixel set
	 * at creation time whenever the server exposes previously-hidden areas,
	 * so no explicit XClearWindow call is needed here. */
	xflush(dpy);
}

Client *
wintosystrayicon(Window w)
{
	Client *i = NULL;

	if (!showsystray || !w)
		return i;
	for (i = systray->icons; i && i->win != w; i = i->next)
		;
	return i;
}

void
addsniiconsystray(Window w, int width, int height)
{
	Client *i;

	if (!showsystray || !w)
		return;

	/* Check if already exists */
	if (wintosystrayicon(w))
		return;

	/* Allocate new systray icon */
	if (!(i = (Client *) calloc(1, sizeof(Client))))
		die("fatal: could not malloc() %u bytes\n", sizeof(Client));

	i->win         = w;
	i->mon         = selmon;
	i->next        = systray->icons;
	systray->icons = i;
	i->tags        = 1; /* Mark as visible */
	i->issni       = 1; /* Mark as SNI icon */

	/* Set geometry */
	updatesystrayicongeom(i, width, height);

	awm_debug("SNI icon geometry after geom update: %dx%d", i->w, i->h);

	/* Reparent to systray container */
	xcb_reparent_window(XGetXCBConnection(dpy), i->win, systray->win, 0, 0);

	/* Update systray layout */
	updatesystray();

	awm_debug("Added SNI window 0x%lx to systray (final size: %dx%d)", w, i->w,
	    i->h);
}

void
removesniiconsystray(Window w)
{
	Client *i;

	if (!showsystray || !w)
		return;

	i = wintosystrayicon(w);
	if (!i)
		return;

	awm_debug("Removing SNI window 0x%lx from systray", w);
	removesystrayicon(i);
	/* Guard against teardown: mons/selmon may already be freed when
	 * sni_cleanup() is called after cleanupmon() during WM exit. */
	if (mons)
		updatesystray();
}
