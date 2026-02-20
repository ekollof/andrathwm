/* AndrathWM - monitor management
 * See LICENSE file for copyright and license details. */

#include "monitor.h"
#include "awm.h"
#include "client.h"
#include "ewmh.h"
#include "spawn.h"
#include "systray.h"
#include "xrdb.h"
#include "config.h"

#ifdef XINERAMA
static int
isuniquegeom(XineramaScreenInfo *unique, size_t n, XineramaScreenInfo *info)
{
	while (n--)
		if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org &&
		    unique[n].width == info->width && unique[n].height == info->height)
			return 0;
	return 1;
}
#endif /* XINERAMA */

void
arrange(Monitor *m)
{
	if (m)
		showhide(m->cl->stack);
	else
		for (m = mons; m; m = m->next)
			showhide(m->cl->stack);
	if (m) {
		arrangemon(m);
		restack(m);
	} else {
		for (m = mons; m; m = m->next)
			arrangemon(m);
		/* Flush all pending requests and discard stale EnterNotify
		 * events so we don't spuriously change focus after a
		 * layout change.  All operations stay on the XCB connection;
		 * non-EnterNotify events are put back into the Xlib queue. */
		{
			xcb_connection_t    *xc = XGetXCBConnection(dpy);
			xcb_generic_event_t *xe;
			xcb_flush(xc);
			while ((xe = xcb_poll_for_event(xc))) {
				if ((xe->response_type & ~0x80) != XCB_ENTER_NOTIFY)
					XPutBackEvent(dpy, (XEvent *) (void *) xe); /* NOLINT */
				free(xe);
			}
		}
	}
}

void
arrangemon(Monitor *m)
{
	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, sizeof m->ltsymbol);
	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
}

void
cleanupmon(Monitor *mon)
{
	Monitor *m;

	if (mon == mons)
		mons = mons->next;
	else {
		for (m = mons; m && m->next != mon; m = m->next)
			;
		m->next = mon->next;
	}
	xcb_connection_t *xc = XGetXCBConnection(dpy);
	xcb_unmap_window(xc, mon->barwin);
	xcb_destroy_window(xc, mon->barwin);
	free(mon->pertag->nmasters);
	free(mon->pertag->mfacts);
	free(mon->pertag->sellts);
	free(mon->pertag->ltidxs);
	free(mon->pertag->showbars);
	free(mon->pertag->drawwithgaps);
	free(mon->pertag->gappx);
	free(mon->pertag);
	free(mon);
}

Monitor *
createmon(void)
{
	Monitor     *m, *tm;
	unsigned int i;

	/* bail out if the number of monitors exceeds the number of tags */
	for (i = 1, tm = mons; tm; i++, tm = tm->next)
		;
	if (i > LENGTH(tags)) {
		awm_error("failed to add monitor, number of tags exceeded");
		return NULL;
	}
	/* find the first tag that isn't in use */
	for (i = 0; i < LENGTH(tags); i++) {
		for (tm = mons; tm && !(tm->tagset[tm->seltags] & (1 << i));
		    tm  = tm->next)
            ;
		if (!tm)
			break;
	}
	/* reassign all tags to monitors since there's currently no free tag for
	 * the new monitor */
	if (i >= LENGTH(tags))
		for (i = 0, tm = mons; tm; tm = tm->next, i++) {
			tm->seltags ^= 1;
			tm->tagset[tm->seltags] = (1 << i) & TAGMASK;
		}
	m            = ecalloc(1, sizeof(Monitor));
	m->cl        = cl;
	m->tagset[0] = m->tagset[1] = (1 << i) & TAGMASK;
	m->mfact                    = mfact;
	m->nmaster                  = nmaster;
	m->showbar                  = showbar;
	m->topbar                   = topbar;
	m->lt[0]                    = &layouts[0];
	m->lt[1]                    = &layouts[1 % LENGTH(layouts)];
	strncpy(m->ltsymbol, layouts[0].symbol, sizeof m->ltsymbol);
	m->pertag         = ecalloc(1, sizeof(Pertag));
	m->pertag->curtag = m->pertag->prevtag = 1;
	m->pertag->nmasters     = ecalloc(TAGSLENGTH + 1, sizeof(int));
	m->pertag->mfacts       = ecalloc(TAGSLENGTH + 1, sizeof(float));
	m->pertag->sellts       = ecalloc(TAGSLENGTH + 1, sizeof(unsigned int));
	m->pertag->ltidxs       = ecalloc((TAGSLENGTH + 1) * 2, sizeof(Layout *));
	m->pertag->showbars     = ecalloc(TAGSLENGTH + 1, sizeof(int));
	m->pertag->drawwithgaps = ecalloc(TAGSLENGTH + 1, sizeof(int));
	m->pertag->gappx        = ecalloc(TAGSLENGTH + 1, sizeof(unsigned int));

	for (i = 0; i <= LENGTH(tags); i++) {
		m->pertag->nmasters[i] = m->nmaster;
		m->pertag->mfacts[i]   = m->mfact;

		m->pertag->ltidxs[(i) * 2 + (0)] = m->lt[0];
		m->pertag->ltidxs[(i) * 2 + (1)] = m->lt[1];
		m->pertag->sellts[i]             = m->sellt;

		m->pertag->showbars[i]     = m->showbar;
		m->pertag->drawwithgaps[i] = startwithgaps[0];
		m->pertag->gappx[i]        = gappx[0];
	}
	for (i = 0; i <= LENGTH(tags); i++) {
		m->pertag->drawwithgaps[i] = startwithgaps[0];
		m->pertag->gappx[i]        = gappx[0];
	}

	return m;
}

Monitor *
dirtomon(int dir)
{
	Monitor *m = NULL;

	if (dir > 0) {
		if (!(m = selmon->next))
			m = mons;
	} else if (selmon == mons)
		for (m = mons; m->next; m = m->next)
			;
	else
		for (m = mons; m->next != selmon; m = m->next)
			;
	return m;
}

void
drawbar(Monitor *m)
{
	int          x, w, tw = 0, stw = 0;
	int          boxs = drw->fonts->h / 9;
	int          boxw = drw->fonts->h / 6 + 2;
	unsigned int i, occ = 0, urg = 0, n = 0;
	Client      *c;

	if (!m->showbar)
		return;

	if (showsystray && m == systraytomon(m) && !systrayonleft)
		stw = getsystraywidth();

	/* draw status first so it can be overdrawn by tags later */
	if (m == selmon) { /* status is only drawn on selected monitor */
		drw_setscheme(drw, scheme[SchemeNorm]);
		tw = TEXTW(stext) - lrpad / 2 + 2; /* 2px extra right padding */
		drw_text(drw, m->ww - tw - stw, 0, tw, bh, lrpad / 2 - 2, stext, 0);
	}

	resizebarwin(m);
	for (c = m->cl->clients; c; c = c->next) {
		occ |= c->tags;
		if (c->isurgent)
			urg |= c->tags;
		/* Count all windows on current tags (visible and hidden) */
		if (c->tags & m->tagset[m->seltags])
			n++;
	}
	x = 0;
	for (i = 0; i < LENGTH(tags); i++) {
		/* Skip tags that are not selected and have no windows */
		if (!(m->tagset[m->seltags] & 1 << i) && !(occ & 1 << i))
			continue;

		w = TEXTW(tags[i]);
		drw_setscheme(drw,
		    scheme[m->tagset[m->seltags] & 1 << i ? SchemeSel : SchemeNorm]);
		drw_text(drw, x, 0, w, bh, lrpad / 2, tags[i], urg & 1 << i);
		if (occ & 1 << i)
			drw_rect(drw, x + boxs, boxs, boxw, boxw,
			    m == selmon && selmon->sel && selmon->sel->tags & 1 << i,
			    urg & 1 << i);
		x += w;
	}
	w = TEXTW(m->ltsymbol);
	drw_setscheme(drw, scheme[SchemeNorm]);
	x = drw_text(drw, x, 0, w, bh, lrpad / 2, m->ltsymbol, 0);

	/* Draw window titles with icons (awesomebar) */
	if ((w = m->ww - tw - stw - x) > bh && n > 0) {
		int remainder = w;
		int tabw      = remainder / n;

		for (c = m->cl->clients; c; c = c->next) {
			/* Show all windows on current tags (visible and hidden) */
			if (!(c->tags & m->tagset[m->seltags]))
				continue;

			if (remainder - tabw < lrpad / 2)
				tabw = remainder;

			/* Use different scheme for hidden windows */
			if (c->ishidden)
				drw_setscheme(drw, scheme[SchemeNorm]);
			else
				drw_setscheme(
				    drw, scheme[m->sel == c ? SchemeSel : SchemeNorm]);

			/* Draw icon if available */
			int textx = x;
			if (c->icon) {
				/* Draw background rectangle for icon area first to avoid
				 * garbage. Use invert=1 to use the background color (ColBg).
				 */
				drw_rect(drw, x, 0, iconsize + lrpad / 2, bh, 1, 1);

				/* Render icon using drw */
				drw_pic(drw, x + lrpad / 4, (bh - iconsize) / 2, iconsize,
				    iconsize, c->icon);
				textx = x + iconsize + lrpad / 2;
				drw_text(drw, textx, 0, tabw - (iconsize + lrpad / 2), bh, 0,
				    c->name, 0);
			} else {
				drw_text(drw, x, 0, tabw, bh, lrpad / 2, c->name, 0);
			}

			/* Draw rectangle indicator for hidden windows */
			if (c->ishidden)
				drw_rect(drw, textx + boxs, boxs, boxw, boxw, 0, 0);
			else if (c->isfloating)
				drw_rect(drw, textx + boxs, boxs, boxw, boxw, c->isfixed, 0);

			x += tabw;
			remainder -= tabw;
		}
	} else {
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_rect(drw, x, 0, w, bh, 1, 1);
	}
	drw_map(drw, m->barwin, 0, 0, m->ww - stw, bh);
}

void
drawbars(void)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		drawbar(m);
}

void
focusmon(const Arg *arg)
{
	Monitor *m;

	if (!mons->next)
		return;
	if ((m = dirtomon(arg->i)) == selmon)
		return;
	unfocus(selmon->sel, 0);
	selmon = m;
	focus(NULL);
	warp(selmon->sel);
}

void
monocle(Monitor *m)
{
	unsigned int n = 0;
	Client      *c;

	for (c = m->cl->clients; c; c = c->next)
		if (ISVISIBLE(c, m))
			n++;
	if (n > 0) /* override layout symbol */
		snprintf(m->ltsymbol, sizeof m->ltsymbol, "[%d]", n);
	for (c = m->cl->stack; c && (!ISVISIBLE(c, m) || c->isfloating);
	    c  = c->snext)
        ;
	if (c && !c->isfloating) {
		/* Use resizeclient() directly, bypassing applysizehints().
		 * resize() skips the XConfigureWindow call when the stored
		 * c->x/y/w/h already match the target — which happens when this
		 * window was previously shown in monocle and then hidden via
		 * XMoveWindow (which moves the window off-screen without updating
		 * c->x).  The window would stay off-screen. */
		compositor_set_hidden(c, 0);
		if (m->pertag->drawwithgaps[m->pertag->curtag]) {
			unsigned int gp = m->pertag->gappx[m->pertag->curtag];
			resizeclient(c, m->wx + gp, m->wy + gp, m->ww - 2 * gp - 2 * c->bw,
			    m->wh - 2 * gp - 2 * c->bw);
		} else {
			resizeclient(c, m->wx - c->bw, m->wy, m->ww, m->wh);
		}
		c = c->snext;
	}
	for (; c; c = c->snext)
		if (!c->isfloating && ISVISIBLE(c, m)) {
			compositor_set_hidden(c, 1);
			uint32_t xy[2] = { (uint32_t) (int32_t) (WIDTH(c) * -2),
				(uint32_t) (int32_t) c->y };
			xcb_configure_window(XGetXCBConnection(dpy), c->win,
			    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, xy);
		}
}

Monitor *
recttomon(int x, int y, int w, int h)
{
	Monitor *m, *r   = selmon;
	int      a, area = 0;

	for (m = mons; m; m = m->next)
		if ((a = INTERSECT(x, y, w, h, m)) > area) {
			area = a;
			r    = m;
		}
	return r;
}

void
resizebarwin(Monitor *m)
{
	unsigned int w = m->ww;
	if (showsystray && m == systraytomon(m) && !systrayonleft)
		w -= getsystraywidth();
	uint32_t xywh[4] = { (uint32_t) (int32_t) m->wx,
		(uint32_t) (int32_t) m->by, w, (uint32_t) bh };
	xcb_configure_window(XGetXCBConnection(dpy), m->barwin,
	    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
	        XCB_CONFIG_WINDOW_HEIGHT,
	    xywh);
}

void
restack(Monitor *m)
{
	Client           *c;
	xcb_connection_t *xc = XGetXCBConnection(dpy);

	drawbar(m);
	if (!m->sel)
		return;
	if (m->sel->isfloating || !m->lt[m->sellt]->arrange) {
		uint32_t stack = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(
		    xc, m->sel->win, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
	}
	if (m->lt[m->sellt]->arrange) {
		uint32_t sibling = (uint32_t) m->barwin;
		for (c = m->cl->stack; c; c = c->snext)
			if (!c->isfloating && ISVISIBLE(c, m)) {
				uint32_t vals[2] = { sibling, XCB_STACK_MODE_BELOW };
				xcb_configure_window(xc, c->win,
				    XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
				    vals);
				sibling = (uint32_t) c->win;
			}
	}
	if (m == selmon && (m->tagset[m->seltags] & m->sel->tags) &&
	    m->lt[m->sellt]->arrange != &monocle)
		warp(m->sel);
	/* Same EnterNotify drain as arrange() — see comment there. */
	{
		xcb_generic_event_t *xe;
		xcb_flush(xc);
		while ((xe = xcb_poll_for_event(xc))) {
			if ((xe->response_type & ~0x80) != XCB_ENTER_NOTIFY)
				XPutBackEvent(dpy, (XEvent *) (void *) xe); /* NOLINT */
			free(xe);
		}
	}
	updateclientlist(); /* Update stacking order */
}

void
tile(Monitor *m)
{
	unsigned int i, n, h, mw, my, ty;
	Client      *c;

	for (n = 0, c = nexttiled(m->cl->clients, m); c;
	    c = nexttiled(c->next, m), n++)
		;

	if (n == 0)
		return;

	if (m->pertag->drawwithgaps[m->pertag->curtag]) {
		if (n > m->nmaster)
			mw = m->nmaster ? m->ww * m->mfact : 0;
		else
			mw = m->ww - m->pertag->gappx[m->pertag->curtag];
		for (i = 0, my = ty = m->pertag->gappx[m->pertag->curtag],
		    c    = nexttiled(m->cl->clients, m);
		    c; c = nexttiled(c->next, m), i++)
			if (i < m->nmaster) {
				h = (m->wh - my) / (MIN(n, m->nmaster) - i) -
				    m->pertag->gappx[m->pertag->curtag];
				resize(c, m->wx + m->pertag->gappx[m->pertag->curtag],
				    m->wy + my,
				    mw - (2 * c->bw) - m->pertag->gappx[m->pertag->curtag],
				    h - (2 * c->bw), 0);
				if (my + HEIGHT(c) + m->pertag->gappx[m->pertag->curtag] <
				    m->wh)
					my += HEIGHT(c) + m->pertag->gappx[m->pertag->curtag];
			} else {
				h = (m->wh - ty) / (n - i) -
				    m->pertag->gappx[m->pertag->curtag];
				resize(c, m->wx + mw + m->pertag->gappx[m->pertag->curtag],
				    m->wy + ty,
				    m->ww - mw - (2 * c->bw) -
				        2 * m->pertag->gappx[m->pertag->curtag],
				    h - (2 * c->bw), 0);
				if (ty + HEIGHT(c) + m->pertag->gappx[m->pertag->curtag] <
				    m->wh)
					ty += HEIGHT(c) + m->pertag->gappx[m->pertag->curtag];
			}
	} else { /* draw with singularborders logic */
		if (n > m->nmaster)
			mw = m->nmaster ? m->ww * m->mfact : 0;
		else
			mw = m->ww;
		for (i = my = ty = 0, c = nexttiled(m->cl->clients, m); c;
		    c = nexttiled(c->next, m), i++)
			if (i < m->nmaster) {
				h = (m->wh - my) / (MIN(n, m->nmaster) - i);
				if (n == 1)
					resize(c, m->wx - c->bw, m->wy, m->ww, m->wh, False);
				else
					resize(c, m->wx - c->bw, m->wy + my, mw - c->bw, h - c->bw,
					    False);
				my += HEIGHT(c) - c->bw;
			} else {
				h = (m->wh - ty) / (n - i);
				resize(c, m->wx + mw - c->bw, m->wy + ty, m->ww - mw,
				    h - c->bw, False);
				ty += HEIGHT(c) - c->bw;
			}
	}
}

void
togglebar(const Arg *arg)
{
	selmon->showbar = selmon->pertag->showbars[selmon->pertag->curtag] =
	    !selmon->showbar;
	updatebarpos(selmon);
	resizebarwin(selmon);
	if (showsystray) {
		int32_t  newy;
		uint32_t y;
		if (!selmon->showbar)
			newy = -bh;
		else {
			newy = 0;
			if (!selmon->topbar)
				newy = selmon->mh - bh;
		}
		y = (uint32_t) newy;
		xcb_configure_window(
		    XGetXCBConnection(dpy), systray->win, XCB_CONFIG_WINDOW_Y, &y);
	}
	updateworkarea(selmon);
	arrange(selmon);
}

void
updatebars(void)
{
	unsigned int      w;
	Monitor          *m;
	xcb_connection_t *xc    = XGetXCBConnection(dpy);
	int               depth = DefaultDepth(dpy, screen);

	/* WM_CLASS value: "awm\0awm" (instance NUL class) */
	static const char wm_class[] = "awm\0awm";

	for (m = mons; m; m = m->next) {
		if (m->barwin)
			continue;
		w = m->ww;
		if (showsystray && m == systraytomon(m))
			w -= getsystraywidth();

#ifdef COMPOSITOR
		{
			uint32_t vals[3] = {
				(uint32_t) scheme[SchemeNorm][ColBg].pixel, /* back_pixel */
				1,                              /* override_redirect */
				ButtonPressMask | ExposureMask, /* event_mask */
			};
			m->barwin = xcb_generate_id(xc);
			xcb_create_window(xc, (uint8_t) depth, m->barwin, root,
			    (int16_t) m->wx, (int16_t) m->by, (uint16_t) w, (uint16_t) bh,
			    0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
			    XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT |
			        XCB_CW_EVENT_MASK,
			    vals);
		}
#else
		{
			uint32_t vals[3] = {
				XCB_BACK_PIXMAP_PARENT_RELATIVE, /* back_pixmap =
				                                    ParentRelative */
				1,                               /* override_redirect */
				ButtonPressMask | ExposureMask,  /* event_mask */
			};
			m->barwin = xcb_generate_id(xc);
			xcb_create_window(xc, (uint8_t) depth, m->barwin, root,
			    (int16_t) m->wx, (int16_t) m->by, (uint16_t) w, (uint16_t) bh,
			    0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
			    XCB_CW_BACK_PIXMAP | XCB_CW_OVERRIDE_REDIRECT |
			        XCB_CW_EVENT_MASK,
			    vals);
		}
#endif
		{
			uint32_t cur = (uint32_t) cursor[CurNormal]->cursor;
			xcb_change_window_attributes(xc, m->barwin, XCB_CW_CURSOR, &cur);
		}
		if (showsystray && m == systraytomon(m)) {
			uint32_t stack = XCB_STACK_MODE_ABOVE;
			xcb_map_window(xc, systray->win);
			xcb_configure_window(
			    xc, systray->win, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		}
		{
			uint32_t stack = XCB_STACK_MODE_ABOVE;
			xcb_map_window(xc, m->barwin);
			xcb_configure_window(
			    xc, m->barwin, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		}
		/* WM_CLASS: instance + class both "awm", separated by NUL */
		xcb_change_property(xc, XCB_PROP_MODE_REPLACE, m->barwin,
		    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof(wm_class), wm_class);
	}
}

void
updatebarpos(Monitor *m)
{
	m->wy = m->my;
	m->wh = m->mh;
	if (m->showbar) {
		m->wh -= bh;
		m->by = m->topbar ? m->wy : m->wy + m->wh;
		m->wy = m->topbar ? m->wy + bh : m->wy;
	} else
		m->by = -bh;
}

int
updategeom(void)
{
	int dirty = 0;

#ifdef XRANDR
	if (XRRQueryExtension(dpy, &randrbase, &rrerrbase)) {
		int                 i, j, n, nn;
		Client             *c;
		Monitor            *m;
		XRRScreenResources *sr;
		XRRCrtcInfo        *ci;
		typedef struct {
			int x, y, w, h;
		} ScreenGeom;
		ScreenGeom *unique = NULL;

		sr = XRRGetScreenResources(dpy, root);
		if (!sr)
			goto xinerama_fallback;

		nn     = 0;
		unique = ecalloc(sr->ncrtc, sizeof(ScreenGeom));
		/* Get active CRTC geometries */
		for (i = 0; i < sr->ncrtc; i++) {
			ci = XRRGetCrtcInfo(dpy, sr, sr->crtcs[i]);
			if (!ci || ci->noutput == 0) {
				if (ci)
					XRRFreeCrtcInfo(ci);
				continue;
			}
			/* Check if geometry is unique */
			int is_unique = 1;
			for (j = 0; j < nn; j++) {
				if (unique[j].x == ci->x && unique[j].y == ci->y &&
				    unique[j].w == (int) ci->width &&
				    unique[j].h == (int) ci->height) {
					is_unique = 0;
					break;
				}
			}
			if (is_unique) {
				unique[nn].x = ci->x;
				unique[nn].y = ci->y;
				unique[nn].w = ci->width;
				unique[nn].h = ci->height;
				nn++;
			}
			XRRFreeCrtcInfo(ci);
		}
		XRRFreeScreenResources(sr);

		for (n = 0, m = mons; m; m = m->next, n++)
			;

		/* Create new monitors if nn > n */
		for (i = n; i < nn; i++) {
			for (m = mons; m && m->next; m = m->next)
				;
			if (m)
				m->next = createmon();
			else
				mons = createmon();
		}

		/* Update monitor geometries */
		for (i = 0, m = mons; i < nn && m; m = m->next, i++) {
			if (i >= n || unique[i].x != m->mx || unique[i].y != m->my ||
			    unique[i].w != m->mw || unique[i].h != m->mh) {
				dirty  = 1;
				m->num = i;
				m->mx = m->wx = unique[i].x;
				m->my = m->wy = unique[i].y;
				m->mw = m->ww = unique[i].w;
				m->mh = m->wh = unique[i].h;
				updatebarpos(m);
			}
		}

		/* Remove monitors if n > nn */
		for (i = nn; i < n; i++) {
			for (m = mons; m && m->next; m = m->next)
				;
			if (m == selmon)
				selmon = mons;
			for (c = m->cl->clients; c; c = c->next) {
				dirty = 1;
				if (c->mon == m)
					c->mon = selmon;
			}
			cleanupmon(m);
		}
		free(unique);
	} else
	xinerama_fallback:
#endif /* XRANDR */
#ifdef XINERAMA
		if (XineramaIsActive(dpy)) {
			int                 i, j, n, nn;
			Client             *c;
			Monitor            *m;
			XineramaScreenInfo *info   = XineramaQueryScreens(dpy, &nn);
			XineramaScreenInfo *unique = NULL;

			if (!info)
				goto default_monitor;

			for (n = 0, m = mons; m; m = m->next, n++)
				;
			/* only consider unique geometries as separate screens */
			unique = ecalloc(nn, sizeof(XineramaScreenInfo));
			for (i = 0, j = 0; i < nn; i++)
				if (isuniquegeom(unique, j, &info[i]))
					memcpy(&unique[j++], &info[i], sizeof(XineramaScreenInfo));
			XFree(info);
			nn = j;

			/* new monitors if nn > n */
			for (i = n; i < nn; i++) {
				for (m = mons; m && m->next; m = m->next)
					;
				if (m)
					m->next = createmon();
				else
					mons = createmon();
			}
			for (i = 0, m = mons; i < nn && m; m = m->next, i++)
				if (i >= n || unique[i].x_org != m->mx ||
				    unique[i].y_org != m->my || unique[i].width != m->mw ||
				    unique[i].height != m->mh) {
					dirty  = 1;
					m->num = i;
					m->mx = m->wx = unique[i].x_org;
					m->my = m->wy = unique[i].y_org;
					m->mw = m->ww = unique[i].width;
					m->mh = m->wh = unique[i].height;
					updatebarpos(m);
				}
			/* removed monitors if n > nn */
			for (i = nn; i < n; i++) {
				for (m = mons; m && m->next; m = m->next)
					;
				if (m == selmon)
					selmon = mons;
				for (c = m->cl->clients; c; c = c->next) {
					dirty = True;
					if (c->mon == m)
						c->mon = selmon;
				}
				cleanupmon(m);
			}
			free(unique);
		} else
		default_monitor:
#endif    /* XINERAMA */
		{ /* default monitor setup */
			if (!mons)
				mons = createmon();
			if (mons->mw != sw || mons->mh != sh) {
				dirty    = 1;
				mons->mw = mons->ww = sw;
				mons->mh = mons->wh = sh;
				updatebarpos(mons);
			}
		}
			if (dirty)
				selmon = wintomon(root);
	return dirty;
}

void
updatestatus(void)
{
	if (stext[0] == '\0')
		snprintf(stext, sizeof(stext), "awm-" VERSION);
	drawbar(selmon);
	updatesystray();
}

Monitor *
wintomon(Window w)
{
	int      x, y;
	Client  *c;
	Monitor *m;

	if (w == root && getrootptr(&x, &y))
		return recttomon(x, y, 1, 1);
	for (m = mons; m; m = m->next)
		if (w == m->barwin)
			return m;
	if ((c = wintoclient(w)))
		return c->mon;
	return selmon;
}

Monitor *
systraytomon(Monitor *m)
{
	Monitor *t;
	int      i, n;
	if (!systraypinning) {
		if (!m)
			return selmon;
		return m == selmon ? m : NULL;
	}
	for (n = 1, t = mons; t && t->next; n++, t = t->next)
		;
	for (i = 1, t = mons; t && t->next && i < systraypinning; i++, t = t->next)
		;
	if (systraypinningfailfirst && n < systraypinning)
		return mons;
	return t;
}
