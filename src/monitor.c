/* AndrathWM - monitor management
 * See LICENSE file for copyright and license details. */

#include <assert.h>
#include "monitor.h"
#include "awm.h"
#include "client.h"
#include "ewmh.h"
#include "spawn.h"
#include "status.h"
#include "systray.h"
#include "xrdb.h"
#ifdef COMPOSITOR
#include "compositor.h"
#endif
#include "wmstate.h"
#include "config.h"

#ifdef XINERAMA
/* isuniquegeom moved to platform_x11.c */
#endif /* XINERAMA */

void
arrange(Monitor *m)
{
	Monitor *om;

	if (m)
		showhide(g_awm.stack_head);
	else
		FOR_EACH_MON(om)
	showhide(g_awm.stack_head);
	if (m) {
		arrangemon(m);
		/* showhide() walks the shared client stack and calls
		 * compositor_set_hidden(c, 0) for every client that is
		 * ISVISIBLE on its own monitor — including clients that are
		 * monocle-hidden on OTHER monitors.  Re-run arrangemon on
		 * every other monitor that uses a layout so those monitors
		 * can re-apply their hidden state (e.g. monocle re-hides
		 * the non-top windows that showhide just un-hid). */
		FOR_EACH_MON(om)
		{
			if (om == m)
				continue;
			if (om->lt[om->sellt]->arrange)
				om->lt[om->sellt]->arrange(om);
		}
		restack(m);
	} else {
		FOR_EACH_MON(om)
		arrangemon(om);
		/* Flush all pending requests and discard stale EnterNotify
		 * events so we don't spuriously change focus after a
		 * layout change.  Non-EnterNotify events are dispatched
		 * through the normal handler, and all events are also fed
		 * to the compositor so it keeps its window list in sync
		 * (ConfigureNotify, MapNotify, DamageNotify, etc.). */
		{
			xcb_generic_event_t *xe;
			g_wm_backend->flush(&g_plat);
			while ((xe = g_wm_backend->poll_event(&g_plat))) {
				uint8_t type = xe->response_type & ~0x80;
#ifdef COMPOSITOR
				if (xe->response_type != 0)
					compositor_handle_event(xe);
#endif
				if (type != XCB_ENTER_NOTIFY && type < LASTEvent &&
				    handler[type])
					handler[type](xe);
				free(xe);
			}
		}
	}
}

void
arrangemon(Monitor *m)
{
	assert(m != NULL);
	assert(m->seltags == 0 || m->seltags == 1);
	assert(m->sellt == 0 || m->sellt == 1);
	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, sizeof m->ltsymbol);
	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
}

void
cleanupmon(Monitor *mon)
{
	Client *c;
	int     idx, i;

	assert(mon != NULL);
	assert(
	    mon >= g_awm.monitors && mon < g_awm.monitors + WMSTATE_MAX_MONITORS);

	/* Find index of the monitor being removed. */
	idx = (int) (mon - g_awm.monitors);

	g_wm_backend->unmap(&g_plat, mon->barwin);
	g_wm_backend->destroy_win(&g_plat, mon->barwin);

	/* Compact the array: shift entries left over the removed slot. */
	for (i = idx; i < (int) g_awm.n_monitors - 1; i++)
		g_awm.monitors[i] = g_awm.monitors[i + 1];
	g_awm.n_monitors--;

	/* Patch selmon_num: if it pointed past the removed slot, decrement. */
	if (g_awm.selmon_num > idx)
		g_awm.selmon_num--;
	else if (g_awm.selmon_num == idx)
		g_awm.selmon_num = 0;

	/* Patch c->mon pointers: clients whose mon pointed past the removed
	 * slot now have a stale pointer — fix by re-pointing into the array.
	 * Clients on the removed monitor have already been redirected by the
	 * caller (updategeom) before cleanupmon() was invoked. */
	for (c = g_awm.clients_head; c; c = c->next) {
		int cm = (int) (c->mon - g_awm.monitors);
		if (cm > idx)
			c->mon = &g_awm.monitors[cm - 1];
	}
}

Monitor *
createmon(void)
{
	Monitor     *m, *tm;
	unsigned int i;

	/* Bail if the monitor array is full. */
	if (g_awm.n_monitors >= WMSTATE_MAX_MONITORS) {
		awm_error("failed to add monitor: WMSTATE_MAX_MONITORS exceeded");
		return NULL;
	}
	/* Bail if the number of monitors would exceed the number of tags. */
	if (g_awm.n_monitors >= LENGTH(tags)) {
		awm_error("failed to add monitor, number of tags exceeded");
		return NULL;
	}
	/* Find the first tag that is not already in use by any monitor. */
	for (i = 0; i < LENGTH(tags); i++) {
		int used = 0;
		FOR_EACH_MON(tm)
		if (tm->tagset[tm->seltags] & (1 << i)) {
			used = 1;
			break;
		}
		if (!used)
			break;
	}
	/* If no free tag, reassign all tags to monitors sequentially. */
	if (i >= LENGTH(tags)) {
		FOR_EACH_MON(tm)
		{
			int j = (int) (tm - g_awm.monitors);
			tm->seltags ^= 1;
			tm->tagset[tm->seltags] = (1 << j) & TAGMASK;
		}
		i = g_awm.n_monitors;
	}
	/* Initialise the new slot in place. */
	m = &g_awm.monitors[g_awm.n_monitors];
	memset(m, 0, sizeof *m);
	m->tagset[0] = m->tagset[1] = (1 << i) & TAGMASK;
	m->mfact                    = mfact;
	m->nmaster                  = nmaster;
	m->showbar                  = showbar;
	m->topbar                   = topbar;
	m->lt[0]                    = &layouts[0];
	m->lt[1]                    = &layouts[1 % LENGTH(layouts)];
	strncpy(m->ltsymbol, layouts[0].symbol, sizeof m->ltsymbol);
	m->pertag.curtag = m->pertag.prevtag = 1;

	for (i = 0; i <= LENGTH(tags); i++) {
		m->pertag.nmasters[i] = m->nmaster;
		m->pertag.mfacts[i]   = m->mfact;

		m->pertag.ltidxs[(i) * 2 + (0)] = m->lt[0];
		m->pertag.ltidxs[(i) * 2 + (1)] = m->lt[1];
		m->pertag.sellts[i]             = m->sellt;

		m->pertag.showbars[i]     = m->showbar;
		m->pertag.drawwithgaps[i] = startwithgaps[0];
		m->pertag.gappx[i]        = g_plat.ui_gappx;
	}

	g_awm.n_monitors++;
	return m;
}

Monitor *
dirtomon(int dir)
{
	int idx;

	if (g_awm.n_monitors <= 1)
		return g_awm_selmon;
	idx = g_awm.selmon_num;
	if (dir > 0)
		idx = (idx + 1) % (int) g_awm.n_monitors;
	else
		idx = (idx - 1 + (int) g_awm.n_monitors) % (int) g_awm.n_monitors;
	return &g_awm.monitors[idx];
}

void
drawbar(Monitor *m)
{
	int          x, w, tw = 0, stw = 0;
	int          boxs;
	int          boxw;
	unsigned int i, occ = 0, urg = 0, n = 0;
	Client      *c;

	assert(m != NULL);
	assert(drw != NULL);
	assert(drw->fonts != NULL);
	boxs = drw->fonts->h / 9;
	boxw = drw->fonts->h / 6 + 2;

	if (!m->showbar)
		return;

	if (showsystray && m == systraytomon(m) && !systrayonleft)
		stw = getsystraywidth();

	/* draw status first so it can be overdrawn by tags later */
	if (m == g_awm_selmon) { /* status is only drawn on selected monitor */
		/* Measure the status2d string first (measurement-only: all zeros),
		 * then render it right-aligned against the bar edge.
		 * tw is used below to size the title-tab area.                   */
		drw_setscheme(drw, scheme[SchemeNorm]);
		tw = drw_draw_statusd(drw, 0, 0, 0, 0, stext);
		/* Pre-clear the status region with SchemeNorm bg so there is no
		 * stale content visible between or around the s2d widgets.      */
		drw_rect(drw, m->ww - stw - tw, 0, (unsigned int) tw,
		    (unsigned int) g_plat.bh, 1, 1);
		drw_draw_statusd(drw, m->ww - stw - tw, 0, (unsigned int) tw,
		    (unsigned int) g_plat.bh, stext);
	}

	resizebarwin(m);
	for (c = g_awm.clients_head; c; c = c->next) {
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
		drw_text(
		    drw, x, 0, w, g_plat.bh, g_plat.lrpad / 2, tags[i], urg & 1 << i);
		if (occ & 1 << i)
			drw_rect(drw, x + boxs, boxs, boxw, boxw,
			    m == g_awm_selmon && g_awm_selmon->sel &&
			        g_awm_selmon->sel->tags & 1 << i,
			    urg & 1 << i);
		x += w;
	}
	w = TEXTW(m->ltsymbol);
	drw_setscheme(drw, scheme[SchemeNorm]);
	x = drw_text(drw, x, 0, w, g_plat.bh, g_plat.lrpad / 2, m->ltsymbol, 0);

	/* Draw window titles with icons (awesomebar) */
	if ((w = m->ww - tw - stw - x) > g_plat.bh && n > 0) {
		int remainder = w;
		int tabw      = remainder / n;

		for (c = g_awm.clients_head; c; c = c->next) {
			/* Show all windows on current tags (visible and hidden) */
			if (!(c->tags & m->tagset[m->seltags]))
				continue;

			if (remainder - tabw < g_plat.lrpad / 2)
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
				drw_rect(drw, x, 0,
				    (int) g_plat.ui_iconsize + g_plat.lrpad / 2, g_plat.bh, 1,
				    1);

				/* Render icon using drw */
				drw_pic(drw, x + g_plat.lrpad / 4,
				    (g_plat.bh - (int) g_plat.ui_iconsize) / 2,
				    (int) g_plat.ui_iconsize, (int) g_plat.ui_iconsize,
				    c->icon);
				textx = x + (int) g_plat.ui_iconsize + g_plat.lrpad / 2;
				drw_text(drw, textx, 0,
				    tabw - ((int) g_plat.ui_iconsize + g_plat.lrpad / 2),
				    g_plat.bh, 0, c->name, 0);
			} else {
				drw_text(
				    drw, x, 0, tabw, g_plat.bh, g_plat.lrpad / 2, c->name, 0);
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
		drw_rect(drw, x, 0, w, g_plat.bh, 1, 1);
	}
	drw_map(drw, m->barwin, 0, 0, m->ww - stw, g_plat.bh);
}

void
drawbars(void)
{
	Monitor *m;

	FOR_EACH_MON(m)
	drawbar(m);
}

void
focusmon(const Arg *arg)
{
	Monitor *m;

	if (g_awm.n_monitors <= 1)
		return;
	if ((m = dirtomon(arg->i)) == g_awm_selmon)
		return;
	unfocus(g_awm_selmon->sel, 0);
	g_awm_set_selmon(m);
	focus(NULL);
	warp(g_awm_selmon->sel);
	wmstate_update();
}

void
monocle(Monitor *m)
{
	unsigned int n = 0;
	Client      *c;

	for (c = g_awm.clients_head; c; c = c->next)
		if (ISVISIBLE(c, m))
			n++;
	if (n > 0) /* override layout symbol */
		snprintf(m->ltsymbol, sizeof m->ltsymbol, "[%d]", n);
	for (c = g_awm.stack_head; c && (!ISVISIBLE(c, m) || c->isfloating);
	     c = c->snext)
		;
	if (c && !c->isfloating) {
		/* Use resizeclient() directly, bypassing applysizehints().
		 * resize() skips the XConfigureWindow call when the stored
		 * c->x/y/w/h already match the target — which happens when this
		 * window was previously shown in monocle and then hidden via
		 * XMoveWindow (which moves the window off-screen without updating
		 * c->x).  The window would stay off-screen. */
		compositor_set_hidden(c, 0);
		if (m->pertag.drawwithgaps[m->pertag.curtag]) {
			unsigned int gp = m->pertag.gappx[m->pertag.curtag];
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
			g_wm_backend->configure_win(&g_plat, c->win,
			    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, xy);
		}
}

Monitor *
recttomon(int x, int y, int w, int h)
{
	Monitor *m, *r   = g_awm_selmon;
	int      a, area = 0;

	FOR_EACH_MON(m)
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
		(uint32_t) (int32_t) m->by, w, (uint32_t) g_plat.bh };
	g_wm_backend->configure_win(&g_plat, m->barwin,
	    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
	        XCB_CONFIG_WINDOW_HEIGHT,
	    xywh);
}

void
restack(Monitor *m)
{
	Client *c;

	drawbar(m);
	if (!m->sel)
		return;
	if (m->sel->isfloating || !m->lt[m->sellt]->arrange) {
		uint32_t stack = XCB_STACK_MODE_ABOVE;
		g_wm_backend->configure_win(
		    &g_plat, m->sel->win, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
	}
	if (m->lt[m->sellt]->arrange) {
		uint32_t sibling = (uint32_t) m->barwin;
		for (c = g_awm.stack_head; c; c = c->snext)
			if (!c->isfloating && ISVISIBLE(c, m)) {
				uint32_t vals[2] = { sibling, XCB_STACK_MODE_BELOW };
				g_wm_backend->configure_win(&g_plat, c->win,
				    XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
				    vals);
				sibling = (uint32_t) c->win;
			}
	}
	if (m == g_awm_selmon && (m->tagset[m->seltags] & m->sel->tags) &&
	    m->lt[m->sellt]->arrange != &monocle)
		warp(m->sel);
	/* Same EnterNotify drain as arrange() — see comment there. */
	{
		xcb_generic_event_t *xe;
		g_wm_backend->flush(&g_plat);
		while ((xe = g_wm_backend->poll_event(&g_plat))) {
			uint8_t type = xe->response_type & ~0x80;
#ifdef COMPOSITOR
			if (xe->response_type != 0)
				compositor_handle_event(xe);
#endif
			if (type != XCB_ENTER_NOTIFY && type < LASTEvent && handler[type])
				handler[type](xe);
			free(xe);
		}
	}
	/* After stacking client windows, ensure the compositor overlay remains
	 * on top.  xcb_configure_window(STACK_MODE_ABOVE) on client windows can
	 * push them above the overlay, making the compositor paint under them. */
#ifdef COMPOSITOR
	compositor_raise_overlay();
#endif
	updateclientlist(); /* Update stacking order */
}

void
tile(Monitor *m)
{
	unsigned int i, n, h, mw, my, ty;
	Client      *c;

	assert(m != NULL);

	/* Pass 1: count tiled windows.  The total is needed up-front so that the
	 * per-slot height formula  h = (wh - my) / (MIN(n, nmaster) - i)  can
	 * distribute remaining space evenly.  Both passes are O(n) — nexttiled
	 * advances one window at a time, not from the head each iteration. */
	for (n = 0, c = nexttiled(g_awm.clients_head, m); c;
	     c = nexttiled(c->next, m), n++)
		;

	if (n == 0)
		return;

	if (m->pertag.drawwithgaps[m->pertag.curtag]) {
		if (n > m->nmaster)
			mw = m->nmaster ? m->ww * m->mfact : 0;
		else
			mw = m->ww - m->pertag.gappx[m->pertag.curtag];
		for (i = 0, my = ty = m->pertag.gappx[m->pertag.curtag],
		    c     = nexttiled(g_awm.clients_head, m);
		     c; c = nexttiled(c->next, m), i++)
			if (i < m->nmaster) {
				h = (m->wh - my) / (MIN(n, m->nmaster) - i) -
				    m->pertag.gappx[m->pertag.curtag];
				resize(c, m->wx + m->pertag.gappx[m->pertag.curtag],
				    m->wy + my,
				    mw - (2 * c->bw) - m->pertag.gappx[m->pertag.curtag],
				    h - (2 * c->bw), 0);
				/* Advance by the ideal slot height (h + 2*bw), not the
				 * hint-snapped HEIGHT(c).  If we used HEIGHT(c) and
				 * applysizehints snapped the window smaller, the remaining
				 * space would be divided as if less was consumed, causing
				 * gaps to accumulate at the bottom of the column. */
				if (my + h + 2 * c->bw + m->pertag.gappx[m->pertag.curtag] <
				    m->wh)
					my += h + 2 * c->bw + m->pertag.gappx[m->pertag.curtag];
			} else {
				h = (m->wh - ty) / (n - i) - m->pertag.gappx[m->pertag.curtag];
				resize(c, m->wx + mw + m->pertag.gappx[m->pertag.curtag],
				    m->wy + ty,
				    m->ww - mw - (2 * c->bw) -
				        2 * m->pertag.gappx[m->pertag.curtag],
				    h - (2 * c->bw), 0);
				/* Same: advance by ideal slot height, not snapped HEIGHT(c).
				 */
				if (ty + h + 2 * c->bw + m->pertag.gappx[m->pertag.curtag] <
				    m->wh)
					ty += h + 2 * c->bw + m->pertag.gappx[m->pertag.curtag];
			}
	} else { /* draw with singularborders logic */
		if (n > m->nmaster)
			mw = m->nmaster ? m->ww * m->mfact : 0;
		else
			mw = m->ww;
		for (i = my = ty = 0, c = nexttiled(g_awm.clients_head, m); c;
		     c = nexttiled(c->next, m), i++)
			if (i < m->nmaster) {
				h = (m->wh - my) / (MIN(n, m->nmaster) - i);
				if (n == 1)
					resize(c, m->wx - c->bw, m->wy, m->ww, m->wh, 0);
				else
					resize(c, m->wx - c->bw, m->wy + my, mw - c->bw, h - c->bw,
					    0);
				my += h - c->bw; /* ideal slot, not snapped HEIGHT(c) */
			} else {
				h = (m->wh - ty) / (n - i);
				resize(c, m->wx + mw - c->bw, m->wy + ty, m->ww - mw,
				    h - c->bw, 0);
				ty += h - c->bw; /* ideal slot, not snapped HEIGHT(c) */
			}
	}
}

void
togglebar(const Arg *arg)
{
	g_awm_selmon->showbar =
	    g_awm_selmon->pertag.showbars[g_awm_selmon->pertag.curtag] =
	        !g_awm_selmon->showbar;
	updatebarpos(g_awm_selmon);
	resizebarwin(g_awm_selmon);
	if (showsystray) {
		int32_t  newy;
		uint32_t y;
		if (!g_awm_selmon->showbar)
			newy = -g_plat.bh;
		else {
			newy = 0;
			if (!g_awm_selmon->topbar)
				newy = g_awm_selmon->mh - g_plat.bh;
		}
		y = (uint32_t) newy;
		g_wm_backend->configure_win(
		    &g_plat, systray->win, XCB_CONFIG_WINDOW_Y, &y);
	}
	updateworkarea(g_awm_selmon);
	arrange(g_awm_selmon);
	wmstate_update();
}

void
updatebars(void)
{
	unsigned int w;
	Monitor     *m;

	/* WM_CLASS value: "awm\0awm" (instance NUL class) */
	static const char wm_class[] = "awm\0awm";

	FOR_EACH_MON(m)
	{
		if (m->barwin)
			continue;
		w = m->ww;
		if (showsystray && m == systraytomon(m))
			w -= getsystraywidth();

		m->barwin = g_wm_backend->create_bar_win(
		    &g_plat, m->wx, m->by, (int) w, g_plat.bh, COMPOSITOR_ACTIVE);
		{
			uint32_t cur = (uint32_t) cursor[CurNormal]->cursor;
			g_wm_backend->change_attr(&g_plat, m->barwin, XCB_CW_CURSOR, &cur);
		}
		if (showsystray && m == systraytomon(m)) {
			uint32_t stack = XCB_STACK_MODE_ABOVE;
			g_wm_backend->map(&g_plat, systray->win);
			g_wm_backend->configure_win(
			    &g_plat, systray->win, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		}
		{
			uint32_t stack = XCB_STACK_MODE_ABOVE;
			g_wm_backend->map(&g_plat, m->barwin);
			g_wm_backend->configure_win(
			    &g_plat, m->barwin, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		}
		/* WM_CLASS: instance + class both "awm", separated by NUL */
		g_wm_backend->change_prop(&g_plat, m->barwin, XCB_ATOM_WM_CLASS,
		    XCB_ATOM_STRING, 8, XCB_PROP_MODE_REPLACE, sizeof(wm_class),
		    wm_class);
	}
}

void
updatebarpos(Monitor *m)
{
	m->wy = m->my;
	m->wh = m->mh;
	if (m->showbar) {
		m->wh -= g_plat.bh;
		m->by = m->topbar ? m->wy : m->wy + m->wh;
		m->wy = m->topbar ? m->wy + g_plat.bh : m->wy;
	} else
		m->by = -g_plat.bh;
}

int
updategeom(void)
{
	return g_wm_backend->update_geom(&g_plat);
}

void
updatestatus(void)
{
	if (stext[0] == '\0')
		snprintf(stext, sizeof(stext), "awm-" VERSION);
	drawbar(g_awm_selmon);
	updatesystray();
	wmstate_update();
}

Monitor *
wintomon(xcb_window_t w)
{
	int      x, y;
	Client  *c;
	Monitor *m;

	if (w == g_plat.root && getrootptr(&x, &y))
		return recttomon(x, y, 1, 1);
	FOR_EACH_MON(m)
	if (w == m->barwin)
		return m;
	if ((c = wintoclient(w)))
		return c->mon;
	return g_awm_selmon;
}

Monitor *
systraytomon(Monitor *m)
{
	Monitor *t;
	int      i;

	if (!systraypinning) {
		if (!m)
			return g_awm_selmon;
		return m == g_awm_selmon ? m : NULL;
	}
	/* Pin to the n-th monitor (1-indexed), clamping to last if out of range.
	 */
	i = 1;
	FOR_EACH_MON(t)
	{
		if (i >= systraypinning)
			break;
		i++;
	}
	if (systraypinningfailfirst && (int) g_awm.n_monitors < systraypinning)
		return &g_awm.monitors[0];
	return t;
}
