/* AndrathWM - X event handlers
 * See LICENSE file for copyright and license details. */

#include <assert.h>
#include "events.h"
#include "awm.h"
#include <xkbcommon/xkbcommon-keysyms.h>
#include "client.h"
#include "compositor.h"
#include "ewmh.h"
#include "monitor.h"
#include "spawn.h"
#include "switcher.h"
#include "systray.h"
#include "wmstate.h"
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
	if ((m = wintomon(ev->event)) && m != g_awm_selmon) {
		unfocus(g_awm_selmon->sel, 1);
		g_awm_set_selmon(m);
		focus(NULL);
	}
	if (ev->event == g_awm_selmon->barwin) {
		i = x = 0;
		/* Calculate x position after tags (accounting for hidden empty tags)
		 */
		unsigned int occ = 0;
		for (Client *tc = g_awm.clients_head; tc; tc = tc->next) {
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

		if (i >= LENGTH(tags) &&
		    ev->event_x < x + TEXTW(g_awm_selmon->ltsymbol))
			click = ClkLtSymbol;
		else if (ev->event_x > g_awm_selmon->ww -
		        g_render_backend->draw_statusd(drw, 0, 0, 0, 0, stext) -
		        getsystraywidth())
			click = ClkStatusText;
		else if (i >= LENGTH(tags)) {
			/* Awesomebar - find which window was clicked */
			click = ClkWinTitle;
			c     = NULL;

			/* Add layout symbol width to x position */
			x += TEXTW(g_awm_selmon->ltsymbol);

			int n = 0;
			for (Client *t = g_awm.clients_head; t; t = t->next)
				if (t->tags & m->tagset[m->seltags])
					n++;

			if (n > 0) {
				int tw =
				    g_render_backend->draw_statusd(drw, 0, 0, 0, 0, stext);
				int stw       = getsystraywidth();
				int remainder = m->ww - tw - stw - x;
				int tabw      = remainder / n;
				int cx        = x;

				for (Client *t = g_awm.clients_head; t; t = t->next) {
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
		restack(g_awm_selmon);
		g_wm_backend->allow_events(
		    &g_plat, XCB_ALLOW_REPLAY_POINTER, XCB_CURRENT_TIME);
		xflush();
		click = ClkClientWin;
	}
#ifdef STATUSNOTIFIER
	/* Check if click is on SNI icon */
	else {
		SNIItem *sni_item = sni_find_item_by_window(ev->event);
		if (sni_item) {
			/* Release the passive-grab pointer sync before handing off to
			 * GTK — without this the X server keeps the pointer frozen and
			 * GTK's subsequent seat-grab (for the popup menu) either fails
			 * or never releases, killing awm's key bindings.
			 * Use ASYNC_POINTER (not SYNC_POINTER) so the pointer is freed
			 * unconditionally without replaying the event back to the
			 * g_plat.root, which would cause a grab race with GTK's menu
			 * seat-grab. */
			g_wm_backend->allow_events(
			    &g_plat, XCB_ALLOW_ASYNC_POINTER, ev->time);
			g_wm_backend->flush(&g_plat);
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
	g_wm_backend->check_other_wm(&g_plat);
}

void
clientmessage(xcb_generic_event_t *e)
{
	xcb_client_message_event_t *cme = (xcb_client_message_event_t *) e;
	Client                     *c   = wintoclient(cme->window);
	unsigned int                i;

	if (showsystray && systray && cme->window == systray->win &&
	    cme->type == g_plat.netatom[NetSystemTrayOP]) {
		/* add systray icons */
		if (cme->data.data32[1] == SYSTEM_TRAY_REQUEST_DOCK) {
			if (!(c = (Client *) calloc(1, sizeof(Client))))
				die("fatal: could not malloc() %u bytes\n", sizeof(Client));
			if (!(c->win = cme->data.data32[2])) {
				free(c);
				return;
			}
			c->mon         = g_awm_selmon;
			c->next        = systray->icons;
			systray->icons = c;
			{
				int gx, gy, gw, gh, gbw;
				if (g_wm_backend->get_geometry(
				        &g_plat, c->win, &gx, &gy, &gw, &gh, &gbw)) {
					c->w = c->oldw = gw;
					c->h = c->oldh = gh;
					c->oldbw       = gbw;
				} else {
					c->w = c->oldw = g_plat.bh;
					c->h = c->oldh = g_plat.bh;
					c->oldbw       = 0;
				}
			}
			c->x = c->oldx = c->y = c->oldy = 0;
			c->bw                           = 0;
			c->isfloating                   = 1;
			/* reuse tags field as mapped status */
			c->tags = 1;
			updatesizehints(c);
			updatesystrayicongeom(c, c->w, c->h);
			g_wm_backend->change_save_set(&g_plat, c->win, 1);
			{
				uint32_t mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY |
				    XCB_EVENT_MASK_PROPERTY_CHANGE |
				    XCB_EVENT_MASK_RESIZE_REDIRECT;
				g_wm_backend->change_attr(
				    &g_plat, c->win, XCB_CW_EVENT_MASK, &mask);
			}
			g_wm_backend->reparent_window(&g_plat, c->win, systray->win, 0, 0);
			/* use bar background so icon blends with the bar */
			{
				uint32_t bg = clr_to_argb(&scheme[SchemeNorm][ColBg]);
				g_wm_backend->change_attr(
				    &g_plat, c->win, XCB_CW_BACK_PIXEL, &bg);
			}
			/* Send XEMBED_EMBEDDED_NOTIFY to complete embedding per spec.
			 * data1 = embedder window, data2 = protocol version */
			sendevent(c->win, g_plat.xatom[Xembed],
			    XCB_EVENT_MASK_STRUCTURE_NOTIFY, XCB_CURRENT_TIME,
			    XEMBED_EMBEDDED_NOTIFY, 0, systray->win, XEMBED_VERSION);
			xflush();
			resizebarwin(g_awm_selmon);
			updatesystray();
			setclientstate(c, XCB_ICCCM_WM_STATE_NORMAL);
		}
		return;
	}

	if (!c)
		return;
	if (cme->type == g_plat.netatom[NetWMState]) {
		if (cme->data.data32[1] == g_plat.netatom[NetWMFullscreen] ||
		    cme->data.data32[2] == g_plat.netatom[NetWMFullscreen])
			setfullscreen(c,
			    (cme->data.data32[0] == 1 /* _NET_WM_STATE_ADD    */
			        || (cme->data.data32[0] == 2 /* _NET_WM_STATE_TOGGLE */ &&
			               !c->isfullscreen)));
	} else if (cme->type == g_plat.netatom[NetActiveWindow]) {
		for (i = 0; i < LENGTH(tags) && !((1 << i) & c->tags); i++)
			;
		if (i < LENGTH(tags)) {
			const Arg a = { .ui = 1 << i };
			g_awm_set_selmon(c->mon);
			view(&a);
			focus(c);
			restack(g_awm_selmon);
		}
	} else if (cme->type == g_plat.netatom[NetCloseWindow]) {
		/* _NET_CLOSE_WINDOW client message */
		if (!sendevent(c->win, g_plat.wmatom[WMDelete], 0,
		        g_plat.wmatom[WMDelete], XCB_CURRENT_TIME, 0, 0, 0)) {
			g_wm_backend->kill_client_hard(&g_plat, c->win);
		}
		xflush();
	} else if (cme->type == g_plat.netatom[NetMoveResizeWindow]) {
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

	if (ev->window == g_plat.root) {
		g_plat.sw = ev->width;
		g_plat.sh = ev->height;
		if (updategeom()) {
			g_render_backend->resize(drw, g_plat.sw, g_plat.bh);
			updatebars();
			FOR_EACH_MON(m)
			{
				for (c = g_awm.clients_head; c; c = c->next)
					if (c->isfullscreen) {
						/* Move window to fill new monitor geometry without
						 * touching old{x,y,w,h} so unfullscreen restores
						 * the correct pre-fullscreen position. */
						uint32_t vals[4] = {
							(uint32_t) (int32_t) m->mx,
							(uint32_t) (int32_t) m->my,
							(uint32_t) m->mw,
							(uint32_t) m->mh,
						};
						g_wm_backend->configure_win(&g_plat, c->win,
						    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
						        XCB_CONFIG_WINDOW_WIDTH |
						        XCB_CONFIG_WINDOW_HEIGHT,
						    vals);
						c->x = m->mx;
						c->y = m->my;
						c->w = m->mw;
						c->h = m->mh;
						configure(c);
					}
				resizebarwin(m);
			}
			focus(NULL);
			arrange(NULL);
			FOR_EACH_MON(m)
			updateworkarea(m);
		}
#ifdef COMPOSITOR
		compositor_notify_screen_resize();
#endif
	}
	wmstate_update();
}

void
configurerequest(xcb_generic_event_t *e)
{
	Client                        *c;
	Monitor                       *m;
	xcb_configure_request_event_t *ev = (xcb_configure_request_event_t *) e;

	if ((c = wintoclient(ev->window))) {
		if (c->isfullscreen) {
			/* Don't let clients move/resize themselves while fullscreen;
			 * just echo back the current geometry so they don't hang. */
			configure(c);
			xflush();
			return;
		}
		if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
			c->bw = ev->border_width;
		else if (c->isfloating ||
		    !g_awm_selmon->lt[g_awm_selmon->sellt]->arrange) {
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
				g_wm_backend->configure_win(&g_plat, c->win,
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
			g_wm_backend->configure_win(
			    &g_plat, ev->window, ev->value_mask, vals);
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
		resizebarwin(g_awm_selmon);
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
	    ev->event != g_plat.root)
		return;

	/* While the launcher is open it owns keyboard focus; suppress
	 * focus-follows-mouse so hover over another window does not steal it. */
	if (launcher_visible)
		return;

	/* Same guard for the window switcher. */
	if (switcher_active())
		return;

	/* Bar hover — trigger window preview popup */
	FOR_EACH_MON(m)
	{
		if (ev->event == m->barwin) {
			bar_hover_enter(m);
			return;
		}
	}

	c = wintoclient(ev->event);
	m = c ? c->mon : wintomon(ev->event);
	if (m != g_awm_selmon) {
		unfocus(g_awm_selmon->sel, 1);
		g_awm_set_selmon(m);
	} else if (!c || c == g_awm_selmon->sel)
		return;
	focus(c);
	wmstate_update();
}

void
leavenotify(xcb_generic_event_t *e)
{
	Monitor                  *m;
	xcb_leave_notify_event_t *ev = (xcb_leave_notify_event_t *) e;

	if (ev->mode != XCB_NOTIFY_MODE_NORMAL)
		return;

	/* Bar un-hover — hide window preview popup.
	 * Mirror the enternotify() guards: do not dismiss the preview
	 * while the launcher or switcher owns focus, otherwise hovering
	 * onto those overlapping windows would prematurely hide it. */
	if (launcher_visible || switcher_active())
		return;

	FOR_EACH_MON(m)
	{
		if (ev->event == m->barwin) {
			bar_hover_leave();
			return;
		}
	}
}

void
expose(xcb_generic_event_t *e)
{
	Monitor            *m;
	xcb_expose_event_t *ev = (xcb_expose_event_t *) e;

	if (ev->count == 0 && (m = wintomon(ev->window))) {
		drawbar(m);
		if (m == g_awm_selmon)
			updatesystray();
	}
}

/* there are some broken focus acquiring clients needing extra handling */
void
focusin(xcb_generic_event_t *e)
{
	xcb_focus_in_event_t *ev = (xcb_focus_in_event_t *) e;

	/* While the switcher is visible it must keep keyboard focus; prevent awm
	 * from stealing it back to the previously focused client. */
	if (switcher_active())
		return;

	if (!g_awm_selmon->sel || ev->event == g_awm_selmon->sel->win)
		return;

	/* Allow focus to move to a child window of the currently focused client
	 * (e.g. an in-page widget, chat overlay, or popup inside a fullscreen
	 * browser window).  Without this guard, focusin() would steal focus back
	 * to the top-level client window, making those widgets unreachable. */
	if (g_wm_backend->is_window_descendant(
	        &g_plat, ev->event, g_awm_selmon->sel->win))
		return;

	/* Allow focus to move to override-redirect windows (e.g. the launcher).
	 * These are unmanaged by the WM by design; stealing focus back would
	 * make them permanently unfocusable. */
	{
		int or = 0;
		if (g_wm_backend->get_override_redirect(&g_plat, ev->event, & or) &&
		    or)
			return;
	}

	setfocus(g_awm_selmon->sel);
}

void
grabkeys(void)
{
	g_wm_backend->grab_keys_full(&g_plat);
}

void
keypress(xcb_generic_event_t *e)
{
	unsigned int           i;
	xcb_keysym_t           keysym;
	xcb_key_press_event_t *ev;

	ev              = (xcb_key_press_event_t *) e;
	last_event_time = ev->time;
	keysym = g_wm_backend->get_keysym(&g_plat, (xcb_keycode_t) ev->detail, 0);

	/* While the switcher is open, handle Tab/Escape/Return directly here
	 * and suppress the normal keybinding dispatch.  The switcher GTK window
	 * cannot receive key events because awm holds the X passive grab. */
	if (switcher_active()) {
		if ((KeySym) keysym == XKB_KEY_Escape) {
			switcher_cancel_xkb(NULL);
			return;
		}
		if ((KeySym) keysym == XKB_KEY_Return ||
		    (KeySym) keysym == XKB_KEY_KP_Enter) {
			switcher_confirm_xkb(NULL);
			return;
		}
		if ((KeySym) keysym == XKB_KEY_Tab) {
			/* Shift+Tab — state has Shift modifier */
			if (ev->state & XCB_MOD_MASK_SHIFT)
				switcher_prev(NULL);
			else
				switcher_next(NULL);
			return;
		}
		/* Any other key while switcher is open: ignore */
		return;
	}

	for (i = 0; i < LENGTH(keys); i++)
		if ((KeySym) keysym == keys[i].keysym &&
		    CLEANMASK(keys[i].mod) == CLEANMASK(ev->state) && keys[i].func)
			keys[i].func(&(keys[i].arg));
}

void
keyrelease(xcb_generic_event_t *e)
{
	xcb_key_release_event_t *ev = (xcb_key_release_event_t *) e;
	xcb_keysym_t             keysym =
	    g_wm_backend->get_keysym(&g_plat, (xcb_keycode_t) ev->detail, 0);

	/* Confirm the switcher when the modifier that opened it is released */
	if (switcher_active()) {
		if ((KeySym) keysym == XKB_KEY_Alt_L ||
		    (KeySym) keysym == XKB_KEY_Alt_R ||
		    (KeySym) keysym == XKB_KEY_Super_L ||
		    (KeySym) keysym == XKB_KEY_Super_R) {
			switcher_confirm_xkb(NULL);
		}
	}
}

int
fake_signal(void)
{
	char   fsignal[256];
	char   indicator[9] = "fsignal:";
	char   str_signum[16];
	int    i, v, signum;
	size_t len_fsignal, len_indicator = strlen(indicator);

	/* Get root name property */
	if (gettextprop(g_plat.root, XCB_ATOM_WM_NAME, fsignal, sizeof(fsignal))) {
		len_fsignal = strlen(fsignal);

		/* Check if this is indeed a fake signal */
		if (len_indicator > len_fsignal
		        ? 0
		        : strncmp(indicator, fsignal, len_indicator) == 0) {
			size_t siglen =
			    MIN(len_fsignal - len_indicator, sizeof(str_signum) - 1);
			memcpy(str_signum, &fsignal[len_indicator], siglen);
			str_signum[siglen] = '\0';

			/* Convert string value into manageable integer */
			for (i = signum = 0; i < strlen(str_signum); i++) {
				v = str_signum[i] - '0';
				if (v >= 0 && v <= 9) {
					signum = signum * 10 + v;
				}
			}

			/* Check if a signal was found, and if so handle it */
			if (signum)
				for (i = 0; i < LENGTH(signals); i++)
					if (signum == signals[i].signum && signals[i].func)
						signals[i].func(&(signals[i].arg));

			/* A fake signal was sent */
			return 1;
		}
	}

	/* No fake signal was sent, so proceed with update */
	return 0;
}

void
mappingnotify(xcb_generic_event_t *e)
{
	xcb_mapping_notify_event_t *ev = (xcb_mapping_notify_event_t *) e;

	g_wm_backend->refresh_keyboard_mapping(&g_plat, ev);
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
		resizebarwin(g_awm_selmon);
		updatesystray();
		return;
	}

	{
		int override = 0;
		if (!g_wm_backend->get_override_redirect(
		        &g_plat, ev->window, &override))
			return;
		if (override)
			return;
	}
	if (!wintoclient(ev->window)) {
		int gx, gy, gw, gh, gbw;
		if (g_wm_backend->get_geometry(
		        &g_plat, ev->window, &gx, &gy, &gw, &gh, &gbw))
			manage(ev->window, gx, gy, gw, gh, gbw);
	}
}

void
motionnotify(xcb_generic_event_t *e)
{
	static Monitor            *mon = NULL;
	Monitor                   *m;
	xcb_motion_notify_event_t *ev = (xcb_motion_notify_event_t *) e;

	if (ev->event != g_plat.root)
		return;
	if ((m = recttomon(ev->root_x, ev->root_y, 1, 1)) != mon && mon) {
		unfocus(g_awm_selmon->sel, 1);
		g_awm_set_selmon(m);
		focus(NULL);
	}
	mon = m;
	wmstate_update();
}

void
propertynotify(xcb_generic_event_t *e)
{
	Client                      *c;
	xcb_window_t                 trans;
	xcb_property_notify_event_t *ev = (xcb_property_notify_event_t *) e;

	if ((c = wintosystrayicon(ev->window))) {
		if (ev->atom == XCB_ATOM_WM_NORMAL_HINTS) {
			updatesizehints(c);
			updatesystrayicongeom(c, c->w, c->h);
		} else
			updatesystrayiconstate(c, ev);
		resizebarwin(g_awm_selmon);
		updatesystray();
	}

	if ((ev->window == g_plat.root) && (ev->atom == XCB_ATOM_WM_NAME)) {
		(void) fake_signal();
		return;
	} else if (ev->state == XCB_PROPERTY_DELETE)
		return; /* ignore */
	else if ((c = wintoclient(ev->window))) {
		switch (ev->atom) {
		default:
			break;
		case XCB_ATOM_WM_TRANSIENT_FOR:
			if (!c->isfloating &&
			    (trans = g_wm_backend->get_wm_transient_for(
			         &g_plat, c->win)) != XCB_WINDOW_NONE &&
			    (c->isfloating = (wintoclient(trans)) != NULL))
				arrange(c->mon);
			break;
		case XCB_ATOM_WM_NORMAL_HINTS:
			c->hintsvalid = 0;
			break;
		case XCB_ATOM_WM_HINTS:
			updatewmhints(c);
			barsdirty = 1; /* defer redraw */
			break;
		}
		if (ev->atom == XCB_ATOM_WM_NAME ||
		    ev->atom == g_plat.netatom[NetWMName]) {
			updatetitle(c);
			if (c == c->mon->sel)
				barsdirty = 1; /* defer redraw */
		}
		if (ev->atom == g_plat.netatom[NetWMWindowType])
			updatewindowtype(c);
#ifdef COMPOSITOR
		if (ev->atom == g_plat.netatom[NetWMWindowOpacity]) {
			unsigned long raw = (unsigned long) getatomprop(
			    c, g_plat.netatom[NetWMWindowOpacity]);
			compositor_set_opacity(c, raw);
		}
		if (ev->atom == g_plat.netatom[NetWMBypassCompositor]) {
			int hint =
			    (int) getatomprop(c, g_plat.netatom[NetWMBypassCompositor]);
			if (hint != c->bypass_compositor) {
				c->bypass_compositor = hint;
				compositor_bypass_window(c, hint == 1);
			}
		}
#endif
	}
	wmstate_update();
}

void
resizerequest(xcb_generic_event_t *e)
{
	xcb_resize_request_event_t *ev = (xcb_resize_request_event_t *) e;
	Client                     *i;

	if ((i = wintosystrayicon(ev->window))) {
		updatesystrayicongeom(i, ev->width, ev->height);
		resizebarwin(g_awm_selmon);
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
			setclientstate(c, XCB_ICCCM_WM_STATE_WITHDRAWN);
		else
			unmanage(c, 0);
	} else if ((c = wintosystrayicon(ev->window))) {
		/* KLUDGE! sometimes icons occasionally unmap their windows, but do
		 * _not_ destroy them. We map those windows back */
		{
			uint32_t above = XCB_STACK_MODE_ABOVE;
			g_wm_backend->map(&g_plat, c->win);
			g_wm_backend->configure_win(
			    &g_plat, c->win, XCB_CONFIG_WINDOW_STACK_MODE, &above);
		}
		updatesystray();
	}
}

void
updatenumlockmask(void)
{
	g_wm_backend->update_numlock_mask(&g_plat);
}

/* Return a human-readable string for a base X11 error code (1-17).
 * Extension errors (codes > 127) are labelled generically. */
static const char *
xcb_error_text(uint8_t error_code)
{
	/* X11 core error codes — xproto.h §XCB_REQUEST … XCB_IMPLEMENTATION */
	static const char *names[] = {
		/* 0 */ "Success",
		/* 1 */ "BadRequest",
		/* 2 */ "BadValue",
		/* 3 */ "BadWindow",
		/* 4 */ "BadPixmap",
		/* 5 */ "BadAtom",
		/* 6 */ "BadCursor",
		/* 7 */ "BadFont",
		/* 8 */ "BadMatch",
		/* 9 */ "BadDrawable",
		/* 10 */ "BadAccess",
		/* 11 */ "BadAlloc",
		/* 12 */ "BadColor",
		/* 13 */ "BadGC",
		/* 14 */ "BadIDChoice",
		/* 15 */ "BadName",
		/* 16 */ "BadLength",
		/* 17 */ "BadImplementation",
	};
	if (error_code < (sizeof names / sizeof names[0]))
		return names[error_code];
	return "ExtensionError";
}

/* XCB async error handler — called from x_dispatch_cb() when the event
 * response_type is 0 (error packet).  Mirrors the old Xlib xerror() logic
 * but operates entirely on xcb_generic_error_t fields.
 *
 * Return values:  0 = benign, silently ignored.
 *                 1 = unexpected; logged via awm_error but execution
 * continues.
 *
 * Unlike the old Xlib handler we do NOT call exit() on unexpected errors —
 * the async nature of XCB means some races are unavoidable and a WM must
 * survive them.  Truly fatal conditions (X server death) are caught via the
 * HUP/ERR path in platform_source_dispatch(). */
int
xcb_error_handler(xcb_generic_error_t *e)
{
	uint8_t req = e->major_code;
	uint8_t err = e->error_code;

	/* Whitelist benign async errors that arise routinely in a WM:
	 * - BadWindow:   window destroyed between our request and the reply
	 * - SetInputFocus + BadMatch:   window became unviewable/unmapped
	 * - PolyText8/PolyFillRectangle/PolySegment/CopyArea + BadDrawable:
	 *     drawable destroyed while we were drawing
	 * - ConfigureWindow + BadMatch: sibling ordering race
	 * - GrabButton/GrabKey + BadAccess: another client owns the grab */
	if (err == XCB_WINDOW || (req == X_SetInputFocus && err == XCB_MATCH) ||
	    (req == X_PolyText8 && err == XCB_DRAWABLE) ||
	    (req == X_PolyFillRectangle && err == XCB_DRAWABLE) ||
	    (req == X_PolySegment && err == XCB_DRAWABLE) ||
	    (req == X_ConfigureWindow && err == XCB_MATCH) ||
	    (req == X_GrabButton && err == XCB_ACCESS) ||
	    (req == X_GrabKey && err == XCB_ACCESS) ||
	    (req == X_CopyArea && err == XCB_DRAWABLE))
		return 0;
#ifdef COMPOSITOR
	/* Transient XRender errors (BadPicture, BadPictFormat) arise when a GL
	 * window exits while a compositor repaint is in flight. */
	{
		int render_req, render_err;
		compositor_xrender_errors(&render_req, &render_err);
		if (render_req > 0 && req == (uint8_t) render_req &&
		    (err == (uint8_t) render_err             /* BadPicture    */
		        || err == (uint8_t) (render_err + 1) /* BadPictFormat */
		        || err == XCB_DRAWABLE || err == XCB_PIXMAP))
			return 0;
	}
	/* Transient XDamage errors (BadDamage) when a window is destroyed
	 * while we call xcb_damage_destroy on its damage handle.
	 * BadIDChoice on XDamage Subtract arises when a stale DAMAGE_NOTIFY
	 * event fires after comp_free_win() already destroyed the damage
	 * object — the event was queued before the destroy, so we still try
	 * to ack it and get an async BadIDChoice.  Both are benign. */
	{
		int damage_req, damage_err;
		compositor_damage_errors(&damage_req, &damage_err);
		if (damage_err > 0 && err == (uint8_t) damage_err)
			return 0;
		if (damage_req > 0 && req == (uint8_t) damage_req &&
		    err == XCB_ID_CHOICE)
			return 0;
	}
	/* Transient X Present errors (BadIDChoice) arise when a stale
	 * PresentCompleteNotify or similar event fires after comp_free_win()
	 * already destroyed the Present event subscription (EID).  The X
	 * server sends BadIDChoice on the next xcb_present_select_input
	 * referencing that EID.  Benign — ignore. */
	{
		int present_req;
		compositor_present_errors(&present_req);
		if (present_req > 0 && req == (uint8_t) present_req &&
		    err == XCB_ID_CHOICE)
			return 0;
	}
	/* GLX errors are stubs — compositor_glx_errors always returns -1 */
	{
		int glx_req, glx_err;
		compositor_glx_errors(&glx_req, &glx_err);
		(void) glx_req;
		(void) glx_err;
	}
#endif
	awm_error("X11 async error: %s (major=%d minor=%d error=%d resource=0x%x)",
	    xcb_error_text(err), (int) req, (int) e->minor_code, (int) err,
	    (unsigned) e->resource_id);
	return 1;
}
