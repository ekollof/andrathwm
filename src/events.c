/* AndrathWM - X event handlers
 * See LICENSE file for copyright and license details. */

#include <assert.h>
#include "events.h"
#include "awm.h"
#include <xkbcommon/xkbcommon-keysyms.h>
#include "client.h"
#include "compositor.h"
#include "monitor.h"
#include "spawn.h"
#include "switcher.h"
#include "systray.h"
#ifdef BACKEND_X11
#include "ewmh.h"
#endif
#include "wmstate.h"
#include "wm_properties.h"
#include "xrdb.h"
#include "config.h"

void
buttonpress(AwmEvent *e)
{
	unsigned int i, x, click;
	Arg          arg = { 0 };
	Client      *c;
	Monitor     *m;

	click = ClkRootWin;
	/* focus monitor if necessary */
	if ((m = wintomon(e->window)) && m != g_awm_selmon) {
		unfocus(g_awm_selmon->sel, 1);
		g_awm_set_selmon(m);
		focus(NULL);
	}
	if (e->window == g_awm_selmon->barwin) {
		int stw = 0; /* systray width — 0 on non-X11 */
#ifdef BACKEND_X11
		stw = (int) getsystraywidth();
#endif
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
			if (e->button.event_x < (int) (x + (unsigned int) tw)) {
				click  = ClkTagBar;
				arg.ui = 1 << i;
				break;
			}
			x += (unsigned int) tw;
		}

		if (i >= LENGTH(tags) &&
		    e->button.event_x <
		        (int) (x + (unsigned int) TEXTW(g_awm_selmon->ltsymbol)))
			click = ClkLtSymbol;
		else if (e->button.event_x > g_awm_selmon->ww -
		        g_render_backend->draw_statusd(drw, 0, 0, 0, 0, stext) - stw)
			click = ClkStatusText;
		else if (i >= LENGTH(tags)) {
			/* Awesomebar - find which window was clicked */
			click = ClkWinTitle;
			c     = NULL;

			/* Add layout symbol width to x position */
			x += (unsigned int) TEXTW(g_awm_selmon->ltsymbol);

			int n = 0;
			for (Client *t = g_awm.clients_head; t; t = t->next)
				if (t->tags & m->tagset[m->seltags])
					n++;

			if (n > 0) {
				int tw =
				    g_render_backend->draw_statusd(drw, 0, 0, 0, 0, stext);
				int remainder = m->ww - tw - stw - (int) x;
				int tabw      = remainder / n;
				int cx        = (int) x;

				for (Client *t = g_awm.clients_head; t; t = t->next) {
					if (!(t->tags & m->tagset[m->seltags]))
						continue;
					if (e->button.event_x >= cx &&
					    e->button.event_x < cx + tabw) {
						c = t;
						break;
					}
					cx += tabw;
				}
			}

			if (c)
				arg.v = c;
		}
	} else if ((c = wintoclient(e->window))) {
		focus(c);
		restack(g_awm_selmon);
		g_wm_backend->allow_events(
		    &g_plat, AWM_ALLOW_REPLAY_POINTER, WM_CURRENT_TIME);
		g_wm_backend->flush(&g_plat);
		click = ClkClientWin;
	}
#ifdef STATUSNOTIFIER
	/* Check if click is on SNI icon */
	else {
		SNIItem *sni_item = sni_find_item_by_window(e->window);
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
			    &g_plat, AWM_ALLOW_ASYNC_POINTER, e->button.time);
			g_wm_backend->flush(&g_plat);
			sni_handle_click(e->window, e->button.button, e->button.root_x,
			    e->button.root_y, e->button.time);
			return; /* Don't process further */
		}
	}
#endif
	for (i = 0; i < LENGTH(buttons); i++)
		if (click == buttons[i].click && buttons[i].func &&
		    buttons[i].button == e->button.button &&
		    CLEANMASK(buttons[i].mask) == CLEANMASK(e->button.state))
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
clientmessage(AwmEvent *e)
{
	Client      *c = wintoclient(e->window);
	unsigned int i;

#ifdef BACKEND_X11
	if (showsystray && systray && e->window == systray->win &&
	    e->client_message.msg_type == g_plat.netatom[NetSystemTrayOP]) {
		/* add systray icons */
		if (e->client_message.data[1] == SYSTEM_TRAY_REQUEST_DOCK) {
			if (!(c = (Client *) calloc(1, sizeof(Client))))
				die("fatal: could not malloc() %u bytes\n", sizeof(Client));
			if (!(c->win = (WinId) e->client_message.data[2])) {
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
				uint32_t mask = AWM_EVENT_MASK_STRUCTURE_NOTIFY |
				    AWM_EVENT_MASK_PROPERTY_CHANGE |
				    AWM_EVENT_MASK_RESIZE_REDIRECT;
				g_wm_backend->change_attr(
				    &g_plat, c->win, AWM_CW_EVENT_MASK, &mask);
			}
			g_wm_backend->reparent_window(&g_plat, c->win, systray->win, 0, 0);
			/* use bar background so icon blends with the bar */
			{
				uint32_t bg = clr_to_argb(&scheme[SchemeNorm][ColBg]);
				g_wm_backend->change_attr(
				    &g_plat, c->win, AWM_CW_BACK_PIXEL, &bg);
			}
			/* Send XEMBED_EMBEDDED_NOTIFY to complete embedding per spec.
			 * data1 = embedder window, data2 = protocol version */
			sendevent(c->win, g_plat.xatom[Xembed],
			    AWM_EVENT_MASK_STRUCTURE_NOTIFY, WM_CURRENT_TIME,
			    XEMBED_EMBEDDED_NOTIFY, 0, systray->win, XEMBED_VERSION);
			g_wm_backend->flush(&g_plat);
			resizebarwin(g_awm_selmon);
			updatesystray();
			setclientstate(c, AWM_WM_STATE_NORMAL);
		}
		return;
	}
#endif /* BACKEND_X11 */

	if (!c)
		return;
	if (e->client_message.msg_type == g_plat.netatom[NetWMState]) {
		if (e->client_message.data[1] == g_plat.netatom[NetWMFullscreen] ||
		    e->client_message.data[2] == g_plat.netatom[NetWMFullscreen])
			setfullscreen(c,
			    (e->client_message.data[0] == 1 /* _NET_WM_STATE_ADD    */
			        ||
			        (e->client_message.data[0] == 2 /* _NET_WM_STATE_TOGGLE */
			            && !c->isfullscreen)));
	} else if (e->client_message.msg_type == g_plat.netatom[NetActiveWindow]) {
		for (i = 0; i < LENGTH(tags) && !((1 << i) & c->tags); i++)
			;
		if (i < LENGTH(tags)) {
			const Arg a = { .ui = 1 << i };
			g_awm_set_selmon(c->mon);
			view(&a);
			focus(c);
			restack(g_awm_selmon);
		}
	} else if (e->client_message.msg_type == g_plat.netatom[NetCloseWindow]) {
		/* _NET_CLOSE_WINDOW client message */
		if (!wmprop_send_event(c->win, g_plat.wmatom[WMDelete], 0,
		        g_plat.wmatom[WMDelete], WM_CURRENT_TIME, 0, 0, 0)) {
			g_wm_backend->kill_client_hard(&g_plat, c->win);
		}
		g_wm_backend->flush(&g_plat);
	} else if (e->client_message.msg_type ==
	    g_plat.netatom[NetMoveResizeWindow]) {
		/* _NET_MOVERESIZE_WINDOW client message */
		int          x, y, w, h;
		unsigned int gravity_flags = e->client_message.data[0];

		x = (gravity_flags & (1 << 8)) ? (int) e->client_message.data[1]
		                               : c->x;
		y = (gravity_flags & (1 << 9)) ? (int) e->client_message.data[2]
		                               : c->y;
		w = (gravity_flags & (1 << 10)) ? (int) e->client_message.data[3]
		                                : c->w;
		h = (gravity_flags & (1 << 11)) ? (int) e->client_message.data[4]
		                                : c->h;

		resize(c, x, y, w, h, 1);
	}
}

void
configurenotify(AwmEvent *e)
{
	Monitor *m;
	Client  *c;

	if (e->window == g_plat.root) {
		g_plat.sw = e->configure.w;
		g_plat.sh = e->configure.h;
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
						    AWM_CONFIG_WIN_X | AWM_CONFIG_WIN_Y |
						        AWM_CONFIG_WIN_WIDTH | AWM_CONFIG_WIN_HEIGHT,
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
			wmprop_update_workarea(m);
		}
#ifdef COMPOSITOR
		compositor_notify_screen_resize();
#endif
	}
	wmstate_update();
}

void
configurerequest(AwmEvent *e)
{
	Client  *c;
	Monitor *m;

	if ((c = wintoclient(e->window))) {
		if (c->isfullscreen) {
			/* Don't let clients move/resize themselves while fullscreen;
			 * just echo back the current geometry so they don't hang. */
			configure(c);
			g_wm_backend->flush(&g_plat);
			return;
		}
		if (e->configure_req.value_mask & AWM_CONFIG_WIN_BORDER_WIDTH)
			c->bw = e->configure_req.bw;
		else if (c->isfloating ||
		    !g_awm_selmon->lt[g_awm_selmon->sellt]->arrange) {
			m = c->mon;
			if (!c->issteam) {
				if (e->configure_req.value_mask & AWM_CONFIG_WIN_X) {
					c->oldx = c->x;
					c->x    = m->mx + e->configure_req.x;
				}
				if (e->configure_req.value_mask & AWM_CONFIG_WIN_Y) {
					c->oldy = c->y;
					c->y    = m->my + e->configure_req.y;
				}
			}
			if (e->configure_req.value_mask & AWM_CONFIG_WIN_WIDTH) {
				c->oldw = c->w;
				c->w    = e->configure_req.w;
			}
			if (e->configure_req.value_mask & AWM_CONFIG_WIN_HEIGHT) {
				c->oldh = c->h;
				c->h    = e->configure_req.h;
			}
			if ((c->x + c->w) > m->mx + m->mw && c->isfloating)
				c->x = m->mx +
				    (m->mw / 2 - WIDTH(c) / 2); /* center in x direction */
			if ((c->y + c->h) > m->my + m->mh && c->isfloating)
				c->y = m->my +
				    (m->mh / 2 - HEIGHT(c) / 2); /* center in y direction */
			if ((e->configure_req.value_mask &
			        (AWM_CONFIG_WIN_X | AWM_CONFIG_WIN_Y)) &&
			    !(e->configure_req.value_mask &
			        (AWM_CONFIG_WIN_WIDTH | AWM_CONFIG_WIN_HEIGHT)))
				configure(c);
			if (ISVISIBLE(c, m)) {
				uint32_t xywh[4] = {
					(uint32_t) (int32_t) c->x,
					(uint32_t) (int32_t) c->y,
					(uint32_t) c->w,
					(uint32_t) c->h,
				};
				g_wm_backend->configure_win(&g_plat, c->win,
				    AWM_CONFIG_WIN_X | AWM_CONFIG_WIN_Y |
				        AWM_CONFIG_WIN_WIDTH | AWM_CONFIG_WIN_HEIGHT,
				    xywh);
			}
		} else
			configure(c);
	} else {
		/* Pass unmanaged window configure requests straight through.
		 * Build the value array in ascending bit-position order. */
		uint32_t vals[7];
		int      n = 0;
		if (e->configure_req.value_mask & AWM_CONFIG_WIN_X)
			vals[n++] = (uint32_t) (int32_t) e->configure_req.x;
		if (e->configure_req.value_mask & AWM_CONFIG_WIN_Y)
			vals[n++] = (uint32_t) (int32_t) e->configure_req.y;
		if (e->configure_req.value_mask & AWM_CONFIG_WIN_WIDTH)
			vals[n++] = (uint32_t) e->configure_req.w;
		if (e->configure_req.value_mask & AWM_CONFIG_WIN_HEIGHT)
			vals[n++] = (uint32_t) e->configure_req.h;
		if (e->configure_req.value_mask & AWM_CONFIG_WIN_BORDER_WIDTH)
			vals[n++] = (uint32_t) e->configure_req.bw;
		if (e->configure_req.value_mask & AWM_CONFIG_WIN_SIBLING)
			vals[n++] = (uint32_t) e->configure_req.sibling;
		if (e->configure_req.value_mask & AWM_CONFIG_WIN_STACK_MODE)
			vals[n++] = (uint32_t) e->configure_req.stack_mode;
		if (n > 0)
			g_wm_backend->configure_win(
			    &g_plat, e->window, e->configure_req.value_mask, vals);
	}
	g_wm_backend->flush(&g_plat);
}

void
destroynotify(AwmEvent *e)
{
	Client *c;

	if ((c = wintoclient(e->window)))
		unmanage(c, 1);
#ifdef BACKEND_X11
	else if ((c = wintosystrayicon(e->window))) {
		removesystrayicon(c);
		resizebarwin(g_awm_selmon);
		updatesystray();
	}
#endif /* BACKEND_X11 */
}

void
enternotify(AwmEvent *e)
{
	Client  *c;
	Monitor *m;

	if ((e->enter_leave_focus.mode != AWM_NOTIFY_MODE_NORMAL ||
	        e->enter_leave_focus.detail == AWM_NOTIFY_DETAIL_INFERIOR) &&
	    e->window != g_plat.root)
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
		if (e->window == m->barwin) {
			bar_hover_enter(m);
			return;
		}
	}

	c = wintoclient(e->window);
	m = c ? c->mon : wintomon(e->window);
	if (m != g_awm_selmon) {
		unfocus(g_awm_selmon->sel, 1);
		g_awm_set_selmon(m);
	} else if (!c || c == g_awm_selmon->sel)
		return;
	focus(c);
	wmstate_update();
}

void
leavenotify(AwmEvent *e)
{
	Monitor *m;

	if (e->enter_leave_focus.mode != AWM_NOTIFY_MODE_NORMAL)
		return;

	/* Bar un-hover — hide window preview popup.
	 * Mirror the enternotify() guards: do not dismiss the preview
	 * while the launcher or switcher owns focus, otherwise hovering
	 * onto those overlapping windows would prematurely hide it. */
	if (launcher_visible || switcher_active())
		return;

	FOR_EACH_MON(m)
	{
		if (e->window == m->barwin) {
			bar_hover_leave();
			return;
		}
	}
}

void
expose(AwmEvent *e)
{
	Monitor *m;

	if (e->expose.count == 0 && (m = wintomon(e->window))) {
		drawbar(m);
#ifdef BACKEND_X11
		if (m == g_awm_selmon)
			updatesystray();
#endif
	}
}

/* there are some broken focus acquiring clients needing extra handling */
void
focusin(AwmEvent *e)
{
	/* While the switcher is visible it must keep keyboard focus; prevent awm
	 * from stealing it back to the previously focused client. */
	if (switcher_active())
		return;

	if (!g_awm_selmon->sel || e->window == g_awm_selmon->sel->win)
		return;

	/* Allow focus to move to a child window of the currently focused client
	 * (e.g. an in-page widget, chat overlay, or popup inside a fullscreen
	 * browser window).  Without this guard, focusin() would steal focus back
	 * to the top-level client window, making those widgets unreachable. */
	if (g_wm_backend->is_window_descendant(
	        &g_plat, e->window, g_awm_selmon->sel->win))
		return;

	/* Allow focus to move to override-redirect windows (e.g. the launcher).
	 * These are unmanaged by the WM by design; stealing focus back would
	 * make them permanently unfocusable. */
	{
		int or = 0;
		if (g_wm_backend->get_override_redirect(&g_plat, e->window, & or) &&
		    or)
			return;
	}

	wmprop_set_focus(g_awm_selmon->sel);
}

void
grabkeys(void)
{
	g_wm_backend->grab_keys_full(&g_plat);
}

void
keypress(AwmEvent *e)
{
	unsigned int i;
	KeySym       keysym;

	last_event_time = e->key.time;
	keysym = g_wm_backend->get_keysym(&g_plat, (WmKeycode) e->key.keycode, 0);

	/* While the switcher is open, handle Tab/Escape/Return directly here
	 * and suppress the normal keybinding dispatch.  The switcher GTK window
	 * cannot receive key events because awm holds the X passive grab. */
	if (switcher_active()) {
		if (keysym == XKB_KEY_Escape) {
			switcher_cancel_xkb(NULL);
			return;
		}
		if (keysym == XKB_KEY_Return || keysym == XKB_KEY_KP_Enter) {
			switcher_confirm_xkb(NULL);
			return;
		}
		if (keysym == XKB_KEY_Tab) {
			/* Shift+Tab — state has Shift modifier */
			if (e->key.state & WM_MOD_SHIFT)
				switcher_prev(NULL);
			else
				switcher_next(NULL);
			return;
		}
		/* Any other key while switcher is open: ignore */
		return;
	}

	for (i = 0; i < LENGTH(keys); i++)
		if (keysym == keys[i].keysym &&
		    CLEANMASK(keys[i].mod) == CLEANMASK(e->key.state) && keys[i].func)
			keys[i].func(&(keys[i].arg));
}

void
keyrelease(AwmEvent *e)
{
	KeySym keysym =
	    g_wm_backend->get_keysym(&g_plat, (WmKeycode) e->key.keycode, 0);

	/* Confirm the switcher when the modifier that opened it is released */
	if (switcher_active()) {
		if (keysym == XKB_KEY_Alt_L || keysym == XKB_KEY_Alt_R ||
		    keysym == XKB_KEY_Super_L || keysym == XKB_KEY_Super_R) {
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
	if (gettextprop(g_plat.root, AWM_ATOM_WM_NAME, fsignal, sizeof(fsignal))) {
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
			for (i = signum = 0; i < (int) strlen(str_signum); i++) {
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
mappingnotify(AwmEvent *e)
{
	g_wm_backend->refresh_keyboard_mapping(&g_plat, e);
	if (e->mapping.request == AWM_MAPPING_KEYBOARD)
		grabkeys();
}

void
maprequest(AwmEvent *e)
{
	Client *i;
#ifdef BACKEND_X11
	if ((i = wintosystrayicon(e->window))) {
		/* Systray icon requested mapping - handle via updatesystray */
		resizebarwin(g_awm_selmon);
		updatesystray();
		return;
	}
#else
	(void) i;
#endif /* BACKEND_X11 */

	{
		int override = 0;
		if (!g_wm_backend->get_override_redirect(
		        &g_plat, e->window, &override))
			return;
		if (override)
			return;
	}
	if (!wintoclient(e->window)) {
		int gx, gy, gw, gh, gbw;
		if (g_wm_backend->get_geometry(
		        &g_plat, e->window, &gx, &gy, &gw, &gh, &gbw))
			manage(e->window, gx, gy, gw, gh, gbw);
	}
}

void
motionnotify(AwmEvent *e)
{
	static Monitor *mon = NULL;
	Monitor        *m;

	if (e->window != g_plat.root)
		return;
	if ((m = recttomon(e->motion.root_x, e->motion.root_y, 1, 1)) != mon &&
	    mon) {
		unfocus(g_awm_selmon->sel, 1);
		g_awm_set_selmon(m);
		focus(NULL);
	}
	mon = m;
	wmstate_update();
}

void
propertynotify(AwmEvent *e)
{
	Client *c;
	WinId   trans;

#ifdef BACKEND_X11
	if ((c = wintosystrayicon(e->window))) {
		if (e->property.atom == AWM_ATOM_WM_NORMAL_HINTS) {
			updatesizehints(c);
			updatesystrayicongeom(c, c->w, c->h);
		} else
			updatesystrayiconstate(c, e->property.atom);
		resizebarwin(g_awm_selmon);
		updatesystray();
	}
#endif /* BACKEND_X11 */

	if ((e->window == g_plat.root) && (e->property.atom == AWM_ATOM_WM_NAME)) {
		(void) fake_signal();
		return;
	} else if (e->property.state == AWM_PROPERTY_DELETE)
		return; /* ignore */
	else if ((c = wintoclient(e->window))) {
		switch (e->property.atom) {
		default:
			break;
		case AWM_ATOM_WM_TRANSIENT_FOR:
			if (!c->isfloating &&
			    (trans = g_wm_backend->get_wm_transient_for(
			         &g_plat, c->win)) != WIN_NONE &&
			    (c->isfloating = (wintoclient(trans)) != NULL))
				arrange(c->mon);
			break;
		case AWM_ATOM_WM_NORMAL_HINTS:
			c->hintsvalid = 0;
			break;
		case AWM_ATOM_WM_HINTS:
			updatewmhints(c);
			barsdirty = 1; /* defer redraw */
			break;
		}
		if (e->property.atom == AWM_ATOM_WM_NAME ||
		    e->property.atom == g_plat.netatom[NetWMName]) {
			updatetitle(c);
			if (c == c->mon->sel)
				barsdirty = 1; /* defer redraw */
		}
		if (e->property.atom == g_plat.netatom[NetWMWindowType])
			updatewindowtype(c);
#ifdef COMPOSITOR
		if (e->property.atom == g_plat.netatom[NetWMWindowOpacity]) {
			unsigned long raw = (unsigned long) getatomprop(
			    c, g_plat.netatom[NetWMWindowOpacity]);
			compositor_set_opacity(c, raw);
		}
		if (e->property.atom == g_plat.netatom[NetWMBypassCompositor]) {
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
resizerequest(AwmEvent *e)
{
	Client *i;

#ifdef BACKEND_X11
	if ((i = wintosystrayicon(e->window))) {
		updatesystrayicongeom(i, e->resize_request.w, e->resize_request.h);
		resizebarwin(g_awm_selmon);
		updatesystray();
	}
#else
	(void) i;
#endif /* BACKEND_X11 */
}

void
unmapnotify(AwmEvent *e)
{
	Client *c;

	if ((c = wintoclient(e->window))) {
		if (e->unmap_destroy.is_send_event)
			setclientstate(c, AWM_WM_STATE_WITHDRAWN);
		else
			unmanage(c, 0);
#ifdef BACKEND_X11
	} else if ((c = wintosystrayicon(e->window))) {
		/* KLUDGE! sometimes icons occasionally unmap their windows, but do
		 * _not_ destroy them. We map those windows back */
		{
			uint32_t above = AWM_STACK_MODE_ABOVE;
			g_wm_backend->map(&g_plat, c->win);
			g_wm_backend->configure_win(
			    &g_plat, c->win, AWM_CONFIG_WIN_STACK_MODE, &above);
		}
		updatesystray();
#endif /* BACKEND_X11 */
	}
}

void
updatenumlockmask(void)
{
	g_wm_backend->update_numlock_mask(&g_plat);
}
