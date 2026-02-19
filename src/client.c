/* AndrathWM - client management
 * See LICENSE file for copyright and license details. */

#include "client.h"
#include "awm.h"
#include "events.h"
#include "ewmh.h"
#include "monitor.h"
#include "spawn.h"
#include "systray.h"
#include "xrdb.h"
#include "config.h"

/* ---- compile-time invariants ---- */
_Static_assert(sizeof(Atom) >= 4,
    "sizeof(Atom) is used as the XGetWindowProperty length argument; must be "
    ">= 4 bytes");

/* module-local strings */
static const char broken[] = "broken";

void
applyrules(Client *c)
{
	const char *class, *instance;
	unsigned int i;
	const Rule  *r;
	Monitor     *m;
	XClassHint   ch = { NULL, NULL };

	/* rule matching */
	c->iscentered = 0;
	c->isfloating = 0;
	c->ishidden   = 0;
	c->tags       = 0;
	c->scratchkey = 0;
	XGetClassHint(dpy, c->win, &ch);
	class    = ch.res_class ? ch.res_class : broken;
	instance = ch.res_name ? ch.res_name : broken;

	if (strstr(class, "Steam") || strstr(class, "steam_app_"))
		c->issteam = 1;

	for (i = 0; i < LENGTH(rules); i++) {
		r = &rules[i];
		if ((!r->title || strstr(c->name, r->title)) &&
		    (!r->class || strstr(class, r->class)) &&
		    (!r->instance || strstr(instance, r->instance))) {
			c->iscentered = r->iscentered;
			c->isfloating = r->isfloating;
			c->tags |= r->tags;
			c->scratchkey = r->scratchkey;
			if (r->opacity > 0.0)
				c->opacity = r->opacity;
			for (m = mons; m && (m->tagset[m->seltags] & c->tags) == 0;
			     m = m->next)
				;
			if (m)
				c->mon = m;
		}
	}
	if (ch.res_class)
		XFree(ch.res_class);
	if (ch.res_name)
		XFree(ch.res_name);

	/* Scratchpads always start hidden (tags=0 means off-screen until
	 * toggled).  For normal clients, fall back to the monitor's current
	 * tagset if no tag was assigned by the rule. */
	if (c->scratchkey)
		c->tags = 0;
	else
		c->tags = c->tags & TAGMASK ? c->tags & TAGMASK
		                            : c->mon->tagset[c->mon->seltags];
}

int
applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact)
{
	int      baseismin;
	Monitor *m = c->mon;

	/* set minimum possible */
	*w = MAX(1, *w);
	*h = MAX(1, *h);
	if (interact) {
		if (*x > sw)
			*x = sw - WIDTH(c);
		if (*y > sh)
			*y = sh - HEIGHT(c);
		if (*x + *w + 2 * c->bw < 0)
			*x = 0;
		if (*y + *h + 2 * c->bw < 0)
			*y = 0;
	} else {
		if (*x >= m->wx + m->ww)
			*x = m->wx + m->ww - WIDTH(c);
		if (*y >= m->wy + m->wh)
			*y = m->wy + m->wh - HEIGHT(c);
		if (*x + *w + 2 * c->bw <= m->wx)
			*x = m->wx;
		if (*y + *h + 2 * c->bw <= m->wy)
			*y = m->wy;
	}
	if (*h < bh)
		*h = bh;
	if (*w < bh)
		*w = bh;
	if (resizehints || c->isfloating || !c->mon->lt[c->mon->sellt]->arrange) {
		if (!c->hintsvalid)
			updatesizehints(c);
		/* see last two sentences in ICCCM 4.1.2.3 */
		baseismin = c->basew == c->minw && c->baseh == c->minh;
		if (!baseismin) {
			*w -= c->basew;
			*h -= c->baseh;
		}
		if (c->mina > 0 && c->maxa > 0) {
			if (c->maxa < (float) *w / *h)
				*w = *h * c->maxa + 0.5;
			else if (c->mina < (float) *h / *w)
				*h = *w * c->mina + 0.5;
		}
		if (baseismin) {
			*w -= c->basew;
			*h -= c->baseh;
		}
		if (c->incw)
			*w -= *w % c->incw;
		if (c->inch)
			*h -= *h % c->inch;
		*w = MAX(*w + c->basew, c->minw);
		*h = MAX(*h + c->baseh, c->minh);
		if (c->maxw)
			*w = MIN(*w, c->maxw);
		if (c->maxh)
			*h = MIN(*h, c->maxh);
	}
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

void
attach(Client *c)
{
	c->next             = c->mon->cl->clients;
	c->mon->cl->clients = c;
}

void
attachclients(Monitor *m)
{
	Monitor     *tm;
	Client      *c;
	unsigned int utags = 0;
	Bool         rmons = False;
	if (!m)
		return;

	for (tm = mons; tm; tm = tm->next)
		if (tm != m)
			utags |= tm->tagset[tm->seltags];

	for (c = m->cl->clients; c; c = c->next)
		if (ISVISIBLE(c, m)) {
			if (c->tags & utags) {
				c->tags = c->tags & m->tagset[m->seltags];
				rmons   = True;
			}
			unfocus(c, True);
			c->mon = m;
		}

	if (rmons)
		for (tm = mons; tm; tm = tm->next)
			if (tm != m)
				arrange(tm);
}

void
attachstack(Client *c)
{
	c->snext          = c->mon->cl->stack;
	c->mon->cl->stack = c;
}

void
configure(Client *c)
{
	XConfigureEvent ce;

	ce.type              = ConfigureNotify;
	ce.display           = dpy;
	ce.event             = c->win;
	ce.window            = c->win;
	ce.x                 = c->x;
	ce.y                 = c->y;
	ce.width             = c->w;
	ce.height            = c->h;
	ce.border_width      = c->bw;
	ce.above             = None;
	ce.override_redirect = False;
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *) &ce);
}

void
detach(Client *c)
{
	Client **tc;

	for (tc = &c->mon->cl->clients; *tc && *tc != c; tc = &(*tc)->next)
		;
	*tc = c->next;
}

void
detachstack(Client *c)
{
	Client **tc, *t;

	for (tc = &c->mon->cl->stack; *tc && *tc != c; tc = &(*tc)->snext)
		;
	*tc = c->snext;

	if (c == c->mon->sel) {
		for (t = c->mon->cl->stack; t && !ISVISIBLE(t, c->mon); t = t->snext)
			;
		c->mon->sel = t;
	}
}

void
focus(Client *c)
{
	if (!c || !ISVISIBLE(c, selmon))
		for (c = selmon->cl->stack; c && !ISVISIBLE(c, selmon); c = c->snext)
			;
	if (selmon->sel && selmon->sel != c)
		unfocus(selmon->sel, 0);
	if (c) {
		if (c->mon != selmon)
			selmon = c->mon;
		if (c->isurgent)
			seturgent(c, 0);
		detachstack(c);
		attachstack(c);
		grabbuttons(c, 1);
		XSetWindowBorder(dpy, c->win, scheme[SchemeSel][ColBorder].pixel);
		if (!selmon->pertag->drawwithgaps[selmon->pertag->curtag] &&
		    !c->isfloating) {
			XWindowChanges wc;
			wc.sibling    = selmon->barwin;
			wc.stack_mode = Below;
			XConfigureWindow(dpy, c->win, CWSibling | CWStackMode, &wc);
		}
		setfocus(c);
	} else {
		XSetInputFocus(dpy, selmon->barwin, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
	selmon->sel = c;
	if (selmon->lt[selmon->sellt]->arrange == monocle)
		arrangemon(selmon);
	barsdirty = 1;
#ifdef COMPOSITOR
	/* Dirty the border region of both the newly focused and previously
	 * focused client so the compositor repaints them in the correct colour. */
	compositor_focus_window(c);
#endif
}

void
focusstack(const Arg *arg)
{
	Client *c = NULL, *i;

	if (!selmon->sel || (selmon->sel->isfullscreen && lockfullscreen))
		return;
	if (arg->i > 0) {
		for (c = selmon->sel->next; c && !ISVISIBLE(c, selmon); c = c->next)
			;
		if (!c)
			for (c = selmon->cl->clients; c && !ISVISIBLE(c, selmon);
			     c = c->next)
				;
	} else {
		for (i = selmon->cl->clients; i != selmon->sel; i = i->next)
			if (ISVISIBLE(i, selmon))
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i, selmon))
					c = i;
	}
	if (c) {
		focus(c);
		restack(selmon);
	}
}

void
focusstackhidden(const Arg *arg)
{
	Client *c = NULL, *i;

	if (!selmon->sel || (selmon->sel->isfullscreen && lockfullscreen))
		return;

	if (arg->i > 0) {
		for (c = selmon->sel->next;
		     c && !(c->tags & selmon->tagset[selmon->seltags]); c = c->next)
			;
		if (!c)
			for (c = selmon->cl->clients;
			     c && !(c->tags & selmon->tagset[selmon->seltags]);
			     c = c->next)
				;
	} else {
		for (i = selmon->cl->clients; i != selmon->sel; i = i->next)
			if (i->tags & selmon->tagset[selmon->seltags])
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (i->tags & selmon->tagset[selmon->seltags])
					c = i;
	}

	if (c) {
		if (c->ishidden)
			show(c);
		else {
			focus(c);
			restack(selmon);
		}
	}
}

void
focuswin(const Arg *arg)
{
	Client *c = (Client *) arg->v;

	if (!c)
		return;

	if (c->ishidden) {
		show(c);
		return;
	}

	if (c == selmon->sel) {
		hide(c);
		return;
	}

	if (ISVISIBLE(c, selmon)) {
		if (selmon->lt[selmon->sellt]->arrange && !c->isfloating) {
			pop(c);
		} else {
			focus(c);
			restack(selmon);
		}
	}
}

void
freeicon(Client *c)
{
	if (c->icon) {
		cairo_surface_destroy(c->icon);
		c->icon = NULL;
	}
}

Atom
getatomprop(Client *c, Atom prop)
{
	int            di;
	unsigned long  dl;
	unsigned char *p = NULL;
	Atom           da, atom = None;
	Atom req = (prop == xatom[XembedInfo]) ? xatom[XembedInfo] : XA_ATOM;

	if (XGetWindowProperty(dpy, c->win, prop, 0L, sizeof atom, False, req, &da,
	        &di, &dl, &dl, &p) == Success &&
	    p) {
		atom = *(Atom *) p;
		XFree(p);
	}
	return atom;
}

int
getrootptr(int *x, int *y)
{
	int          di;
	unsigned int dui;
	Window       dummy;

	return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

long
getstate(Window w)
{
	int            format;
	long           result = -1;
	unsigned char *p      = NULL;
	unsigned long  n, extra;
	Atom           real;

	if (XGetWindowProperty(dpy, w, wmatom[WMState], 0L, 2L, False,
	        wmatom[WMState], &real, &format, &n, &extra,
	        (unsigned char **) &p) != Success)
		return -1;
	if (n != 0)
		result = *p;
	XFree(p);
	return result;
}

int
gettextprop(Window w, Atom atom, char *text, unsigned int size)
{
	char        **list = NULL;
	int           n;
	XTextProperty name;

	if (!text || size == 0)
		return 0;
	text[0] = '\0';
	if (!XGetTextProperty(dpy, w, &name, atom) || !name.nitems)
		return 0;
	if (name.encoding == XA_STRING) {
		strncpy(text, (char *) name.value, size - 1);
	} else if (XmbTextPropertyToTextList(dpy, &name, &list, &n) >= Success &&
	    n > 0 && *list) {
		strncpy(text, *list, size - 1);
		XFreeStringList(list);
	}
	text[size - 1] = '\0';
	XFree(name.value);
	return 1;
}

cairo_surface_t *
getwmicon(Window w, int size)
{
	Atom             type;
	int              format;
	unsigned long    nitems, bytes;
	unsigned char   *prop    = NULL;
	cairo_surface_t *surface = NULL;

	if (XGetWindowProperty(dpy, w, netatom[NetWMIcon], 0, LONG_MAX, False,
	        AnyPropertyType, &type, &format, &nitems, &bytes,
	        &prop) != Success ||
	    !prop)
		return NULL;

	if (nitems > 2) {
		unsigned long *data   = (unsigned long *) prop;
		unsigned long  icon_w = data[0];
		unsigned long  icon_h = data[1];

		if (nitems >= 2 + icon_w * icon_h) {
			cairo_surface_t *src;
			cairo_t         *cr;
			unsigned char   *argb_data;
			unsigned long    i;
			int              stride;

			awm_debug(
			    "extracting %lux%lu icon, nitems=%lu", icon_w, icon_h, nitems);

			stride =
			    cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, icon_w);
			argb_data = calloc(icon_h, stride);
			if (!argb_data) {
				XFree(prop);
				return NULL;
			}

			for (i = 0; i < icon_w * icon_h; i++) {
				unsigned long pixel = data[2 + i];
				unsigned char a     = (pixel >> 24) & 0xff;
				unsigned char r     = (pixel >> 16) & 0xff;
				unsigned char g     = (pixel >> 8) & 0xff;
				unsigned char b     = pixel & 0xff;

				unsigned char *q =
				    argb_data + (i / icon_w) * stride + (i % icon_w) * 4;

				if (a == 0) {
					q[0] = q[1] = q[2] = q[3] = 0;
				} else if (a == 255) {
					q[0] = b;
					q[1] = g;
					q[2] = r;
					q[3] = a;
				} else {
					q[0] = (b * a) / 255;
					q[1] = (g * a) / 255;
					q[2] = (r * a) / 255;
					q[3] = a;
				}
			}

			awm_debug(
			    "first 4 pixels (BGRA): [%02x%02x%02x%02x] [%02x%02x%02x%02x] "
			    "[%02x%02x%02x%02x] [%02x%02x%02x%02x]",
			    argb_data[3], argb_data[2], argb_data[1], argb_data[0],
			    argb_data[7], argb_data[6], argb_data[5], argb_data[4],
			    argb_data[11], argb_data[10], argb_data[9], argb_data[8],
			    argb_data[15], argb_data[14], argb_data[13], argb_data[12]);

			src = cairo_image_surface_create_for_data(
			    argb_data, CAIRO_FORMAT_ARGB32, icon_w, icon_h, stride);

			surface =
			    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
			cr = cairo_create(surface);

			cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
			cairo_paint(cr);
			cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

			if (icon_w != size || icon_h != size) {
				double scale_x = (double) size / icon_w;
				double scale_y = (double) size / icon_h;
				cairo_scale(cr, scale_x, scale_y);
			}

			cairo_set_source_surface(cr, src, 0, 0);
			cairo_paint(cr);

			cairo_destroy(cr);
			cairo_surface_destroy(src);
			free(argb_data);
		}
	}

	XFree(prop);
	return surface;
}

void
grabbuttons(Client *c, int focused)
{
	updatenumlockmask();
	{
		unsigned int i, j;
		unsigned int modifiers[] = { 0, LockMask, numlockmask,
			numlockmask | LockMask };
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		if (!focused)
			XGrabButton(dpy, AnyButton, AnyModifier, c->win, False, BUTTONMASK,
			    GrabModeSync, GrabModeSync, None, None);
		for (i = 0; i < LENGTH(buttons); i++)
			if (buttons[i].click == ClkClientWin)
				for (j = 0; j < LENGTH(modifiers); j++)
					XGrabButton(dpy, buttons[i].button,
					    buttons[i].mask | modifiers[j], c->win, False,
					    BUTTONMASK, GrabModeAsync, GrabModeSync, None, None);
	}
}

void
hide(Client *c)
{
	if (!c || c->ishidden)
		return;

	Window                   w = c->win;
	static XWindowAttributes ra, ca;

	XGrabServer(dpy);
	XGetWindowAttributes(dpy, root, &ra);
	XGetWindowAttributes(dpy, w, &ca);
	XSelectInput(dpy, root, ra.your_event_mask & ~SubstructureNotifyMask);
	XSelectInput(dpy, w, ca.your_event_mask & ~StructureNotifyMask);
	XUnmapWindow(dpy, w);
	setclientstate(c, IconicState);
	XSelectInput(dpy, root, ra.your_event_mask);
	XSelectInput(dpy, w, ca.your_event_mask);
	XUngrabServer(dpy);

	c->ishidden = 1;
	focus(NULL);
	arrange(c->mon);
	barsdirty = 1;
}

void
hidewin(const Arg *arg)
{
	Client *c = (Client *) arg->v;
	if (!c)
		c = selmon->sel;
	if (!c)
		return;
	hide(c);
}

void
show(Client *c)
{
	if (!c || !c->ishidden)
		return;

	XMapWindow(dpy, c->win);
	setclientstate(c, NormalState);
	c->ishidden = 0;
	focus(c);
	arrange(c->mon);
	barsdirty = 1;
}

void
restorewin(const Arg *arg)
{
	Client *c = (Client *) arg->v;
	if (!c)
		for (c = selmon->cl->stack; c && !c->ishidden; c = c->snext)
			;
	if (!c)
		return;
	show(c);
}

void
showall(const Arg *arg)
{
	Client *c;

	for (c = selmon->cl->clients; c; c = c->next)
		if (c->ishidden && (c->tags & selmon->tagset[selmon->seltags]))
			show(c);
}

void
incnmaster(const Arg *arg)
{
	selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag] =
	    MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

void
killclient(const Arg *arg)
{
	if (!selmon->sel)
		return;

	if (!sendevent(selmon->sel->win, wmatom[WMDelete], NoEventMask,
	        wmatom[WMDelete], CurrentTime, 0, 0, 0)) {
		XGrabServer(dpy);
		XSetErrorHandler(xerrordummy);
		XSetCloseDownMode(dpy, DestroyAll);
		XKillClient(dpy, selmon->sel->win);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
}

void
manage(Window w, XWindowAttributes *wa)
{
	Client        *c, *t = NULL;
	Window         trans = None;
	XWindowChanges wc;

	c      = ecalloc(1, sizeof(Client));
	c->win = w;
	c->x = c->oldx = wa->x;
	c->y = c->oldy = wa->y;
	c->w = c->oldw = wa->width;
	c->h = c->oldh       = wa->height;
	c->oldbw             = wa->border_width;
	c->opacity           = 1.0;
	c->bypass_compositor = 0;

	updatetitle(c);
	c->icon = getwmicon(w, 16);
	if (XGetTransientForHint(dpy, w, &trans) && (t = wintoclient(trans))) {
		c->mon  = t->mon;
		c->tags = t->tags;
	} else {
		c->mon = selmon;
		applyrules(c);
	}
#ifdef COMPOSITOR
	/* If the window already has _NET_WM_WINDOW_OPACITY set (common for apps
	 * that manage their own translucency), let it override the rule value so
	 * the window always wins over the rule default. */
	{
		unsigned long raw =
		    (unsigned long) getatomprop(c, netatom[NetWMWindowOpacity]);
		if (raw != 0)
			c->opacity = (double) raw / (double) 0xFFFFFFFFUL;
	}
#endif

	if (c->x + WIDTH(c) > c->mon->wx + c->mon->ww)
		c->x = c->mon->wx + c->mon->ww - WIDTH(c);
	if (c->y + HEIGHT(c) > c->mon->wy + c->mon->wh)
		c->y = c->mon->wy + c->mon->wh - HEIGHT(c);
	c->x  = MAX(c->x, c->mon->wx);
	c->y  = MAX(c->y, c->mon->wy);
	c->bw = borderpx;

	wc.border_width = c->bw;
	XConfigureWindow(dpy, w, CWBorderWidth, &wc);
	XSetWindowBorder(dpy, w, scheme[SchemeNorm][ColBorder].pixel);
	configure(c);
	updatewindowtype(c);
	updatesizehints(c);
	updatewmhints(c);
	if (c->iscentered) {
		c->x = c->mon->mx + (c->mon->mw - WIDTH(c)) / 2;
		c->y = c->mon->my + (c->mon->mh - HEIGHT(c)) / 2;
	}
	XSelectInput(dpy, w,
	    EnterWindowMask | FocusChangeMask | PropertyChangeMask |
	        StructureNotifyMask);
	grabbuttons(c, 0);
	if (!c->isfloating)
		c->isfloating = c->oldstate = t != NULL || c->isfixed;
	if (c->isfloating)
		XRaiseWindow(dpy, c->win);
	attach(c);
	attachstack(c);
	XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32,
	    PropModeAppend, (unsigned char *) &(c->win), 1);

	setewmhdesktop(c);
	setwmstate(c);

	{
		long extents[4] = { c->bw, c->bw, c->bw, c->bw };
		XChangeProperty(dpy, c->win, netatom[NetFrameExtents], XA_CARDINAL, 32,
		    PropModeReplace, (unsigned char *) extents, 4);
	}

	XMoveResizeWindow(dpy, c->win, c->x + 2 * sw, c->y, c->w, c->h);
	setclientstate(c, NormalState);
	if (c->mon == selmon)
		unfocus(selmon->sel, 0);
	/* Don't make a hidden scratchpad the selected client */
	if (!c->scratchkey)
		c->mon->sel = c;
	arrange(c->mon);
	XMapWindow(dpy, c->win);
#ifdef COMPOSITOR
	compositor_add_window(c);
	/* Force-sync the CompWin geometry to the client struct.  During a
	 * restart, arrange() runs before the CompWin exists (comp_add_by_xid
	 * skips unmapped windows), so compositor_configure_window() was a
	 * no-op.  If comp_add_by_xid later captured stale X server geometry,
	 * this call corrects it. */
	compositor_configure_window(c, c->bw);
	c->bypass_compositor =
	    (int) getatomprop(c, netatom[NetWMBypassCompositor]);
	if (c->bypass_compositor == 1)
		compositor_bypass_window(c, 1);
#endif
	focus(NULL);
}

void
movemouse(const Arg *arg)
{
	int      x, y, ocx, ocy, nx, ny;
	Client  *c;
	Monitor *m;
	XEvent   ev;
	Time     lasttime = 0;

	if (!(c = selmon->sel))
		return;
	if (c->isfullscreen)
		return;
	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
	        None, cursor[CurMove]->cursor, CurrentTime) != GrabSuccess)
		return;
	if (!getrootptr(&x, &y))
		return;
	do {
		XMaskEvent(
		    dpy, MOUSEMASK | ExposureMask | SubstructureRedirectMask, &ev);
		switch (ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / motionfps))
				continue;
			lasttime = ev.xmotion.time;

			nx = ocx + (ev.xmotion.x - x);
			ny = ocy + (ev.xmotion.y - y);
			if (abs(selmon->wx - nx) < snap)
				nx = selmon->wx;
			else if (abs((selmon->wx + selmon->ww) - (nx + WIDTH(c))) < snap)
				nx = selmon->wx + selmon->ww - WIDTH(c);
			if (abs(selmon->wy - ny) < snap)
				ny = selmon->wy;
			else if (abs((selmon->wy + selmon->wh) - (ny + HEIGHT(c))) < snap)
				ny = selmon->wy + selmon->wh - HEIGHT(c);
			if (!c->isfloating && selmon->lt[selmon->sellt]->arrange &&
			    (abs(nx - c->x) > snap || abs(ny - c->y) > snap))
				togglefloating(NULL);
			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
				resize(c, nx, ny, c->w, c->h, 1);
#ifdef COMPOSITOR
			compositor_repaint_now();
#endif
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}
}

Client *
nexttiled(Client *c, Monitor *m)
{
	for (; c && (c->isfloating || !ISVISIBLE(c, m) || c->ishidden);
	     c = c->next)
		;
	return c;
}

void
pop(Client *c)
{
	detach(c);
	attach(c);
	focus(c);
	arrange(c->mon);
}

void
resize(Client *c, int x, int y, int w, int h, int interact)
{
	if (applysizehints(c, &x, &y, &w, &h, interact))
		resizeclient(c, x, y, w, h);
}

void
resizeclient(Client *c, int x, int y, int w, int h)
{
	XWindowChanges wc;

	c->oldx = c->x;
	c->x = wc.x = x;
	c->oldy     = c->y;
	c->y = wc.y = y;
	c->oldw     = c->w;
	c->w = wc.width = w;
	c->oldh         = c->h;
	c->h = wc.height = h;
	wc.border_width  = c->bw;
	if (!selmon->pertag->drawwithgaps[selmon->pertag->curtag] &&
	    (((nexttiled(c->mon->cl->clients, selmon) == c &&
	          !nexttiled(c->next, selmon)) ||
	        &monocle == c->mon->lt[c->mon->sellt]->arrange)) &&
	    !c->isfullscreen && !c->isfloating &&
	    NULL != c->mon->lt[c->mon->sellt]->arrange) {
		c->w            = wc.width += c->bw * 2;
		c->h            = wc.height += c->bw * 2;
		wc.border_width = 0;
	}
	XConfigureWindow(
	    dpy, c->win, CWX | CWY | CWWidth | CWHeight | CWBorderWidth, &wc);
	configure(c);
	XSync(dpy, False);
#ifdef COMPOSITOR
	compositor_configure_window(c, wc.border_width);
#endif
}

void
resizemouse(const Arg *arg)
{
	int      ocx, ocy, nw, nh;
	Client  *c;
	Monitor *m;
	XEvent   ev;
	Time     lasttime = 0;

	if (!(c = selmon->sel))
		return;
	if (c->isfullscreen)
		return;
	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
	        None, cursor[CurResize]->cursor, CurrentTime) != GrabSuccess)
		return;
	XWarpPointer(
	    dpy, None, c->win, 0, 0, 0, 0, c->w + c->bw - 1, c->h + c->bw - 1);
	do {
		XMaskEvent(
		    dpy, MOUSEMASK | ExposureMask | SubstructureRedirectMask, &ev);
		switch (ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / motionfps))
				continue;
			lasttime = ev.xmotion.time;

			nw = MAX(ev.xmotion.x - ocx - 2 * c->bw + 1, 1);
			nh = MAX(ev.xmotion.y - ocy - 2 * c->bw + 1, 1);
			if (c->mon->wx + nw >= selmon->wx &&
			    c->mon->wx + nw <= selmon->wx + selmon->ww &&
			    c->mon->wy + nh >= selmon->wy &&
			    c->mon->wy + nh <= selmon->wy + selmon->wh) {
				if (!c->isfloating && selmon->lt[selmon->sellt]->arrange &&
				    (abs(nw - c->w) > snap || abs(nh - c->h) > snap))
					togglefloating(NULL);
			}
			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
				resize(c, c->x, c->y, nw, nh, 1);
#ifdef COMPOSITOR
			compositor_repaint_now();
#endif
			break;
		}
	} while (ev.type != ButtonRelease);
	XWarpPointer(
	    dpy, None, c->win, 0, 0, 0, 0, c->w + c->bw - 1, c->h + c->bw - 1);
	XUngrabPointer(dpy, CurrentTime);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev))
		;
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}
}

void
sendmon(Client *c, Monitor *m)
{
	if (c->mon == m)
		return;
	unfocus(c, 1);
	detachstack(c);
	c->mon  = m;
	c->tags = m->tagset[m->seltags];
	attachstack(c);
	focus(NULL);
	arrange(NULL);
}

void
setclientstate(Client *c, long state)
{
	long data[] = { state, None };

	XChangeProperty(dpy, c->win, wmatom[WMState], wmatom[WMState], 32,
	    PropModeReplace, (unsigned char *) data, 2);
}

void
setfullscreen(Client *c, int fullscreen)
{
	if (fullscreen && !c->isfullscreen) {
		c->isfullscreen = 1;
		c->oldstate     = c->isfloating;
		c->oldbw        = c->bw;
		c->bw           = 0;
		c->isfloating   = 1;
		setwmstate(c);
#ifdef COMPOSITOR
		compositor_bypass_window(c, 1);
#endif
		resizeclient(c, c->mon->mx, c->mon->my, c->mon->mw, c->mon->mh);
		XRaiseWindow(dpy, c->win);
	} else if (!fullscreen && c->isfullscreen) {
		c->isfullscreen = 0;
		c->isfloating   = c->oldstate;
		c->bw           = c->oldbw;
		c->x            = c->oldx;
		c->y            = c->oldy;
		c->w            = c->oldw;
		c->h            = c->oldh;
		setwmstate(c);
#ifdef COMPOSITOR
		compositor_bypass_window(c, 0);
#endif
		resizeclient(c, c->x, c->y, c->w, c->h);
#ifdef COMPOSITOR
		compositor_raise_overlay();
#endif
		arrange(c->mon);
	}
}

void
setgaps(const Arg *arg)
{
	switch (arg->i) {
	case GAP_TOGGLE:
		selmon->pertag->drawwithgaps[selmon->pertag->curtag] =
		    !selmon->pertag->drawwithgaps[selmon->pertag->curtag];
		break;
	case GAP_RESET:
		if (selmon->pertag->curtag > 0)
			selmon->pertag->gappx[selmon->pertag->curtag] =
			    gappx[selmon->pertag->curtag - 1 % LENGTH(gappx)];
		else
			selmon->pertag->gappx[0] = gappx[0];
		break;
	default:
		if (selmon->pertag->gappx[selmon->pertag->curtag] + arg->i < 0)
			selmon->pertag->gappx[selmon->pertag->curtag] = 0;
		else
			selmon->pertag->gappx[selmon->pertag->curtag] += arg->i;
	}
	arrange(selmon);
}

void
setlayout(const Arg *arg)
{
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
		selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag] ^= 1;
	if (arg && arg->v)
		selmon->lt[selmon->sellt] =
		    selmon->pertag
		        ->ltidxs[(selmon->pertag->curtag) * 2 + (selmon->sellt)] =
		        (Layout *) arg->v;
	strncpy(selmon->ltsymbol, selmon->lt[selmon->sellt]->symbol,
	    sizeof selmon->ltsymbol);
	if (selmon->sel)
		arrange(selmon);
	else
		drawbar(selmon);
}

void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0 ? arg->f + selmon->mfact : arg->f - 1.0;
	if (f < 0.05 || f > 0.95)
		return;
	selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag] = f;
	arrange(selmon);
}

void
seturgent(Client *c, int urg)
{
	XWMHints *wmh;

	c->isurgent = urg;
	if (!(wmh = XGetWMHints(dpy, c->win)))
		return;
	wmh->flags =
	    urg ? (wmh->flags | XUrgencyHint) : (wmh->flags & ~XUrgencyHint);
	XSetWMHints(dpy, c->win, wmh);
	XFree(wmh);
	setwmstate(c);
}

void
showhide(Client *c)
{
	if (!c)
		return;
	if (ISVISIBLE(c, c->mon) && !c->ishidden) {
		XMoveWindow(dpy, c->win, c->x, c->y);
		if ((!c->mon->lt[c->mon->sellt]->arrange || c->isfloating) &&
		    !c->isfullscreen)
			resize(c, c->x, c->y, c->w, c->h, 0);
		showhide(c->snext);
	} else {
		showhide(c->snext);
		XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y);
	}
}

void
tag(const Arg *arg)
{
	Monitor     *m;
	unsigned int newtags;
	if (selmon->sel && arg->ui & TAGMASK) {
		newtags = arg->ui & TAGMASK;
		for (m = mons; m; m = m->next)
			if (m != selmon && m->tagset[m->seltags] & newtags) {
				if (newtags & selmon->tagset[selmon->seltags])
					return;
				selmon->sel->tags = newtags;
				selmon->sel->mon  = m;
				setewmhdesktop(selmon->sel);
				arrange(m);
				break;
			}
		selmon->sel->tags = arg->ui & TAGMASK;
		setewmhdesktop(selmon->sel);
		focus(NULL);
		arrange(selmon);
	}
}

void
tagmon(const Arg *arg)
{
	if (!selmon->sel || !mons->next)
		return;
	sendmon(selmon->sel, dirtomon(arg->i));
}

void
togglefloating(const Arg *arg)
{
	if (!selmon->sel)
		return;
	if (selmon->sel->isfullscreen)
		return;
	selmon->sel->isfloating = !selmon->sel->isfloating || selmon->sel->isfixed;
	if (selmon->sel->isfloating)
		resize(selmon->sel, selmon->sel->x, selmon->sel->y, selmon->sel->w,
		    selmon->sel->h, 0);
	arrange(selmon);
}

void
togglescratch(const Arg *arg)
{
	Client      *c;
	unsigned int found = 0;

	for (c = selmon->cl->clients;
	     c && !(found = c->scratchkey == ((char **) arg->v)[0][0]);
	     c = c->next)
		;
	if (found) {
		if (ISVISIBLE(c, selmon)) {
			/* Hide: remove from all tags */
			c->tags = 0;
			focus(NULL);
			arrange(selmon);
		} else {
			/* Show: move to selmon, re-centre if changing monitor */
			if (c->mon != selmon) {
				detachstack(c);
				c->mon = selmon;
				attachstack(c);
				/* Re-centre on the new monitor */
				c->x = selmon->mx + (selmon->mw - WIDTH(c)) / 2;
				c->y = selmon->my + (selmon->mh - HEIGHT(c)) / 2;
			}
			c->tags = selmon->tagset[selmon->seltags];
			arrange(selmon);
			focus(c);
			restack(selmon);
		}
	} else {
		spawnscratch(arg);
	}
}

void
toggletag(const Arg *arg)
{
	unsigned int newtags;

	if (!selmon->sel)
		return;
	newtags = selmon->sel->tags ^ (arg->ui & TAGMASK);
	if (newtags) {
		selmon->sel->tags = newtags;
		setewmhdesktop(selmon->sel);
		focus(NULL);
		arrange(selmon);
	}
	updatecurrentdesktop();
}

void
toggleview(const Arg *arg)
{
	Monitor     *m;
	unsigned int newtagset =
	    selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK);
	int i;

	if (newtagset) {
		for (m = mons; m; m = m->next)
			if (m != selmon && newtagset & m->tagset[m->seltags]) {
				int selmon_curtag, m_curtag, j;

				if (selmon->tagset[selmon->seltags] == ~0)
					selmon_curtag = 0;
				else {
					for (i = 0; !(selmon->tagset[selmon->seltags] & 1 << i);
					     i++)
						;
					selmon_curtag = i + 1;
				}

				if (newtagset == ~0)
					m_curtag = 0;
				else {
					for (i = 0; !(newtagset & 1 << i); i++)
						;
					m_curtag = i + 1;
				}

				selmon->pertag->nmasters[m_curtag] =
				    m->pertag->nmasters[m_curtag];
				selmon->pertag->mfacts[m_curtag] = m->pertag->mfacts[m_curtag];
				selmon->pertag->sellts[m_curtag] = m->pertag->sellts[m_curtag];
				selmon->pertag->showbars[m_curtag] =
				    m->pertag->showbars[m_curtag];
				for (j = 0; j < 2; j++)
					selmon->pertag->ltidxs[(m_curtag) * 2 + (j)] =
					    m->pertag->ltidxs[(m_curtag) * 2 + (j)];

				m->pertag->nmasters[selmon_curtag] =
				    selmon->pertag->nmasters[selmon_curtag];
				m->pertag->mfacts[selmon_curtag] =
				    selmon->pertag->mfacts[selmon_curtag];
				m->pertag->sellts[selmon_curtag] =
				    selmon->pertag->sellts[selmon_curtag];
				m->pertag->showbars[selmon_curtag] =
				    selmon->pertag->showbars[selmon_curtag];
				for (j = 0; j < 2; j++)
					m->pertag->ltidxs[(selmon_curtag) * 2 + (j)] =
					    selmon->pertag->ltidxs[(selmon_curtag) * 2 + (j)];

				m->sel = selmon->sel;
				m->seltags ^= 1;
				m->tagset[m->seltags] = selmon->tagset[selmon->seltags];
				m->pertag->curtag     = selmon_curtag;

				m->nmaster = m->pertag->nmasters[m->pertag->curtag];
				m->mfact   = m->pertag->mfacts[m->pertag->curtag];
				m->sellt   = m->pertag->sellts[m->pertag->curtag];
				m->lt[m->sellt] =
				    m->pertag->ltidxs[(m->pertag->curtag) * 2 + (m->sellt)];
				m->lt[m->sellt ^ 1] =
				    m->pertag
				        ->ltidxs[(m->pertag->curtag) * 2 + (m->sellt ^ 1)];
				if (m->showbar != m->pertag->showbars[m->pertag->curtag])
					togglebar(NULL);

				attachclients(m);
				arrange(m);

				selmon->tagset[selmon->seltags] = newtagset;
				selmon->pertag->prevtag         = selmon->pertag->curtag;
				selmon->pertag->curtag          = m_curtag;

				selmon->nmaster =
				    selmon->pertag->nmasters[selmon->pertag->curtag];
				selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag];
				selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag];
				selmon->lt[selmon->sellt] =
				    selmon->pertag->ltidxs[(selmon->pertag->curtag) * 2 +
				        (selmon->sellt)];
				selmon->lt[selmon->sellt ^ 1] =
				    selmon->pertag->ltidxs[(selmon->pertag->curtag) * 2 +
				        (selmon->sellt ^ 1)];
				if (selmon->showbar !=
				    selmon->pertag->showbars[selmon->pertag->curtag])
					togglebar(NULL);

				attachclients(selmon);
				arrange(selmon);
				focus(NULL);
				updatecurrentdesktop();
				return;
			}

		selmon->tagset[selmon->seltags] = newtagset;

		if (newtagset == ~0) {
			selmon->pertag->prevtag = selmon->pertag->curtag;
			selmon->pertag->curtag  = 0;
		}

		if (!(newtagset & 1 << (selmon->pertag->curtag - 1))) {
			selmon->pertag->prevtag = selmon->pertag->curtag;
			for (i = 0; !(newtagset & 1 << i); i++)
				;
			selmon->pertag->curtag = i + 1;
		}

		selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
		selmon->mfact   = selmon->pertag->mfacts[selmon->pertag->curtag];
		selmon->sellt   = selmon->pertag->sellts[selmon->pertag->curtag];
		selmon->lt[selmon->sellt] =
		    selmon->pertag
		        ->ltidxs[(selmon->pertag->curtag) * 2 + (selmon->sellt)];
		selmon->lt[selmon->sellt ^ 1] =
		    selmon->pertag
		        ->ltidxs[(selmon->pertag->curtag) * 2 + (selmon->sellt ^ 1)];

		if (selmon->showbar !=
		    selmon->pertag->showbars[selmon->pertag->curtag])
			togglebar(NULL);

		attachclients(selmon);
		arrange(selmon);
		focus(NULL);
	}
	updatecurrentdesktop();
}

void
unfocus(Client *c, int setfocus)
{
	if (!c)
		return;
	grabbuttons(c, 0);
	XSetWindowBorder(dpy, c->win, scheme[SchemeNorm][ColBorder].pixel);
#ifdef COMPOSITOR
	compositor_focus_window(c);
#endif
	if (setfocus) {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
}

void
unmanage(Client *c, int destroyed)
{
	Monitor       *m = c->mon;
	XWindowChanges wc;

	detach(c);
	detachstack(c);
	if (!destroyed) {
		wc.border_width = c->oldbw;
		XGrabServer(dpy);
		XSetErrorHandler(xerrordummy);
		XSelectInput(dpy, c->win, NoEventMask);
		XConfigureWindow(dpy, c->win, CWBorderWidth, &wc);
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		setclientstate(c, WithdrawnState);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
	freeicon(c);
#ifdef COMPOSITOR
	compositor_remove_window(c);
#endif
	free(c);
	focus(NULL);
	updateclientlist();
	arrange(m);
}

void
updatesizehints(Client *c)
{
	long       msize;
	XSizeHints size;

	if (!XGetWMNormalHints(dpy, c->win, &size, &msize))
		size.flags = PSize;
	if (size.flags & PBaseSize) {
		c->basew = size.base_width;
		c->baseh = size.base_height;
	} else if (size.flags & PMinSize) {
		c->basew = size.min_width;
		c->baseh = size.min_height;
	} else
		c->basew = c->baseh = 0;
	if (size.flags & PResizeInc) {
		c->incw = size.width_inc;
		c->inch = size.height_inc;
	} else
		c->incw = c->inch = 0;
	if (size.flags & PMaxSize) {
		c->maxw = size.max_width;
		c->maxh = size.max_height;
	} else
		c->maxw = c->maxh = 0;
	if (size.flags & PMinSize) {
		c->minw = size.min_width;
		c->minh = size.min_height;
	} else if (size.flags & PBaseSize) {
		c->minw = size.base_width;
		c->minh = size.base_height;
	} else
		c->minw = c->minh = 0;
	if (size.flags & PAspect) {
		c->mina = (float) size.min_aspect.y / size.min_aspect.x;
		c->maxa = (float) size.max_aspect.x / size.max_aspect.y;
	} else
		c->maxa = c->mina = 0.0;
	c->isfixed =
	    (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
	c->hintsvalid = 1;
}

void
updatetitle(Client *c)
{
	if (!gettextprop(c->win, netatom[NetWMName], c->name, sizeof c->name))
		gettextprop(c->win, XA_WM_NAME, c->name, sizeof c->name);
	if (c->name[0] == '\0')
		strcpy(c->name, broken);
}

void
updatewindowtype(Client *c)
{
	Atom state = getatomprop(c, netatom[NetWMState]);
	Atom wtype = getatomprop(c, netatom[NetWMWindowType]);

	if (state == netatom[NetWMFullscreen])
		setfullscreen(c, 1);
	if (wtype == netatom[NetWMWindowTypeDialog]) {
		c->iscentered = 1;
		c->isfloating = 1;
	}
}

void
updatewmhints(Client *c)
{
	XWMHints *wmh;

	if ((wmh = XGetWMHints(dpy, c->win))) {
		if (c == selmon->sel && wmh->flags & XUrgencyHint) {
			wmh->flags &= ~XUrgencyHint;
			XSetWMHints(dpy, c->win, wmh);
		} else
			c->isurgent = (wmh->flags & XUrgencyHint) ? 1 : 0;
		if (wmh->flags & InputHint)
			c->neverfocus = !wmh->input;
		else
			c->neverfocus = 0;
		XFree(wmh);
	}
}

void
view(const Arg *arg)
{
	Monitor     *m;
	int          i;
	unsigned int tmptag;
	unsigned int newtagset = selmon->tagset[selmon->seltags ^ 1];

	if ((arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	if (arg->ui & TAGMASK)
		newtagset = arg->ui & TAGMASK;
	for (m = mons; m; m = m->next)
		if (m != selmon && newtagset & m->tagset[m->seltags]) {
			if (newtagset & selmon->tagset[selmon->seltags])
				return;
			int selmon_curtag, m_curtag, j;

			if (selmon->tagset[selmon->seltags] == ~0)
				selmon_curtag = 0;
			else {
				for (i = 0; !(selmon->tagset[selmon->seltags] & 1 << i); i++)
					;
				selmon_curtag = i + 1;
			}

			if (newtagset == ~0)
				m_curtag = 0;
			else {
				for (i = 0; !(newtagset & 1 << i); i++)
					;
				m_curtag = i + 1;
			}

			selmon->pertag->nmasters[m_curtag] = m->pertag->nmasters[m_curtag];
			selmon->pertag->mfacts[m_curtag]   = m->pertag->mfacts[m_curtag];
			selmon->pertag->sellts[m_curtag]   = m->pertag->sellts[m_curtag];
			selmon->pertag->showbars[m_curtag] = m->pertag->showbars[m_curtag];
			for (j = 0; j < 2; j++)
				selmon->pertag->ltidxs[(m_curtag) * 2 + (j)] =
				    m->pertag->ltidxs[(m_curtag) * 2 + (j)];

			m->pertag->nmasters[selmon_curtag] =
			    selmon->pertag->nmasters[selmon_curtag];
			m->pertag->mfacts[selmon_curtag] =
			    selmon->pertag->mfacts[selmon_curtag];
			m->pertag->sellts[selmon_curtag] =
			    selmon->pertag->sellts[selmon_curtag];
			m->pertag->showbars[selmon_curtag] =
			    selmon->pertag->showbars[selmon_curtag];
			for (j = 0; j < 2; j++)
				m->pertag->ltidxs[(selmon_curtag) * 2 + (j)] =
				    selmon->pertag->ltidxs[(selmon_curtag) * 2 + (j)];

			m->sel = selmon->sel;
			m->seltags ^= 1;
			m->tagset[m->seltags] = selmon->tagset[selmon->seltags];
			m->pertag->curtag     = selmon_curtag;

			m->nmaster = m->pertag->nmasters[m->pertag->curtag];
			m->mfact   = m->pertag->mfacts[m->pertag->curtag];
			m->sellt   = m->pertag->sellts[m->pertag->curtag];
			m->lt[m->sellt] =
			    m->pertag->ltidxs[(m->pertag->curtag) * 2 + (m->sellt)];
			m->lt[m->sellt ^ 1] =
			    m->pertag->ltidxs[(m->pertag->curtag) * 2 + (m->sellt ^ 1)];
			if (m->showbar != m->pertag->showbars[m->pertag->curtag])
				togglebar(NULL);

			attachclients(m);
			arrange(m);

			selmon->seltags ^= 1;
			selmon->tagset[selmon->seltags] = newtagset;
			selmon->pertag->prevtag         = selmon->pertag->curtag;
			selmon->pertag->curtag          = m_curtag;

			selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
			selmon->mfact   = selmon->pertag->mfacts[selmon->pertag->curtag];
			selmon->sellt   = selmon->pertag->sellts[selmon->pertag->curtag];
			selmon->lt[selmon->sellt] =
			    selmon->pertag
			        ->ltidxs[(selmon->pertag->curtag) * 2 + (selmon->sellt)];
			selmon->lt[selmon->sellt ^ 1] =
			    selmon->pertag->ltidxs[(selmon->pertag->curtag) * 2 +
			        (selmon->sellt ^ 1)];
			if (selmon->showbar !=
			    selmon->pertag->showbars[selmon->pertag->curtag])
				togglebar(NULL);

			attachclients(selmon);
			arrange(selmon);
			focus(NULL);
			updatecurrentdesktop();
			return;
		}
	selmon->seltags ^= 1;
	if (arg->ui & TAGMASK) {
		selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
		selmon->pertag->prevtag         = selmon->pertag->curtag;

		if (arg->ui == ~0)
			selmon->pertag->curtag = 0;
		else {
			for (i = 0; !(arg->ui & 1 << i); i++)
				;
			selmon->pertag->curtag = i + 1;
		}
	} else {
		tmptag                  = selmon->pertag->prevtag;
		selmon->pertag->prevtag = selmon->pertag->curtag;
		selmon->pertag->curtag  = tmptag;
	}

	selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
	selmon->mfact   = selmon->pertag->mfacts[selmon->pertag->curtag];
	selmon->sellt   = selmon->pertag->sellts[selmon->pertag->curtag];
	selmon->lt[selmon->sellt] =
	    selmon->pertag->ltidxs[(selmon->pertag->curtag) * 2 + (selmon->sellt)];
	selmon->lt[selmon->sellt ^ 1] =
	    selmon->pertag
	        ->ltidxs[(selmon->pertag->curtag) * 2 + (selmon->sellt ^ 1)];

	if (selmon->showbar != selmon->pertag->showbars[selmon->pertag->curtag])
		togglebar(NULL);

	attachclients(selmon);
	arrange(selmon);
	focus(NULL);
	updatecurrentdesktop();
}

void
warp(const Client *c)
{
	int x, y;

	if (!c) {
		XWarpPointer(dpy, None, root, 0, 0, 0, 0, selmon->wx + selmon->ww / 2,
		    selmon->wy + selmon->wh / 2);
		return;
	}

	if (!getrootptr(&x, &y) ||
	    (x > c->x - c->bw && y > c->y - c->bw && x < c->x + c->w + c->bw * 2 &&
	        y < c->y + c->h + c->bw * 2) ||
	    (y > c->mon->by && y < c->mon->by + bh) || (c->mon->topbar && !y))
		return;

	XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w / 2, c->h / 2);
}

Client *
wintoclient(Window w)
{
	Client  *c;
	Monitor *m;

	for (m = mons; m; m = m->next)
		for (c = m->cl->clients; c; c = c->next)
			if (c->win == w)
				return c;
	return NULL;
}

void
zoom(const Arg *arg)
{
	Client *c = selmon->sel;

	if (!selmon->lt[selmon->sellt]->arrange || !c || c->isfloating)
		return;
	if (c == nexttiled(selmon->cl->clients, selmon) &&
	    !(c = nexttiled(c->next, selmon)))
		return;
	pop(c);
}

void
movestack(const Arg *arg)
{
	Client *c = NULL, *p = NULL, *pc = NULL, *i;

	if (arg->i > 0) {
		/* find the client after selmon->sel */
		for (c = selmon->sel->next;
		     c && (!ISVISIBLE(c, selmon) || c->isfloating); c = c->next)
			;
		if (!c)
			for (c = selmon->cl->clients;
			     c && (!ISVISIBLE(c, selmon) || c->isfloating); c = c->next)
				;

	} else {
		/* find the client before selmon->sel */
		for (i = selmon->cl->clients; i != selmon->sel; i = i->next)
			if (ISVISIBLE(i, selmon) && !i->isfloating)
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i, selmon) && !i->isfloating)
					c = i;
	}
	/* find the client before selmon->sel and c */
	for (i = selmon->cl->clients; i && (!p || !pc); i = i->next) {
		if (i->next == selmon->sel)
			p = i;
		if (i->next == c)
			pc = i;
	}

	/* swap c and selmon->sel in the clients list */
	if (c && c != selmon->sel) {
		Client *temp =
		    selmon->sel->next == c ? selmon->sel : selmon->sel->next;
		selmon->sel->next = c->next == selmon->sel ? c : c->next;
		c->next           = temp;

		if (p && p != c)
			p->next = c;
		if (pc && pc != selmon->sel)
			pc->next = selmon->sel;

		if (selmon->sel == selmon->cl->clients)
			selmon->cl->clients = c;
		else if (c == selmon->cl->clients)
			selmon->cl->clients = selmon->sel;

		arrange(selmon);
	}
}
