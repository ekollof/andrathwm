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
buttonpress(XEvent *e)
{
	unsigned int         i, x, click;
	Arg                  arg = { 0 };
	Client              *c;
	Monitor             *m;
	XButtonPressedEvent *ev = &e->xbutton;

	click = ClkRootWin;
	/* focus monitor if necessary */
	if ((m = wintomon(ev->window)) && m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	if (ev->window == selmon->barwin) {
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
			if (ev->x < x + tw) {
				click  = ClkTagBar;
				arg.ui = 1 << i;
				break;
			}
			x += tw;
		}

		if (i >= LENGTH(tags) && ev->x < x + TEXTW(selmon->ltsymbol))
			click = ClkLtSymbol;
		else if (ev->x > selmon->ww - (int) TEXTW(stext) - getsystraywidth())
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
					if (ev->x >= cx && ev->x < cx + tabw) {
						c = t;
						break;
					}
					cx += tabw;
				}
			}

			if (c)
				arg.v = c;
		}
	} else if ((c = wintoclient(ev->window))) {
		focus(c);
		restack(selmon);
		xcb_allow_events(XGetXCBConnection(dpy), XCB_ALLOW_REPLAY_POINTER,
		    XCB_CURRENT_TIME);
		click = ClkClientWin;
	}
#ifdef STATUSNOTIFIER
	/* Check if click is on SNI icon */
	else {
		SNIItem *sni_item = sni_find_item_by_window(ev->window);
		if (sni_item) {
			sni_handle_click(
			    ev->window, ev->button, ev->x_root, ev->y_root, ev->time);
			return; /* Don't process further */
		}
	}
#endif
	for (i = 0; i < LENGTH(buttons); i++)
		if (click == buttons[i].click && buttons[i].func &&
		    buttons[i].button == ev->button &&
		    CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state))
			buttons[i].func((click == ClkTagBar && buttons[i].arg.i == 0) ||
			            click == ClkWinTitle
			        ? &arg
			        : &buttons[i].arg);
}

void
checkotherwm(void)
{
	xerrorxlib = XSetErrorHandler(xerrorstart);
	/* this causes an error if some other window manager is running */
	XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	XSync(dpy, False);
}

void
clientmessage(XEvent *e)
{
	XClientMessageEvent *cme = &e->xclient;
	Client              *c   = wintoclient(cme->window);
	unsigned int         i;

	if (showsystray && cme->window == systray->win &&
	    cme->message_type == netatom[NetSystemTrayOP]) {
		/* add systray icons */
		if (cme->data.l[1] == SYSTEM_TRAY_REQUEST_DOCK) {
			if (!(c = (Client *) calloc(1, sizeof(Client))))
				die("fatal: could not malloc() %u bytes\n", sizeof(Client));
			if (!(c->win = cme->data.l[2])) {
				free(c);
				return;
			}
			c->mon         = selmon;
			c->next        = systray->icons;
			systray->icons = c;
			{
				xcb_get_geometry_cookie_t ck =
				    xcb_get_geometry(XGetXCBConnection(dpy), c->win);
				xcb_get_geometry_reply_t *gr =
				    xcb_get_geometry_reply(XGetXCBConnection(dpy), ck, NULL);
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
			XAddToSaveSet(dpy, c->win);
			{
				uint32_t mask = StructureNotifyMask | PropertyChangeMask |
				    ResizeRedirectMask;
				xcb_change_window_attributes(
				    XGetXCBConnection(dpy), c->win, XCB_CW_EVENT_MASK, &mask);
			}
			xcb_reparent_window(
			    XGetXCBConnection(dpy), c->win, systray->win, 0, 0);
			/* use bar background so icon blends with the bar */
			{
				uint32_t bg = clr_to_argb(&scheme[SchemeNorm][ColBg]);
				xcb_change_window_attributes(
				    XGetXCBConnection(dpy), c->win, XCB_CW_BACK_PIXEL, &bg);
			}
			/* Send XEMBED_EMBEDDED_NOTIFY to complete embedding per spec.
			 * data1 = embedder window, data2 = protocol version */
			sendevent(c->win, netatom[Xembed], StructureNotifyMask,
			    CurrentTime, XEMBED_EMBEDDED_NOTIFY, 0, systray->win,
			    XEMBED_VERSION);
			xflush(dpy);
			resizebarwin(selmon);
			updatesystray();
			setclientstate(c, NormalState);
		}
		return;
	}

	if (!c)
		return;
	if (cme->message_type == netatom[NetWMState]) {
		if (cme->data.l[1] == netatom[NetWMFullscreen] ||
		    cme->data.l[2] == netatom[NetWMFullscreen])
			setfullscreen(c,
			    (cme->data.l[0] == 1 /* _NET_WM_STATE_ADD    */
			        || (cme->data.l[0] == 2 /* _NET_WM_STATE_TOGGLE */ &&
			               !c->isfullscreen)));
	} else if (cme->message_type == netatom[NetActiveWindow]) {
		for (i = 0; i < LENGTH(tags) && !((1 << i) & c->tags); i++)
			;
		if (i < LENGTH(tags)) {
			const Arg a = { .ui = 1 << i };
			selmon      = c->mon;
			view(&a);
			focus(c);
			restack(selmon);
		}
	} else if (cme->message_type == netatom[NetCloseWindow]) {
		/* _NET_CLOSE_WINDOW client message */
		if (!sendevent(c->win, wmatom[WMDelete], NoEventMask, wmatom[WMDelete],
		        CurrentTime, 0, 0, 0)) {
			XGrabServer(dpy);
			XSetErrorHandler(xerrordummy);
			xcb_set_close_down_mode(
			    XGetXCBConnection(dpy), XCB_CLOSE_DOWN_DESTROY_ALL);
			xcb_kill_client(XGetXCBConnection(dpy), c->win);
			xflush(dpy);
			XSetErrorHandler(xerror);
			XUngrabServer(dpy);
		}
	} else if (cme->message_type == netatom[NetMoveResizeWindow]) {
		/* _NET_MOVERESIZE_WINDOW client message */
		int          x, y, w, h;
		unsigned int gravity_flags = cme->data.l[0];

		x = (gravity_flags & (1 << 8)) ? cme->data.l[1] : c->x;
		y = (gravity_flags & (1 << 9)) ? cme->data.l[2] : c->y;
		w = (gravity_flags & (1 << 10)) ? cme->data.l[3] : c->w;
		h = (gravity_flags & (1 << 11)) ? cme->data.l[4] : c->h;

		resize(c, x, y, w, h, 1);
	}
}

void
configurenotify(XEvent *e)
{
	Monitor         *m;
	Client          *c;
	XConfigureEvent *ev = &e->xconfigure;

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
configurerequest(XEvent *e)
{
	Client                 *c;
	Monitor                *m;
	XConfigureRequestEvent *ev = &e->xconfigurerequest;

	if ((c = wintoclient(ev->window))) {
		if (ev->value_mask & CWBorderWidth)
			c->bw = ev->border_width;
		else if (c->isfloating || !selmon->lt[selmon->sellt]->arrange) {
			m = c->mon;
			if (!c->issteam) {
				if (ev->value_mask & CWX) {
					c->oldx = c->x;
					c->x    = m->mx + ev->x;
				}
				if (ev->value_mask & CWY) {
					c->oldy = c->y;
					c->y    = m->my + ev->y;
				}
			}
			if (ev->value_mask & CWWidth) {
				c->oldw = c->w;
				c->w    = ev->width;
			}
			if (ev->value_mask & CWHeight) {
				c->oldh = c->h;
				c->h    = ev->height;
			}
			if ((c->x + c->w) > m->mx + m->mw && c->isfloating)
				c->x = m->mx +
				    (m->mw / 2 - WIDTH(c) / 2); /* center in x direction */
			if ((c->y + c->h) > m->my + m->mh && c->isfloating)
				c->y = m->my +
				    (m->mh / 2 - HEIGHT(c) / 2); /* center in y direction */
			if ((ev->value_mask & (CWX | CWY)) &&
			    !(ev->value_mask & (CWWidth | CWHeight)))
				configure(c);
			if (ISVISIBLE(c, m)) {
				uint32_t xywh[4] = {
					(uint32_t) (int32_t) c->x,
					(uint32_t) (int32_t) c->y,
					(uint32_t) c->w,
					(uint32_t) c->h,
				};
				xcb_configure_window(XGetXCBConnection(dpy), c->win,
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
		if (ev->value_mask & CWX)
			vals[n++] = (uint32_t) (int32_t) ev->x;
		if (ev->value_mask & CWY)
			vals[n++] = (uint32_t) (int32_t) ev->y;
		if (ev->value_mask & CWWidth)
			vals[n++] = (uint32_t) ev->width;
		if (ev->value_mask & CWHeight)
			vals[n++] = (uint32_t) ev->height;
		if (ev->value_mask & CWBorderWidth)
			vals[n++] = (uint32_t) ev->border_width;
		if (ev->value_mask & CWSibling)
			vals[n++] = (uint32_t) ev->above;
		if (ev->value_mask & CWStackMode)
			vals[n++] = (uint32_t) ev->detail;
		if (n > 0)
			xcb_configure_window(XGetXCBConnection(dpy), ev->window,
			    (uint16_t) ev->value_mask, vals);
	}
	xflush(dpy);
}

void
destroynotify(XEvent *e)
{
	Client              *c;
	XDestroyWindowEvent *ev = &e->xdestroywindow;

	if ((c = wintoclient(ev->window)))
		unmanage(c, 1);
	else if ((c = wintosystrayicon(ev->window))) {
		removesystrayicon(c);
		resizebarwin(selmon);
		updatesystray();
	}
}

void
enternotify(XEvent *e)
{
	Client         *c;
	Monitor        *m;
	XCrossingEvent *ev = &e->xcrossing;

	if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) &&
	    ev->window != root)
		return;
	c = wintoclient(ev->window);
	m = c ? c->mon : wintomon(ev->window);
	if (m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
	} else if (!c || c == selmon->sel)
		return;
	focus(c);
}

void
expose(XEvent *e)
{
	Monitor      *m;
	XExposeEvent *ev = &e->xexpose;

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
	Window       root_ret, parent, *children;
	unsigned int nchildren;

	while (w && w != ancestor && w != root) {
		if (!XQueryTree(dpy, w, &root_ret, &parent, &children, &nchildren))
			break;
		if (children)
			XFree(children);
		w = parent;
	}
	return w == ancestor;
}

/* there are some broken focus acquiring clients needing extra handling */
void
focusin(XEvent *e)
{
	XFocusChangeEvent *ev = &e->xfocus;

	if (!selmon->sel || ev->window == selmon->sel->win)
		return;

	/* Allow focus to move to a child window of the currently focused client
	 * (e.g. an in-page widget, chat overlay, or popup inside a fullscreen
	 * browser window).  Without this guard, focusin() would steal focus back
	 * to the top-level client window, making those widgets unreachable. */
	if (iswindowdescendant(ev->window, selmon->sel->win))
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
		xcb_connection_t  *xc          = XGetXCBConnection(dpy);
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
keypress(XEvent *e)
{
	unsigned int i;
	xcb_keysym_t keysym;
	XKeyEvent   *ev;

	ev              = &e->xkey;
	last_event_time = ev->time;
	keysym =
	    xcb_key_symbols_get_keysym(keysyms, (xcb_keycode_t) ev->keycode, 0);
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
mappingnotify(XEvent *e)
{
	XMappingEvent             *ev  = &e->xmapping;
	xcb_mapping_notify_event_t mne = { 0 };

	mne.request       = (uint8_t) ev->request;
	mne.first_keycode = (xcb_keycode_t) ev->first_keycode;
	mne.count         = (uint8_t) ev->count;
	xcb_refresh_keyboard_mapping(keysyms, &mne);
	if (ev->request == MappingKeyboard)
		grabkeys();
}

void
maprequest(XEvent *e)
{
	XMapRequestEvent *ev = &e->xmaprequest;

	Client *i;
	if ((i = wintosystrayicon(ev->window))) {
		/* Systray icon requested mapping - handle via updatesystray */
		resizebarwin(selmon);
		updatesystray();
		return;
	}

	{
		xcb_get_window_attributes_cookie_t ck =
		    xcb_get_window_attributes(XGetXCBConnection(dpy), ev->window);
		xcb_get_window_attributes_reply_t *r =
		    xcb_get_window_attributes_reply(XGetXCBConnection(dpy), ck, NULL);
		if (!r)
			return;
		int override = r->override_redirect;
		free(r);
		if (override)
			return;
	}
	if (!wintoclient(ev->window)) {
		xcb_get_geometry_cookie_t gck =
		    xcb_get_geometry(XGetXCBConnection(dpy), ev->window);
		xcb_get_geometry_reply_t *gr =
		    xcb_get_geometry_reply(XGetXCBConnection(dpy), gck, NULL);
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
motionnotify(XEvent *e)
{
	static Monitor *mon = NULL;
	Monitor        *m;
	XMotionEvent   *ev = &e->xmotion;

	if (ev->window != root)
		return;
	if ((m = recttomon(ev->x_root, ev->y_root, 1, 1)) != mon && mon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	mon = m;
}

void
propertynotify(XEvent *e)
{
	Client         *c;
	Window          trans;
	XPropertyEvent *ev = &e->xproperty;

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
	} else if (ev->state == PropertyDelete)
		return; /* ignore */
	else if ((c = wintoclient(ev->window))) {
		switch (ev->atom) {
		default:
			break;
		case XA_WM_TRANSIENT_FOR:
			if (!c->isfloating &&
			    (XGetTransientForHint(dpy, c->win, &trans)) &&
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
resizerequest(XEvent *e)
{
	XResizeRequestEvent *ev = &e->xresizerequest;
	Client              *i;

	if ((i = wintosystrayicon(ev->window))) {
		updatesystrayicongeom(i, ev->width, ev->height);
		resizebarwin(selmon);
		updatesystray();
	}
}

void
unmapnotify(XEvent *e)
{
	Client      *c;
	XUnmapEvent *ev = &e->xunmap;

	if ((c = wintoclient(ev->window))) {
		if (ev->send_event)
			setclientstate(c, WithdrawnState);
		else
			unmanage(c, 0);
	} else if ((c = wintosystrayicon(ev->window))) {
		/* KLUDGE! sometimes icons occasionally unmap their windows, but do
		 * _not_ destroy them. We map those windows back */
		{
			xcb_connection_t *xc    = XGetXCBConnection(dpy);
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
	xcb_connection_t                 *xc = XGetXCBConnection(dpy);
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

int
xerrordummy(Display *dpy, XErrorEvent *ee)
{
	return 0;
}

/* Startup Error handler to check if another window manager
 * is already running. */
int
xerrorstart(Display *dpy, XErrorEvent *ee)
{
	die("awm: another window manager is already running");
	return -1;
}
