/* AndrathWM - X event handlers
 * See LICENSE file for copyright and license details. */

#include "events.h"
#include "awm.h"
#include <X11/XKBlib.h>
#include "client.h"
#include "compositor.h"
#include "ewmh.h"
#include "monitor.h"
#include "spawn.h"
#include "systray.h"
#include "xrdb.h"
#include "config.h"

void
buttonpress(xcb_generic_event_t *e)
{
	unsigned int              i, x, click;
	Arg                       arg = { 0 };
	Client                   *c;
	Monitor                  *m;
	xcb_button_press_event_t *ev = (xcb_button_press_event_t *) e;

	click = ClkRootWin;
	/* focus monitor if necessary */
	if ((m = wintomon(ev->event)) && m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	if (ev->event == selmon->barwin) {
		i = x = 0;
		/* Calculate x position after tags (accounting for hidden empty tags)
		 */
		unsigned int occ = 0;
		for (Client *tc = m->cl->clients; tc; tc = tc->next) {
			occ |= tc->tags;
		}

		/* Find which tag was clicked */
		for (i = 0; i < LENGTH(tags); i++) {
			/* Skip tags that are not selected and have no windows */
			if (!(m->tagset[m->seltags] & 1 << i) && !(occ & 1 << i))
				continue;

			int tw = TEXTW(tags[i]);
			if (ev->event_x < x + tw) {
				click  = ClkTagBar;
				arg.ui = 1 << i;
				break;
			}
			x += tw;
		}

		if (i >= LENGTH(tags) && ev->event_x < x + TEXTW(selmon->ltsymbol))
			click = ClkLtSymbol;
		else if (ev->event_x >
		    selmon->ww - (int) TEXTW(stext) - getsystraywidth())
			click = ClkStatusText;
		else if (i >= LENGTH(tags)) {
			/* Awesomebar - find which window was clicked */
			click = ClkWinTitle;
			c     = NULL;

			/* Add layout symbol width to x position */
			x += TEXTW(selmon->ltsymbol);

			int n = 0;
			for (Client *t = m->cl->clients; t; t = t->next)
				if (t->tags & m->tagset[m->seltags])
					n++;

			if (n > 0) {
				int tw        = TEXTW(stext);
				int stw       = getsystraywidth();
				int remainder = m->ww - tw - stw - x;
				int tabw      = remainder / n;
				int cx        = x;

				for (Client *t = m->cl->clients; t; t = t->next) {
					if (!(t->tags & m->tagset[m->seltags]))
						continue;
					if (ev->event_x >= cx && ev->event_x < cx + tabw) {
						c = t;
						break;
					}
					cx += tabw;
				}
			}

			if (c)
				arg.v = c;
		}
	} else if ((c = wintoclient(ev->event))) {
		focus(c);
		restack(selmon);
		xcb_allow_events(xc, XCB_ALLOW_REPLAY_POINTER,
		    XCB_CURRENT_TIME);
		click = ClkClientWin;
	}
#ifdef STATUSNOTIFIER
	/* Check if click is on SNI icon */
	else {
		SNIItem *sni_item = sni_find_item_by_window(ev->event);
		if (sni_item) {
			sni_handle_click(
			    ev->event, ev->detail, ev->root_x, ev->root_y, ev->time);
			return; /* Don't process further */
		}
	}
#endif
	for (i = 0; i < LENGTH(buttons); i++)
		if (click == buttons[i].click && buttons[i].func &&
		    buttons[i].button == ev->detail &&
		    CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state))
			buttons[i].func((click == ClkTagBar && buttons[i].arg.i == 0) ||
			            click == ClkWinTitle
			        ? &arg
			        : &buttons[i].arg);
}

void
checkotherwm(void)
{
	uint32_t             mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;
	xcb_void_cookie_t    ck;
	xcb_generic_error_t *err;

	/* Probe for another WM by requesting SubstructureRedirect on root.
	 * Only one client may hold this mask; xcb_request_check returns an
	 * error synchronously if another WM is already running. */
	ck = xcb_change_window_attributes_checked(
	    xc, root, XCB_CW_EVENT_MASK, &mask);
	err = xcb_request_check(xc, ck);
	if (err) {
		free(err);
		die("awm: another window manager is already running");
	}
	xerrorxlib = XSetErrorHandler(xerror);
}

void
clientmessage(xcb_generic_event_t *e)
{
	xcb_client_message_event_t *cme = (xcb_client_message_event_t *) e;
	Client                     *c   = wintoclient(cme->window);
	unsigned int                i;

	if (showsystray && cme->window == systray->win &&
	    cme->type == netatom[NetSystemTrayOP]) {
		/* add systray icons */
		if (cme->data.data32[1] == SYSTEM_TRAY_REQUEST_DOCK) {
			if (!(c = (Client *) calloc(1, sizeof(Client))))
				die("fatal: could not malloc() %u bytes\n", sizeof(Client));
			if (!(c->win = cme->data.data32[2])) {
				free(c);
				return;
			}
			c->mon         = selmon;
			c->next        = systray->icons;
			systray->icons = c;
			{
				xcb_get_geometry_cookie_t ck =
				    xcb_get_geometry(xc, c->win);
				xcb_get_geometry_reply_t *gr =
				    xcb_get_geometry_reply(xc, ck, NULL);
				if (gr) {
					c->w = c->oldw = gr->width;
					c->h = c->oldh = gr->height;
					c->oldbw       = gr->border_width;
					free(gr);
				} else {
					c->w = c->oldw = bh;
					c->h = c->oldh = bh;
					c->oldbw       = 0;
				}
			}
			c->x = c->oldx = c->y = c->oldy = 0;
			c->bw                           = 0;
			c->isfloating                   = True;
			/* reuse tags field as mapped status */
			c->tags = 1;
			updatesizehints(c);
			updatesystrayicongeom(c, c->w, c->h);
			xcb_change_save_set(
			    xc, XCB_SET_MODE_INSERT, c->win);
			{
				uint32_t mask = StructureNotifyMask | PropertyChangeMask |
				    ResizeRedirectMask;
				xcb_change_window_attributes(
				    xc, c->win, XCB_CW_EVENT_MASK, &mask);
			}
			xcb_reparent_window(
			    xc, c->win, systray->win, 0, 0);
			/* use bar background so icon blends with the bar */
			{
				uint32_t bg = clr_to_argb(&scheme[SchemeNorm][ColBg]);
				xcb_change_window_attributes(
				    xc, c->win, XCB_CW_BACK_PIXEL, &bg);
			}
			/* Send XEMBED_EMBEDDED_NOTIFY to complete embedding per spec.
			 * data1 = embedder window, data2 = protocol version */
			sendevent(c->win, netatom[Xembed], StructureNotifyMask,
			    CurrentTime, XEMBED_EMBEDDED_NOTIFY, 0, systray->win,
			    XEMBED_VERSION);
			xflush();
			resizebarwin(selmon);
			updatesystray();
			setclientstate(c, NormalState);
		}
		return;
	}

	if (!c)
		return;
	if (cme->type == netatom[NetWMState]) {
		if (cme->data.data32[1] == netatom[NetWMFullscreen] ||
		    cme->data.data32[2] == netatom[NetWMFullscreen])
			setfullscreen(c,
			    (cme->data.data32[0] == 1 /* _NET_WM_STATE_ADD    */
			        || (cme->data.data32[0] == 2 /* _NET_WM_STATE_TOGGLE */ &&
			               !c->isfullscreen)));
	} else if (cme->type == netatom[NetActiveWindow]) {
		for (i = 0; i < LENGTH(tags) && !((1 << i) & c->tags); i++)
			;
		if (i < LENGTH(tags)) {
			const Arg a = { .ui = 1 << i };
			selmon      = c->mon;
			view(&a);
			focus(c);
			restack(selmon);
		}
	} else if (cme->type == netatom[NetCloseWindow]) {
		/* _NET_CLOSE_WINDOW client message */
		if (!sendevent(c->win, wmatom[WMDelete], NoEventMask, wmatom[WMDelete],
		        CurrentTime, 0, 0, 0)) {
			
			xcb_grab_server(xc);
			xcb_set_close_down_mode(xc, XCB_CLOSE_DOWN_DESTROY_ALL);
			xcb_kill_client(xc, c->win);
			xcb_ungrab_server(xc);
			xflush();
		}
	} else if (cme->type == netatom[NetMoveResizeWindow]) {
		/* _NET_MOVERESIZE_WINDOW client message */
		int          x, y, w, h;
		unsigned int gravity_flags = cme->data.data32[0];

		x = (gravity_flags & (1 << 8)) ? (int) cme->data.data32[1] : c->x;
		y = (gravity_flags & (1 << 9)) ? (int) cme->data.data32[2] : c->y;
		w = (gravity_flags & (1 << 10)) ? (int) cme->data.data32[3] : c->w;
		h = (gravity_flags & (1 << 11)) ? (int) cme->data.data32[4] : c->h;

		resize(c, x, y, w, h, 1);
	}
}

void
configurenotify(xcb_generic_event_t *e)
{
	Monitor                      *m;
	Client                       *c;
	xcb_configure_notify_event_t *ev = (xcb_configure_notify_event_t *) e;

	if (ev->window == root) {
		sw = ev->width;
		sh = ev->height;
		if (updategeom()) {
			drw_resize(drw, sw, bh);
			updatebars();
			for (m = mons; m; m = m->next) {
				for (c = m->cl->clients; c; c = c->next)
					if (c->isfullscreen)
						resizeclient(c, m->mx, m->my, m->mw, m->mh);
				resizebarwin(m);
			}
			focus(NULL);
			arrange(NULL);
		}
#ifdef COMPOSITOR
		compositor_notify_screen_resize();
#endif
	}
}

void
configurerequest(xcb_generic_event_t *e)
{
	Client                        *c;
	Monitor                       *m;
	xcb_configure_request_event_t *ev = (xcb_configure_request_event_t *) e;

	if ((c = wintoclient(ev->window))) {
		if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
			c->bw = ev->border_width;
		else if (c->isfloating || !selmon->lt[selmon->sellt]->arrange) {
			m = c->mon;
			if (!c->issteam) {
				if (ev->value_mask & XCB_CONFIG_WINDOW_X) {
					c->oldx = c->x;
					c->x    = m->mx + ev->x;
				}
				if (ev->value_mask & XCB_CONFIG_WINDOW_Y) {
					c->oldy = c->y;
					c->y    = m->my + ev->y;
				}
			}
			if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
				c->oldw = c->w;
				c->w    = ev->width;
			}
			if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
				c->oldh = c->h;
				c->h    = ev->height;
			}
			if ((c->x + c->w) > m->mx + m->mw && c->isfloating)
				c->x = m->mx +
				    (m->mw / 2 - WIDTH(c) / 2); /* center in x direction */
			if ((c->y + c->h) > m->my + m->mh && c->isfloating)
				c->y = m->my +
				    (m->mh / 2 - HEIGHT(c) / 2); /* center in y direction */
			if ((ev->value_mask &
			        (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y)) &&
			    !(ev->value_mask &
			        (XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT)))
				configure(c);
			if (ISVISIBLE(c, m)) {
				uint32_t xywh[4] = {
					(uint32_t) (int32_t) c->x,
					(uint32_t) (int32_t) c->y,
					(uint32_t) c->w,
					(uint32_t) c->h,
				};
				xcb_configure_window(xc, c->win,
				    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
				        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
				    xywh);
			}
		} else
			configure(c);
	} else {
		/* Pass unmanaged window configure requests straight through.
		 * Build the XCB value array in ascending bit-position order. */
		uint32_t vals[7];
		int      n = 0;
		if (ev->value_mask & XCB_CONFIG_WINDOW_X)
			vals[n++] = (uint32_t) (int32_t) ev->x;
		if (ev->value_mask & XCB_CONFIG_WINDOW_Y)
			vals[n++] = (uint32_t) (int32_t) ev->y;
		if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)
			vals[n++] = (uint32_t) ev->width;
		if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
			vals[n++] = (uint32_t) ev->height;
		if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
			vals[n++] = (uint32_t) ev->border_width;
		if (ev->value_mask & XCB_CONFIG_WINDOW_SIBLING)
			vals[n++] = (uint32_t) ev->sibling;
		if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
			vals[n++] = (uint32_t) ev->stack_mode;
		if (n > 0)
			xcb_configure_window(
			    xc, ev->window, ev->value_mask, vals);
	}
	xflush();
}

void
destroynotify(xcb_generic_event_t *e)
{
	Client                     *c;
	xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t *) e;

	if ((c = wintoclient(ev->window)))
		unmanage(c, 1);
	else if ((c = wintosystrayicon(ev->window))) {
		removesystrayicon(c);
		resizebarwin(selmon);
		updatesystray();
	}
}

void
enternotify(xcb_generic_event_t *e)
{
	Client                   *c;
	Monitor                  *m;
	xcb_enter_notify_event_t *ev = (xcb_enter_notify_event_t *) e;

	if ((ev->mode != XCB_NOTIFY_MODE_NORMAL ||
	        ev->detail == XCB_NOTIFY_DETAIL_INFERIOR) &&
	    ev->event != root)
		return;
	c = wintoclient(ev->event);
	m = c ? c->mon : wintomon(ev->event);
	if (m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
	} else if (!c || c == selmon->sel)
		return;
	focus(c);
}

void
expose(xcb_generic_event_t *e)
{
	Monitor            *m;
	xcb_expose_event_t *ev = (xcb_expose_event_t *) e;

	if (ev->count == 0 && (m = wintomon(ev->window))) {
		drawbar(m);
		if (m == selmon)
			updatesystray();
	}
}

/* Return 1 if `w` is a descendant of `ancestor` in the X window tree.
 * We walk up via XQueryTree, stopping at the root.  The depth is bounded by
 * the browser's internal widget hierarchy (typically 2–5 hops), so this is
 * cheap in practice. */
static int
iswindowdescendant(Window w, Window ancestor)
{
	

	while (w && w != ancestor && w != root) {
		xcb_query_tree_reply_t *r =
		    xcb_query_tree_reply(xc, xcb_query_tree(xc, w), NULL);
		if (!r)
			break;
		w = r->parent;
		free(r);
	}
	return w == ancestor;
}

/* there are some broken focus acquiring clients needing extra handling */
void
focusin(xcb_generic_event_t *e)
{
	xcb_focus_in_event_t *ev = (xcb_focus_in_event_t *) e;

	if (!selmon->sel || ev->event == selmon->sel->win)
		return;

	/* Allow focus to move to a child window of the currently focused client
	 * (e.g. an in-page widget, chat overlay, or popup inside a fullscreen
	 * browser window).  Without this guard, focusin() would steal focus back
	 * to the top-level client window, making those widgets unreachable. */
	if (iswindowdescendant(ev->event, selmon->sel->win))
		return;

	setfocus(selmon->sel);
}

void
grabkeys(void)
{
	updatenumlockmask();
	{
		unsigned int       i, j, k;
		unsigned int       modifiers[] = { 0, LockMask, numlockmask,
			      numlockmask | LockMask };
		const xcb_setup_t *setup       = xcb_get_setup(xc);
		xcb_keycode_t      kmin        = setup->min_keycode;
		xcb_keycode_t      kmax        = setup->max_keycode;
		int                count       = kmax - kmin + 1;

		xcb_ungrab_key(xc, XCB_GRAB_ANY, root, XCB_MOD_MASK_ANY);

		xcb_get_keyboard_mapping_cookie_t mck =
		    xcb_get_keyboard_mapping(xc, kmin, (uint8_t) count);
		xcb_get_keyboard_mapping_reply_t *mr =
		    xcb_get_keyboard_mapping_reply(xc, mck, NULL);
		if (!mr)
			return;
		int           skip = mr->keysyms_per_keycode;
		xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(mr);
		for (k = kmin; k <= kmax; k++)
			for (i = 0; i < LENGTH(keys); i++)
				if (keys[i].keysym == (KeySym) syms[(k - kmin) * skip])
					for (j = 0; j < LENGTH(modifiers); j++)
						xcb_grab_key(xc, 1, root,
						    (uint16_t) (keys[i].mod | modifiers[j]),
						    (xcb_keycode_t) k, XCB_GRAB_MODE_ASYNC,
						    XCB_GRAB_MODE_ASYNC);
		free(mr);
	}
}

void
keypress(xcb_generic_event_t *e)
{
	unsigned int           i;
	xcb_keysym_t           keysym;
	xcb_key_press_event_t *ev;

	ev              = (xcb_key_press_event_t *) e;
	last_event_time = ev->time;
	keysym =
	    xcb_key_symbols_get_keysym(keysyms, (xcb_keycode_t) ev->detail, 0);
	for (i = 0; i < LENGTH(keys); i++)
		if ((KeySym) keysym == keys[i].keysym &&
		    CLEANMASK(keys[i].mod) == CLEANMASK(ev->state) && keys[i].func)
			keys[i].func(&(keys[i].arg));
}

int
fake_signal(void)
{
	char   fsignal[256];
	char   indicator[9] = "fsignal:";
	char   str_signum[16];
	int    i, v, signum;
	size_t len_fsignal, len_indicator = strlen(indicator);

	// Get root name property
	if (gettextprop(root, XA_WM_NAME, fsignal, sizeof(fsignal))) {
		len_fsignal = strlen(fsignal);

		// Check if this is indeed a fake signal
		if (len_indicator > len_fsignal
		        ? 0
		        : strncmp(indicator, fsignal, len_indicator) == 0) {
			size_t siglen =
			    MIN(len_fsignal - len_indicator, sizeof(str_signum) - 1);
			memcpy(str_signum, &fsignal[len_indicator], siglen);
			str_signum[siglen] = '\0';

			// Convert string value into managable integer
			for (i = signum = 0; i < strlen(str_signum); i++) {
				v = str_signum[i] - '0';
				if (v >= 0 && v <= 9) {
					signum = signum * 10 + v;
				}
			}

			// Check if a signal was found, and if so handle it
			if (signum)
				for (i = 0; i < LENGTH(signals); i++)
					if (signum == signals[i].signum && signals[i].func)
						signals[i].func(&(signals[i].arg));

			// A fake signal was sent
			return 1;
		}
	}

	// No fake signal was sent, so proceed with update
	return 0;
}

void
mappingnotify(xcb_generic_event_t *e)
{
	xcb_mapping_notify_event_t *ev = (xcb_mapping_notify_event_t *) e;

	xcb_refresh_keyboard_mapping(keysyms, ev);
	if (ev->request == XCB_MAPPING_KEYBOARD)
		grabkeys();
}

void
maprequest(xcb_generic_event_t *e)
{
	xcb_map_request_event_t *ev = (xcb_map_request_event_t *) e;

	Client *i;
	if ((i = wintosystrayicon(ev->window))) {
		/* Systray icon requested mapping - handle via updatesystray */
		resizebarwin(selmon);
		updatesystray();
		return;
	}

	{
		xcb_get_window_attributes_cookie_t ck =
		    xcb_get_window_attributes(xc, ev->window);
		xcb_get_window_attributes_reply_t *r =
		    xcb_get_window_attributes_reply(xc, ck, NULL);
		if (!r)
			return;
		int override = r->override_redirect;
		free(r);
		if (override)
			return;
	}
	if (!wintoclient(ev->window)) {
		xcb_get_geometry_cookie_t gck =
		    xcb_get_geometry(xc, ev->window);
		xcb_get_geometry_reply_t *gr =
		    xcb_get_geometry_reply(xc, gck, NULL);
		if (gr) {
			XWindowAttributes wa;
			wa.x            = gr->x;
			wa.y            = gr->y;
			wa.width        = gr->width;
			wa.height       = gr->height;
			wa.border_width = gr->border_width;
			free(gr);
			manage(ev->window, &wa);
		}
	}
}

void
motionnotify(xcb_generic_event_t *e)
{
	static Monitor            *mon = NULL;
	Monitor                   *m;
	xcb_motion_notify_event_t *ev = (xcb_motion_notify_event_t *) e;

	if (ev->event != root)
		return;
	if ((m = recttomon(ev->root_x, ev->root_y, 1, 1)) != mon && mon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	mon = m;
}

void
propertynotify(xcb_generic_event_t *e)
{
	Client                      *c;
	Window                       trans;
	xcb_property_notify_event_t *ev = (xcb_property_notify_event_t *) e;

	if ((c = wintosystrayicon(ev->window))) {
		if (ev->atom == XA_WM_NORMAL_HINTS) {
			updatesizehints(c);
			updatesystrayicongeom(c, c->w, c->h);
		} else
			updatesystrayiconstate(c, ev);
		resizebarwin(selmon);
		updatesystray();
	}

	if ((ev->window == root) && (ev->atom == XA_WM_NAME)) {
		(void) fake_signal();
		return;
	} else if (ev->state == XCB_PROPERTY_DELETE)
		return; /* ignore */
	else if ((c = wintoclient(ev->window))) {
		switch (ev->atom) {
		default:
			break;
		case XA_WM_TRANSIENT_FOR:
			if (!c->isfloating &&
			    xcb_icccm_get_wm_transient_for_reply(xc,
			        xcb_icccm_get_wm_transient_for(
			            xc, c->win),
			        (xcb_window_t *) (void *) &trans, NULL) &&
			    (c->isfloating = (wintoclient(trans)) != NULL))
				arrange(c->mon);
			break;
		case XA_WM_NORMAL_HINTS:
			c->hintsvalid = 0;
			break;
		case XA_WM_HINTS:
			updatewmhints(c);
			barsdirty = 1; /* defer redraw */
			break;
		}
		if (ev->atom == XA_WM_NAME || ev->atom == netatom[NetWMName]) {
			updatetitle(c);
			if (c == c->mon->sel)
				barsdirty = 1; /* defer redraw */
		}
		if (ev->atom == netatom[NetWMWindowType])
			updatewindowtype(c);
#ifdef COMPOSITOR
		if (ev->atom == netatom[NetWMWindowOpacity]) {
			unsigned long raw =
			    (unsigned long) getatomprop(c, netatom[NetWMWindowOpacity]);
			compositor_set_opacity(c, raw);
		}
#endif
	}
}

void
resizerequest(xcb_generic_event_t *e)
{
	xcb_resize_request_event_t *ev = (xcb_resize_request_event_t *) e;
	Client                     *i;

	if ((i = wintosystrayicon(ev->window))) {
		updatesystrayicongeom(i, ev->width, ev->height);
		resizebarwin(selmon);
		updatesystray();
	}
}

void
unmapnotify(xcb_generic_event_t *e)
{
	Client                   *c;
	xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *) e;

	if ((c = wintoclient(ev->window))) {
		if (e->response_type & 0x80)
			setclientstate(c, WithdrawnState);
		else
			unmanage(c, 0);
	} else if ((c = wintosystrayicon(ev->window))) {
		/* KLUDGE! sometimes icons occasionally unmap their windows, but do
		 * _not_ destroy them. We map those windows back */
		{
			uint32_t          above = XCB_STACK_MODE_ABOVE;
			xcb_map_window(xc, c->win);
			xcb_configure_window(
			    xc, c->win, XCB_CONFIG_WINDOW_STACK_MODE, &above);
		}
		updatesystray();
	}
}

void
updatenumlockmask(void)
{
	unsigned int                      i, j;
	xcb_get_modifier_mapping_cookie_t ck = xcb_get_modifier_mapping(xc);
	xcb_get_modifier_mapping_reply_t *mr;
	xcb_keycode_t                    *nlcodes;
	xcb_keycode_t                    *modcodes;

	numlockmask = 0;
	mr          = xcb_get_modifier_mapping_reply(xc, ck, NULL);
	if (!mr)
		return;
	nlcodes  = xcb_key_symbols_get_keycode(keysyms, XK_Num_Lock);
	modcodes = xcb_get_modifier_mapping_keycodes(mr);
	if (nlcodes) {
		for (i = 0; i < 8; i++)
			for (j = 0; j < mr->keycodes_per_modifier; j++) {
				xcb_keycode_t kc = modcodes[i * mr->keycodes_per_modifier + j];
				xcb_keycode_t *nl;
				for (nl = nlcodes; *nl != XCB_NO_SYMBOL; nl++)
					if (kc == *nl)
						numlockmask = (1u << i);
			}
		free(nlcodes);
	}
	free(mr);
}

int
xerror(Display *dpy, XErrorEvent *ee)
{
	if (ee->error_code == BadWindow ||
	    (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch) ||
	    (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable) ||
	    (ee->request_code == X_PolyFillRectangle &&
	        ee->error_code == BadDrawable) ||
	    (ee->request_code == X_PolySegment && ee->error_code == BadDrawable) ||
	    (ee->request_code == X_ConfigureWindow &&
	        ee->error_code == BadMatch) ||
	    (ee->request_code == X_GrabButton && ee->error_code == BadAccess) ||
	    (ee->request_code == X_GrabKey && ee->error_code == BadAccess) ||
	    (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
		return 0;
#ifdef COMPOSITOR
	/* Transient XRender errors (BadPicture, BadPictFormat) arise when a GL
	 * window (e.g. alacritty) exits while a compositor repaint is in flight.
	 * The compositor uses xerror_push_ignore() around individual calls, but
	 * asynchronous errors can still slip through.  Whitelist them here so
	 * the WM does not exit. */
	{
		int render_req, render_err;
		compositor_xrender_errors(&render_req, &render_err);
		if (render_req > 0 && ee->request_code == render_req &&
		    (ee->error_code == render_err           /* BadPicture    */
		        || ee->error_code == render_err + 1 /* BadPictFormat */
		        || ee->error_code == BadDrawable ||
		        ee->error_code == BadPixmap))
			return 0;
	}
	/* Transient XDamage errors (BadDamage) arise when a window is destroyed
	 * while we are calling XDamageDestroy on its Damage handle. */
	{
		int damage_err;
		compositor_damage_errors(&damage_err);
		if (damage_err >= 0 && ee->error_code == damage_err) /* BadDamage */
			return 0;
	}
	/* Transient GLX errors arise when glXDestroyPixmap / glXReleaseTexImageEXT
	 * is called on a pixmap that the X server has already invalidated — this
	 * happens routinely when a fullscreen window bypasses the compositor and
	 * its TFP pixmap is released mid-frame.  These are harmless; ignore them
	 * rather than letting the default Xlib handler call exit(). */
	{
		int glx_req, glx_err;
		compositor_glx_errors(&glx_req, &glx_err);
		if (glx_req > 0 && ee->request_code == glx_req) {
			awm_debug("xerror: ignoring GLX error: "
			          "request_code=%d error_code=%d",
			    (int) ee->request_code, (int) ee->error_code);
			return 0;
		}
		(void) glx_err;
	}
#endif
	{
		char desc[128];
		XGetErrorText(dpy, ee->error_code, desc, sizeof(desc));
		awm_error("fatal X11 error: %s (request_code=%d error_code=%d "
		          "resourceid=0x%lx)",
		    desc, (int) ee->request_code, (int) ee->error_code,
		    (unsigned long) ee->resourceid);
	}
	return xerrorxlib(dpy, ee); /* may call exit */
}
