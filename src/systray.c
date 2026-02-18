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

/* Convert a Clr (allocated in the default colormap) to a 32-bit ARGB pixel
 * suitable for use with the systray's ARGB visual and colormap. */
unsigned long
clr_to_argb(Clr *clr)
{
	XColor xc;
	xc.pixel = clr->pixel;
	XQueryColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &xc);
	/* XColor channels are 16-bit; shift down to 8-bit and pack ARGB */
	return 0xFF000000UL | ((unsigned long) (xc.red >> 8) << 16) |
	    ((unsigned long) (xc.green >> 8) << 8) |
	    ((unsigned long) (xc.blue >> 8));
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
		XMapRaised(dpy, i->win);
		setclientstate(i, NormalState);
	} else if (!(flags & XEMBED_MAPPED) && i->tags) {
		i->tags = 0;
		XUnmapWindow(dpy, i->win);
		setclientstate(i, WithdrawnState);
	}
}

void
updatesystrayiconcolors(void)
{
	XColor        color;
	unsigned long colors[12];

	if (!showsystray || !systray)
		return;

	color.pixel = scheme[SchemeNorm][ColFg].pixel;
	XQueryColor(dpy, DefaultColormap(dpy, DefaultScreen(dpy)), &color);
	/* foreground color from bar scheme */
	colors[0] = color.red;
	colors[1] = color.green;
	colors[2] = color.blue;
	/* use same for error/warning/success - simple approach */
	colors[3] = colors[6] = colors[9] = color.red;
	colors[4] = colors[7] = colors[10] = color.green;
	colors[5] = colors[8] = colors[11] = color.blue;
	XChangeProperty(dpy, systray->win, netatom[NetSystemTrayColors],
	    XA_CARDINAL, 32, PropModeReplace, (unsigned char *) colors, 12);
}

void
updatesystray(void)
{
	XSetWindowAttributes wa;
	XWindowChanges       wc;
	Client              *i;
	Monitor             *m  = systraytomon(NULL);
	unsigned int         x  = m->mx + m->mw;
	unsigned int         sw = TEXTW(stext) - lrpad + systrayspacing;
	unsigned int         w  = 1;

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
		 * colormap context.  Fall back to the default visual if unavailable.
		 */
		{
			XVisualInfo vinfo;
			if (XMatchVisualInfo(
			        dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) {
				systray->visual = vinfo.visual;
				systray->colormap =
				    XCreateColormap(dpy, root, systray->visual, AllocNone);
			} else {
				systray->visual   = DefaultVisual(dpy, DefaultScreen(dpy));
				systray->colormap = DefaultColormap(dpy, DefaultScreen(dpy));
			}
		}

		unsigned long bgpix  = clr_to_argb(&scheme[SchemeNorm][ColBg]);
		wa.event_mask        = ButtonPressMask | ExposureMask;
		wa.override_redirect = True;
		wa.background_pixel  = bgpix;
		wa.border_pixel      = 0;
		wa.colormap          = systray->colormap;
		systray->win         = XCreateWindow(dpy, root, x, m->by, w, bh, 0, 32,
		            InputOutput, systray->visual,
		            CWEventMask | CWOverrideRedirect | CWBackPixel | CWBorderPixel |
		                CWColormap,
		            &wa);
		XSelectInput(dpy, systray->win, SubstructureNotifyMask);
		XChangeProperty(dpy, systray->win, netatom[NetSystemTrayOrientation],
		    XA_CARDINAL, 32, PropModeReplace,
		    (unsigned char *) &netatom[NetSystemTrayOrientationHorz], 1);
		{
			VisualID visual_id = XVisualIDFromVisual(systray->visual);
			XChangeProperty(dpy, systray->win, netatom[NetSystemTrayVisual],
			    XA_VISUALID, 32, PropModeReplace, (unsigned char *) &visual_id,
			    1);
		}
		updatesystrayiconcolors();
		XMapRaised(dpy, systray->win);
		XSetSelectionOwner(
		    dpy, netatom[NetSystemTray], systray->win, CurrentTime);
		if (XGetSelectionOwner(dpy, netatom[NetSystemTray]) == systray->win) {
			sendevent(root, xatom[Manager], StructureNotifyMask, CurrentTime,
			    netatom[NetSystemTray], systray->win, 0, 0);
			XSync(dpy, False);
		} else {
			awm_error("Unable to obtain system tray window");
			free(systray);
			systray = NULL;
			return;
		}
	}
	for (w = 0, i = systray->icons; i; i = i->next) {
		if (!i->issni) {
			wa.background_pixel = clr_to_argb(&scheme[SchemeNorm][ColBg]);
			XChangeWindowAttributes(dpy, i->win, CWBackPixel, &wa);
		}
		XMapRaised(dpy, i->win);
		w += systrayspacing;
		i->x = w;
		XMoveResizeWindow(dpy, i->win, i->x, 0, i->w, i->h);
		w += i->w;
		if (i->mon != m)
			i->mon = m;
	}
	w = w ? w + systrayspacing : 1;
	x -= w;
	XMoveResizeWindow(dpy, systray->win, x, m->by, w, bh);
	wc.x          = x;
	wc.y          = m->by;
	wc.width      = w;
	wc.height     = bh;
	wc.stack_mode = Above;
	wc.sibling    = m->barwin;
	XConfigureWindow(dpy, systray->win,
	    CWX | CWY | CWWidth | CWHeight | CWSibling | CWStackMode, &wc);
	XMapWindow(dpy, systray->win);
	XMapSubwindows(dpy, systray->win);
	/* redraw background — XClearWindow uses the window's own background
	 * pixel, avoiding a BadMatch from using drw->gc (depth-24) on a
	 * depth-32 window. */
	XClearWindow(dpy, systray->win);
	XSync(dpy, False);
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
	XReparentWindow(dpy, i->win, systray->win, 0, 0);

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
	updatesystray();
}
