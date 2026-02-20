/* AndrathWM - client management
 * See LICENSE file for copyright and license details. */

#include <stdint.h>
#include <xcb/xcb.h>

#include "client.h"
#include "awm.h"
#include "events.h"
#include "ewmh.h"
#include "monitor.h"
#include "spawn.h"
#include "systray.h"
#include "xrdb.h"
#include "config.h"

/* module-local strings */
static const char broken[] = "broken";

void
applyrules(Client *c)
{
	const char *class, *instance;
	unsigned int i;
	const Rule  *r;
	Monitor     *m;

	/* rule matching */
	c->iscentered = 0;
	c->isfloating = 0;
	c->ishidden   = 0;
	c->tags       = 0;
	c->scratchkey = 0;

	/* Fetch WM_CLASS via XCB: value is "instance\0class\0" (STRING).
	 * Parse the two null-separated fields from the raw reply data. */
	char cls_buf[256]  = { 0 };
	char inst_buf[256] = { 0 };
	{
		xcb_get_property_cookie_t pck =
		    xcb_get_property(XGetXCBConnection(dpy), 0, c->win,
		        XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 512);
		xcb_get_property_reply_t *pr =
		    xcb_get_property_reply(XGetXCBConnection(dpy), pck, NULL);
		if (pr && xcb_get_property_value_length(pr) > 0) {
			const char *val = (const char *) xcb_get_property_value(pr);
			int         len = xcb_get_property_value_length(pr);
			/* First field: instance (res_name) */
			int inst_len = (int) strnlen(val, (size_t) len);
			if (inst_len > 0)
				snprintf(inst_buf, sizeof inst_buf, "%.*s", inst_len, val);
			/* Second field: class (res_class) starts after first \0 */
			if (inst_len + 1 < len) {
				const char *cls_start = val + inst_len + 1;
				int         cls_len =
				    (int) strnlen(cls_start, (size_t) (len - inst_len - 1));
				if (cls_len > 0)
					snprintf(
					    cls_buf, sizeof cls_buf, "%.*s", cls_len, cls_start);
			}
		}
		free(pr);
	}
	class    = cls_buf[0] ? cls_buf : broken;
	instance = inst_buf[0] ? inst_buf : broken;

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
	xcb_configure_notify_event_t ce;

	ce.response_type     = XCB_CONFIGURE_NOTIFY;
	ce.pad0              = 0;
	ce.sequence          = 0;
	ce.event             = c->win;
	ce.window            = c->win;
	ce.above_sibling     = XCB_NONE;
	ce.x                 = (int16_t) c->x;
	ce.y                 = (int16_t) c->y;
	ce.width             = (uint16_t) c->w;
	ce.height            = (uint16_t) c->h;
	ce.border_width      = (uint16_t) c->bw;
	ce.override_redirect = 0;
	ce.pad1              = 0;
	xcb_send_event(XGetXCBConnection(dpy), 0, c->win,
	    XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *) &ce);
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
		{
			xcb_connection_t *xcb = XGetXCBConnection(dpy);
			uint32_t          pix = scheme[SchemeSel][ColBorder].pixel;
			xcb_change_window_attributes(
			    xcb, c->win, XCB_CW_BORDER_PIXEL, &pix);
			if (!selmon->pertag->drawwithgaps[selmon->pertag->curtag] &&
			    !c->isfloating) {
				uint32_t vals[2];
				vals[0] = (uint32_t) selmon->barwin;
				vals[1] = XCB_STACK_MODE_BELOW;
				xcb_configure_window(xcb, c->win,
				    XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
				    vals);
			}
		}
		setfocus(c);
	} else {
		xcb_set_input_focus(XGetXCBConnection(dpy),
		    XCB_INPUT_FOCUS_POINTER_ROOT, selmon->barwin, XCB_CURRENT_TIME);
		xcb_delete_property(
		    XGetXCBConnection(dpy), root, netatom[NetActiveWindow]);
	}
	selmon->sel = c;
	if (selmon->lt[selmon->sellt]->arrange == monocle)
		arrangemon(selmon);
	barsdirty = 1;
#ifdef COMPOSITOR
	/* Dirty the border region of both the newly focused and previously
	 * focused client so the compositor repaints them in the correct colour. */
	compositor_focus_window(c);
	/* Re-evaluate fullscreen unredirect — the topmost window may have
	 * changed. */
	compositor_check_unredirect();
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
	xcb_connection_t         *xc  = XGetXCBConnection(dpy);
	xcb_atom_t                req = (prop == (Atom) xatom[XembedInfo])
	                   ? (xcb_atom_t) xatom[XembedInfo]
	                   : XCB_ATOM_ATOM;
	xcb_get_property_cookie_t ck  = xcb_get_property(
        xc, 0, (xcb_window_t) c->win, (xcb_atom_t) prop, req, 0, 1);
	xcb_get_property_reply_t *r    = xcb_get_property_reply(xc, ck, NULL);
	Atom                      atom = None;

	if (r && xcb_get_property_value_length(r) >= (int) sizeof(xcb_atom_t)) {
		atom = (Atom) * (xcb_atom_t *) xcb_get_property_value(r);
	}
	free(r);
	return atom;
}

int
getrootptr(int *x, int *y)
{
	xcb_query_pointer_cookie_t ck =
	    xcb_query_pointer(XGetXCBConnection(dpy), root);
	xcb_query_pointer_reply_t *r =
	    xcb_query_pointer_reply(XGetXCBConnection(dpy), ck, NULL);
	if (!r)
		return 0;
	*x = r->root_x;
	*y = r->root_y;
	free(r);
	return 1;
}

long
getstate(Window w)
{
	xcb_connection_t         *xc = XGetXCBConnection(dpy);
	xcb_get_property_cookie_t ck = xcb_get_property(xc, 0, (xcb_window_t) w,
	    (xcb_atom_t) wmatom[WMState], (xcb_atom_t) wmatom[WMState], 0, 2);
	xcb_get_property_reply_t *r  = xcb_get_property_reply(xc, ck, NULL);
	long                      result = -1;

	if (r && xcb_get_property_value_length(r) > 0) {
		result = (long) *(uint32_t *) xcb_get_property_value(r);
	}
	free(r);
	return result;
}

int
gettextprop(Window w, Atom atom, char *text, unsigned int size)
{
	xcb_connection_t                   *xc;
	xcb_get_property_cookie_t           ck;
	xcb_icccm_get_text_property_reply_t prop;
	unsigned int                        len;

	if (!text || size == 0)
		return 0;
	text[0] = '\0';
	xc      = XGetXCBConnection(dpy);
	ck = xcb_icccm_get_text_property(xc, (xcb_window_t) w, (xcb_atom_t) atom);
	if (!xcb_icccm_get_text_property_reply(xc, ck, &prop, NULL))
		return 0;
	if (prop.name_len > 0 && prop.name) {
		len = prop.name_len < size - 1 ? prop.name_len : size - 1;
		memcpy(text, prop.name, len);
		text[len] = '\0';
	}
	xcb_icccm_get_text_property_reply_wipe(&prop);
	return 1;
}

cairo_surface_t *
getwmicon(Window w, int size)
{
	xcb_connection_t         *xc = XGetXCBConnection(dpy);
	xcb_get_property_cookie_t ck = xcb_get_property(xc, 0, (xcb_window_t) w,
	    (xcb_atom_t) netatom[NetWMIcon], XCB_ATOM_ANY, 0, UINT32_MAX / 4);
	xcb_get_property_reply_t *r  = xcb_get_property_reply(xc, ck, NULL);
	cairo_surface_t          *surface = NULL;

	if (!r || xcb_get_property_value_length(r) == 0) {
		free(r);
		return NULL;
	}

	{
		int            vlen   = xcb_get_property_value_length(r);
		unsigned long *data   = (unsigned long *) xcb_get_property_value(r);
		unsigned long  nitems = (unsigned long) vlen / sizeof(unsigned long);

		if (nitems > 2) {
			unsigned long icon_w = data[0];
			unsigned long icon_h = data[1];

			if (nitems >= 2 + icon_w * icon_h) {
				cairo_surface_t *src;
				cairo_t         *cr;
				unsigned char   *argb_data;
				unsigned long    i;
				int              stride;

				awm_debug("extracting %lux%lu icon, nitems=%lu", icon_w,
				    icon_h, nitems);

				stride =
				    cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, icon_w);
				argb_data = calloc(icon_h, stride);
				if (!argb_data) {
					free(r);
					return NULL;
				}

				for (i = 0; i < icon_w * icon_h; i++) {
					unsigned long pixel = data[2 + i];
					unsigned char a     = (pixel >> 24) & 0xff;
					unsigned char rv    = (pixel >> 16) & 0xff;
					unsigned char g     = (pixel >> 8) & 0xff;
					unsigned char b     = pixel & 0xff;

					unsigned char *q =
					    argb_data + (i / icon_w) * stride + (i % icon_w) * 4;

					if (a == 0) {
						q[0] = q[1] = q[2] = q[3] = 0;
					} else if (a == 255) {
						q[0] = b;
						q[1] = g;
						q[2] = rv;
						q[3] = a;
					} else {
						q[0] = (b * a) / 255;
						q[1] = (g * a) / 255;
						q[2] = (rv * a) / 255;
						q[3] = a;
					}
				}

				awm_debug("first 4 pixels (BGRA): [%02x%02x%02x%02x] "
				          "[%02x%02x%02x%02x] "
				          "[%02x%02x%02x%02x] [%02x%02x%02x%02x]",
				    argb_data[3], argb_data[2], argb_data[1], argb_data[0],
				    argb_data[7], argb_data[6], argb_data[5], argb_data[4],
				    argb_data[11], argb_data[10], argb_data[9], argb_data[8],
				    argb_data[15], argb_data[14], argb_data[13],
				    argb_data[12]);

				src = cairo_image_surface_create_for_data(
				    argb_data, CAIRO_FORMAT_ARGB32, icon_w, icon_h, stride);

				surface = cairo_image_surface_create(
				    CAIRO_FORMAT_ARGB32, size, size);
				cr = cairo_create(surface);

				cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
				cairo_paint(cr);
				cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

				if (icon_w != (unsigned long) size ||
				    icon_h != (unsigned long) size) {
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
	}

	free(r);
	return surface;
}

void
grabbuttons(Client *c, int focused)
{
	updatenumlockmask();
	{
		unsigned int      i, j;
		unsigned int      modifiers[] = { 0, LockMask, numlockmask,
			     numlockmask | LockMask };
		xcb_connection_t *xc          = XGetXCBConnection(dpy);

		xcb_ungrab_button(xc, XCB_BUTTON_INDEX_ANY, c->win, XCB_MOD_MASK_ANY);
		if (!focused)
			xcb_grab_button(xc, 0 /*owner_events*/, c->win,
			    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE,
			    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_SYNC, XCB_WINDOW_NONE,
			    XCB_CURSOR_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);
		for (i = 0; i < LENGTH(buttons); i++)
			if (buttons[i].click == ClkClientWin)
				for (j = 0; j < LENGTH(modifiers); j++)
					xcb_grab_button(xc, 0 /*owner_events*/, c->win,
					    XCB_EVENT_MASK_BUTTON_PRESS |
					        XCB_EVENT_MASK_BUTTON_RELEASE,
					    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_SYNC,
					    XCB_WINDOW_NONE, XCB_CURSOR_NONE,
					    (uint8_t) buttons[i].button,
					    (uint16_t) (buttons[i].mask | modifiers[j]));
	}
}

void
hide(Client *c)
{
	if (!c || c->ishidden)
		return;

	Window            w  = c->win;
	xcb_connection_t *xc = XGetXCBConnection(dpy);

	xcb_grab_server(xc);
	{
		xcb_get_window_attributes_cookie_t rck =
		    xcb_get_window_attributes(xc, root);
		xcb_get_window_attributes_cookie_t cck =
		    xcb_get_window_attributes(xc, w);
		xcb_get_window_attributes_reply_t *rr =
		    xcb_get_window_attributes_reply(xc, rck, NULL);
		xcb_get_window_attributes_reply_t *cr =
		    xcb_get_window_attributes_reply(xc, cck, NULL);

		uint32_t root_em = rr ? rr->your_event_mask : 0;
		uint32_t win_em  = cr ? cr->your_event_mask : 0;
		free(rr);
		free(cr);

		uint32_t mask;
		mask = root_em & ~(uint32_t) SubstructureNotifyMask;
		xcb_change_window_attributes(xc, root, XCB_CW_EVENT_MASK, &mask);
		mask = win_em & ~(uint32_t) StructureNotifyMask;
		xcb_change_window_attributes(xc, w, XCB_CW_EVENT_MASK, &mask);
		xcb_unmap_window(xc, w);
		setclientstate(c, IconicState);
		mask = root_em;
		xcb_change_window_attributes(xc, root, XCB_CW_EVENT_MASK, &mask);
		mask = win_em;
		xcb_change_window_attributes(xc, w, XCB_CW_EVENT_MASK, &mask);
	}
	xcb_ungrab_server(xc);

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

	xcb_map_window(XGetXCBConnection(dpy), c->win);
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
		xcb_connection_t *xc = XGetXCBConnection(dpy);
		xcb_grab_server(xc);
		xcb_set_close_down_mode(xc, XCB_CLOSE_DOWN_DESTROY_ALL);
		xcb_kill_client(xc, selmon->sel->win);
		xcb_ungrab_server(xc);
		xflush(dpy);
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
	if (xcb_icccm_get_wm_transient_for_reply(XGetXCBConnection(dpy),
	        xcb_icccm_get_wm_transient_for(XGetXCBConnection(dpy), w),
	        (xcb_window_t *) (void *) &trans, NULL) &&
	    (t = wintoclient(trans))) {
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
	{
		xcb_connection_t *xcb = XGetXCBConnection(dpy);
		uint32_t          bw  = (uint32_t) c->bw;
		xcb_configure_window(xcb, w, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
		uint32_t pix = scheme[SchemeNorm][ColBorder].pixel;
		xcb_change_window_attributes(xcb, w, XCB_CW_BORDER_PIXEL, &pix);
	}
	configure(c);
	updatewindowtype(c);
	updatesizehints(c);
	updatewmhints(c);
	if (c->iscentered) {
		c->x = c->mon->mx + (c->mon->mw - WIDTH(c)) / 2;
		c->y = c->mon->my + (c->mon->mh - HEIGHT(c)) / 2;
	}
	{
		uint32_t mask = EnterWindowMask | FocusChangeMask |
		    PropertyChangeMask | StructureNotifyMask;
		xcb_change_window_attributes(
		    XGetXCBConnection(dpy), w, XCB_CW_EVENT_MASK, &mask);
	}
	grabbuttons(c, 0);
	if (!c->isfloating)
		c->isfloating = c->oldstate = t != NULL || c->isfixed;
	if (c->isfloating) {
		uint32_t stack = XCB_STACK_MODE_ABOVE;
		xcb_configure_window(XGetXCBConnection(dpy), c->win,
		    XCB_CONFIG_WINDOW_STACK_MODE, &stack);
	}
	attach(c);
	attachstack(c);
	{
		xcb_connection_t *xcb    = XGetXCBConnection(dpy);
		xcb_atom_t        winxid = (xcb_atom_t) c->win;
		xcb_change_property(xcb, XCB_PROP_MODE_APPEND, root,
		    netatom[NetClientList], XCB_ATOM_WINDOW, 32, 1, &winxid);
	}

	setewmhdesktop(c);
	setwmstate(c);

	{
		uint32_t extents[4] = { (uint32_t) c->bw, (uint32_t) c->bw,
			(uint32_t) c->bw, (uint32_t) c->bw };
		xcb_change_property(XGetXCBConnection(dpy), XCB_PROP_MODE_REPLACE,
		    c->win, netatom[NetFrameExtents], XCB_ATOM_CARDINAL, 32, 4,
		    extents);
	}

	{
		uint32_t vals[4] = { (uint32_t) (c->x + 2 * sw), (uint32_t) c->y,
			(uint32_t) c->w, (uint32_t) c->h };
		xcb_configure_window(XGetXCBConnection(dpy), c->win,
		    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
		    vals);
	}
	setclientstate(c, NormalState);
	if (c->mon == selmon)
		unfocus(selmon->sel, 0);
	/* Don't make a hidden scratchpad the selected client */
	if (!c->scratchkey)
		c->mon->sel = c;
	arrange(c->mon);
	xcb_map_window(XGetXCBConnection(dpy), c->win);
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
	{
		xcb_connection_t         *xc = XGetXCBConnection(dpy);
		xcb_grab_pointer_cookie_t gck =
		    xcb_grab_pointer(xc, 0, root, MOUSEMASK, XCB_GRAB_MODE_ASYNC,
		        XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE,
		        (xcb_cursor_t) cursor[CurMove]->cursor, XCB_CURRENT_TIME);
		xcb_grab_pointer_reply_t *gr = xcb_grab_pointer_reply(xc, gck, NULL);
		if (!gr || gr->status != XCB_GRAB_STATUS_SUCCESS) {
			free(gr);
			return;
		}
		free(gr);
	}
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
	xcb_ungrab_pointer(XGetXCBConnection(dpy), XCB_CURRENT_TIME);
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
	{
		uint32_t vals[5] = { (uint32_t) wc.x, (uint32_t) wc.y,
			(uint32_t) wc.width, (uint32_t) wc.height,
			(uint32_t) wc.border_width };
		xcb_configure_window(XGetXCBConnection(dpy), c->win,
		    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
		        XCB_CONFIG_WINDOW_BORDER_WIDTH,
		    vals);
	}
	configure(c);
	xflush(dpy);
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
	{
		xcb_connection_t         *xc = XGetXCBConnection(dpy);
		xcb_grab_pointer_cookie_t gck =
		    xcb_grab_pointer(xc, 0, root, MOUSEMASK, XCB_GRAB_MODE_ASYNC,
		        XCB_GRAB_MODE_ASYNC, XCB_WINDOW_NONE,
		        (xcb_cursor_t) cursor[CurResize]->cursor, XCB_CURRENT_TIME);
		xcb_grab_pointer_reply_t *gr = xcb_grab_pointer_reply(xc, gck, NULL);
		if (!gr || gr->status != XCB_GRAB_STATUS_SUCCESS) {
			free(gr);
			return;
		}
		free(gr);
	}
	xcb_warp_pointer(XGetXCBConnection(dpy), XCB_WINDOW_NONE, c->win, 0, 0, 0,
	    0, (int16_t) (c->w + c->bw - 1), (int16_t) (c->h + c->bw - 1));
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
	xcb_warp_pointer(XGetXCBConnection(dpy), XCB_WINDOW_NONE, c->win, 0, 0, 0,
	    0, (int16_t) (c->w + c->bw - 1), (int16_t) (c->h + c->bw - 1));
	xcb_ungrab_pointer(XGetXCBConnection(dpy), XCB_CURRENT_TIME);
	/* Discard any stale EnterNotify events that accumulated during the
	 * grab so we don't spuriously change focus after a resize. */
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
	uint32_t data[2] = { (uint32_t) state, XCB_ATOM_NONE };

	xcb_change_property(XGetXCBConnection(dpy), XCB_PROP_MODE_REPLACE, c->win,
	    wmatom[WMState], wmatom[WMState], 32, 2, data);
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
		{
			uint32_t stack = XCB_STACK_MODE_ABOVE;
			xcb_configure_window(XGetXCBConnection(dpy), c->win,
			    XCB_CONFIG_WINDOW_STACK_MODE, &stack);
		}
#ifdef COMPOSITOR
		compositor_check_unredirect();
#endif
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
		compositor_check_unredirect();
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
	c->isurgent = urg;
	{
		xcb_connection_t         *xc = XGetXCBConnection(dpy);
		xcb_get_property_cookie_t ck = xcb_icccm_get_wm_hints(xc, c->win);
		xcb_icccm_wm_hints_t      wmh;
		if (xcb_icccm_get_wm_hints_reply(xc, ck, &wmh, NULL)) {
			if (urg)
				wmh.flags |= XCB_ICCCM_WM_HINT_X_URGENCY;
			else
				wmh.flags &= ~XCB_ICCCM_WM_HINT_X_URGENCY;
			xcb_icccm_set_wm_hints(xc, c->win, &wmh);
		}
	}
	setwmstate(c);
}

void
showhide(Client *c)
{
	if (!c)
		return;
	if (ISVISIBLE(c, c->mon) && !c->ishidden) {
		compositor_set_hidden(c, 0);
		{
			uint32_t vals[2] = { (uint32_t) c->x, (uint32_t) c->y };
			xcb_configure_window(XGetXCBConnection(dpy), c->win,
			    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
		}
		if ((!c->mon->lt[c->mon->sellt]->arrange || c->isfloating) &&
		    !c->isfullscreen)
			resize(c, c->x, c->y, c->w, c->h, 0);
		showhide(c->snext);
	} else {
		showhide(c->snext);
		compositor_set_hidden(c, 1);
		{
			uint32_t vals[2] = { (uint32_t) (WIDTH(c) * -2), (uint32_t) c->y };
			xcb_configure_window(XGetXCBConnection(dpy), c->win,
			    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
		}
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
	{
		uint32_t pix = scheme[SchemeNorm][ColBorder].pixel;
		xcb_change_window_attributes(
		    XGetXCBConnection(dpy), c->win, XCB_CW_BORDER_PIXEL, &pix);
	}
#ifdef COMPOSITOR
	compositor_focus_window(c);
#endif
	if (setfocus) {
		xcb_set_input_focus(XGetXCBConnection(dpy),
		    XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);
		xcb_delete_property(
		    XGetXCBConnection(dpy), root, netatom[NetActiveWindow]);
	}
}

void
unmanage(Client *c, int destroyed)
{
	Monitor *m = c->mon;

	detach(c);
	detachstack(c);
	if (!destroyed) {
		xcb_connection_t *xc = XGetXCBConnection(dpy);
		xcb_grab_server(xc);
		{
			uint32_t no_events = XCB_EVENT_MASK_NO_EVENT;
			xcb_change_window_attributes(
			    xc, c->win, XCB_CW_EVENT_MASK, &no_events);
		}
		{
			uint32_t bw = (uint32_t) c->oldbw;
			xcb_configure_window(
			    xc, c->win, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
		}
		xcb_ungrab_button(xc, XCB_BUTTON_INDEX_ANY, c->win, XCB_MOD_MASK_ANY);
		setclientstate(c, WithdrawnState);
		xcb_ungrab_server(xc);
		xflush(dpy);
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
	xcb_connection_t         *xc = XGetXCBConnection(dpy);
	xcb_get_property_cookie_t ck = xcb_icccm_get_wm_normal_hints(xc, c->win);
	xcb_size_hints_t          size;

	if (!xcb_icccm_get_wm_normal_hints_reply(xc, ck, &size, NULL))
		size.flags = XCB_ICCCM_SIZE_HINT_P_SIZE;
	if (size.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) {
		c->basew = size.base_width;
		c->baseh = size.base_height;
	} else if (size.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
		c->basew = size.min_width;
		c->baseh = size.min_height;
	} else
		c->basew = c->baseh = 0;
	if (size.flags & XCB_ICCCM_SIZE_HINT_P_RESIZE_INC) {
		c->incw = size.width_inc;
		c->inch = size.height_inc;
	} else
		c->incw = c->inch = 0;
	if (size.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
		c->maxw = size.max_width;
		c->maxh = size.max_height;
	} else
		c->maxw = c->maxh = 0;
	if (size.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
		c->minw = size.min_width;
		c->minh = size.min_height;
	} else if (size.flags & XCB_ICCCM_SIZE_HINT_BASE_SIZE) {
		c->minw = size.base_width;
		c->minh = size.base_height;
	} else
		c->minw = c->minh = 0;
	if (size.flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) {
		c->mina = (float) size.min_aspect_den / size.min_aspect_num;
		c->maxa = (float) size.max_aspect_num / size.max_aspect_den;
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
	xcb_connection_t         *xc = XGetXCBConnection(dpy);
	xcb_get_property_cookie_t ck = xcb_icccm_get_wm_hints(xc, c->win);
	xcb_icccm_wm_hints_t      wmh;

	if (xcb_icccm_get_wm_hints_reply(xc, ck, &wmh, NULL)) {
		if (c == selmon->sel && (wmh.flags & XCB_ICCCM_WM_HINT_X_URGENCY)) {
			wmh.flags &= ~XCB_ICCCM_WM_HINT_X_URGENCY;
			xcb_icccm_set_wm_hints(xc, c->win, &wmh);
		} else
			c->isurgent = (wmh.flags & XCB_ICCCM_WM_HINT_X_URGENCY) ? 1 : 0;
		if (wmh.flags & XCB_ICCCM_WM_HINT_INPUT)
			c->neverfocus = !wmh.input;
		else
			c->neverfocus = 0;
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
		xcb_warp_pointer(XGetXCBConnection(dpy), XCB_WINDOW_NONE, root, 0, 0,
		    0, 0, (int16_t) (selmon->wx + selmon->ww / 2),
		    (int16_t) (selmon->wy + selmon->wh / 2));
		return;
	}

	if (!getrootptr(&x, &y) ||
	    (x > c->x - c->bw && y > c->y - c->bw && x < c->x + c->w + c->bw * 2 &&
	        y < c->y + c->h + c->bw * 2) ||
	    (y > c->mon->by && y < c->mon->by + bh) || (c->mon->topbar && !y))
		return;

	xcb_warp_pointer(XGetXCBConnection(dpy), XCB_WINDOW_NONE, c->win, 0, 0, 0,
	    0, (int16_t) (c->w / 2), (int16_t) (c->h / 2));
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
