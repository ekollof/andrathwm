/* AndrathWM - client management
 * See LICENSE file for copyright and license details. */

#include <assert.h>
#include <stdint.h>

#include "client.h"
#include "awm.h"
#include "events.h"
#include "monitor.h"
#include "spawn.h"
#include "systray.h"
#include "wmstate.h"
#include "wm_properties.h"
#include "xrdb.h"
#include "config.h"

/* module-local strings */
static const char broken[] = "broken";

/* O(1) window-to-client lookup table; keyed by xcb_window_t cast to pointer */
static GHashTable *win_to_client;

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
	g_wm_backend->get_wm_class(
	    &g_plat, c->win, inst_buf, sizeof inst_buf, cls_buf, sizeof cls_buf);
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
			FOR_EACH_MON(m)
			if (m->tagset[m->seltags] & c->tags)
				break;
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
	Monitor *m;

	assert(c != NULL);
	assert(c->mon != NULL);
	m = c->mon;

	/* set minimum possible */
	*w = MAX(1, *w);
	*h = MAX(1, *h);
	if (interact) {
		if (*x > g_plat.sw)
			*x = g_plat.sw - WIDTH(c);
		if (*y > g_plat.sh)
			*y = g_plat.sh - HEIGHT(c);
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
	if (*h < g_plat.bh)
		*h = g_plat.bh;
	if (*w < g_plat.bh)
		*w = g_plat.bh;
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
	if (!win_to_client)
		win_to_client = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_hash_table_insert(win_to_client, GUINT_TO_POINTER(c->win), c);
	c->next            = g_awm.clients_head;
	g_awm.clients_head = c;
}

void
attachclients(Monitor *m)
{
	Monitor     *tm;
	Client      *c;
	unsigned int utags = 0;
	int          rmons = 0;
	if (!m)
		return;

	FOR_EACH_MON(tm)
	if (tm != m)
		utags |= tm->tagset[tm->seltags];

	for (c = g_awm.clients_head; c; c = c->next)
		if (ISVISIBLE(c, m)) {
			if (c->tags & utags) {
				c->tags = c->tags & m->tagset[m->seltags];
				rmons   = 1;
			}
			unfocus(c, 1);
			c->mon = m;
		}

	if (rmons)
		FOR_EACH_MON(tm)
	if (tm != m)
		arrange(tm);
}

void
attachstack(Client *c)
{
	c->snext         = g_awm.stack_head;
	g_awm.stack_head = c;
}

void
configure(Client *c)
{
	g_wm_backend->send_configure_notify(
	    &g_plat, c->win, c->x, c->y, c->bw, (int) c->w, (int) c->h);
}

void
detach(Client *c)
{
	Client **tc;

	if (win_to_client)
		g_hash_table_remove(win_to_client, GUINT_TO_POINTER(c->win));
	for (tc = &g_awm.clients_head; *tc && *tc != c; tc = &(*tc)->next)
		;
	*tc = c->next;
}

void
detachstack(Client *c)
{
	Client **tc, *t;

	assert(c != NULL);
	assert(c->mon != NULL);

	for (tc = &g_awm.stack_head; *tc && *tc != c; tc = &(*tc)->snext)
		;
	*tc = c->snext;

	if (c == c->mon->sel) {
		for (t = g_awm.stack_head; t && !ISVISIBLE(t, c->mon); t = t->snext)
			;
		c->mon->sel = t;
	}
}

void
focus(Client *c)
{
	if (c)
		assert(c->mon != NULL);
	if (!c || !ISVISIBLE(c, g_awm_selmon))
		for (c = g_awm.stack_head; c && !ISVISIBLE(c, g_awm_selmon);
		     c = c->snext)
			;
	if (g_awm_selmon->sel && g_awm_selmon->sel != c)
		unfocus(g_awm_selmon->sel, 0);
	if (c) {
		if (c->mon != g_awm_selmon) {
			g_awm_set_selmon(c->mon);
		}
		if (c->isurgent)
			seturgent(c, 0);
		detachstack(c);
		attachstack(c);
		grabbuttons(c, 1);
		{
			uint32_t pix = scheme[SchemeSel][ColBorder].pixel;
			g_wm_backend->change_attr(
			    &g_plat, c->win, XCB_CW_BORDER_PIXEL, &pix);
			if (!g_awm_selmon->pertag
			         .drawwithgaps[g_awm_selmon->pertag.curtag] &&
			    !c->isfloating) {
				uint32_t vals[2];
				vals[0] = (uint32_t) g_awm_selmon->barwin;
				vals[1] = XCB_STACK_MODE_BELOW;
				g_wm_backend->configure_win(&g_plat, c->win,
				    XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
				    vals);
			}
		}
		wmprop_set_focus(c);
	} else {
		g_wm_backend->set_input_focus(
		    &g_plat, g_awm_selmon->barwin, XCB_CURRENT_TIME);
		g_wm_backend->delete_prop(
		    &g_plat, g_plat.root, g_plat.netatom[NetActiveWindow]);
	}
	g_awm_selmon->sel = c;
	if (g_awm_selmon->lt[g_awm_selmon->sellt]->arrange == monocle)
		arrangemon(g_awm_selmon);
	barsdirty = 1;
#ifdef COMPOSITOR
	/* Dirty the border region of both the newly focused and previously
	 * focused client so the compositor repaints them in the correct colour. */
	compositor_focus_window(c);
	/* Re-evaluate fullscreen unredirect — the topmost window may have
	 * changed. */
	compositor_check_unredirect();
#endif
	wmstate_update();
}

void
focusstack(const Arg *arg)
{
	Client *c = NULL, *i;

	if (!g_awm_selmon->sel ||
	    (g_awm_selmon->sel->isfullscreen && lockfullscreen))
		return;
	if (arg->i > 0) {
		for (c = g_awm_selmon->sel->next; c && !ISVISIBLE(c, g_awm_selmon);
		     c = c->next)
			;
		if (!c)
			for (c = g_awm.clients_head; c && !ISVISIBLE(c, g_awm_selmon);
			     c = c->next)
				;
	} else {
		for (i = g_awm.clients_head; i != g_awm_selmon->sel; i = i->next) {
			if (ISVISIBLE(i, g_awm_selmon))
				c = i;
		}
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i, g_awm_selmon))
					c = i;
	}
	if (c) {
		focus(c);
		restack(g_awm_selmon);
	}
}

void
focusstackhidden(const Arg *arg)
{
	Client *c = NULL, *i;

	if (!g_awm_selmon->sel ||
	    (g_awm_selmon->sel->isfullscreen && lockfullscreen))
		return;

	if (arg->i > 0) {
		for (c = g_awm_selmon->sel->next;
		     c && !(c->tags & g_awm_selmon->tagset[g_awm_selmon->seltags]);
		     c = c->next)
			;
		if (!c)
			for (c = g_awm.clients_head;
			     c && !(c->tags & g_awm_selmon->tagset[g_awm_selmon->seltags]);
			     c = c->next)
				;
	} else {
		for (i = g_awm.clients_head; i != g_awm_selmon->sel; i = i->next) {
			if (i->tags & g_awm_selmon->tagset[g_awm_selmon->seltags])
				c = i;
		}
		if (!c)
			for (; i; i = i->next)
				if (i->tags & g_awm_selmon->tagset[g_awm_selmon->seltags])
					c = i;
	}

	if (c) {
		if (c->ishidden)
			show(c);
		else {
			focus(c);
			restack(g_awm_selmon);
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

	if (c == g_awm_selmon->sel) {
		hide(c);
		return;
	}

	if (ISVISIBLE(c, g_awm_selmon)) {
		if (g_awm_selmon->lt[g_awm_selmon->sellt]->arrange && !c->isfloating) {
			pop(c);
		} else {
			focus(c);
			restack(g_awm_selmon);
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

xcb_atom_t
getatomprop(Client *c, xcb_atom_t prop)
{
	xcb_atom_t req = (prop == g_plat.xatom[XembedInfo])
	    ? g_plat.xatom[XembedInfo]
	    : XCB_ATOM_ATOM;
	return g_wm_backend->get_atom_prop(&g_plat, c->win, prop, req);
}

int
getrootptr(int *x, int *y)
{
	return g_wm_backend->query_pointer(&g_plat, x, y);
}

long
getstate(xcb_window_t w)
{
	return (long) g_wm_backend->get_atom_prop(
	    &g_plat, w, g_plat.wmatom[WMState], g_plat.wmatom[WMState]);
}

int
gettextprop(xcb_window_t w, xcb_atom_t atom, char *text, unsigned int size)
{
	return g_wm_backend->get_text_prop(&g_plat, w, atom, text, size);
}

cairo_surface_t *
getwmicon(xcb_window_t w, int size)
{
	uint32_t        *data    = NULL;
	int              nitems  = 0;
	cairo_surface_t *surface = NULL;

	if (!g_wm_backend->get_wm_icon(&g_plat, w, &data, &nitems) || !data)
		return NULL;

	{

		if (nitems > 2) {
			unsigned long icon_w = data[0];
			unsigned long icon_h = data[1];

			if ((unsigned long) nitems >= 2 + icon_w * icon_h) {
				cairo_surface_t *src;
				cairo_t         *cr;
				unsigned char   *argb_data;
				unsigned long    i;
				int              stride;

				awm_debug("extracting %lux%lu icon, nitems=%d", icon_w, icon_h,
				    nitems);

				stride =
				    cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, icon_w);
				argb_data = calloc(icon_h, stride);
				if (!argb_data) {
					free(data);
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

	free(data);
	return surface;
}

void
grabbuttons(Client *c, int focused)
{
	g_wm_backend->grab_buttons(&g_plat, c->win, focused);
}

void
hide(Client *c)
{
	if (!c || c->ishidden)
		return;

	xcb_window_t w = c->win;

	g_wm_backend->grab_server(&g_plat);
	{
		uint32_t root_em = 0, win_em = 0;
		g_wm_backend->get_event_mask(&g_plat, g_plat.root, &root_em);
		g_wm_backend->get_event_mask(&g_plat, w, &win_em);
		uint32_t mask;
		mask = root_em & ~(uint32_t) XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
		g_wm_backend->change_attr(
		    &g_plat, g_plat.root, XCB_CW_EVENT_MASK, &mask);
		mask = win_em & ~(uint32_t) XCB_EVENT_MASK_STRUCTURE_NOTIFY;
		g_wm_backend->change_attr(&g_plat, w, XCB_CW_EVENT_MASK, &mask);
		g_wm_backend->unmap(&g_plat, w);
		setclientstate(c, AWM_WM_STATE_ICONIC);
		mask = root_em;
		g_wm_backend->change_attr(
		    &g_plat, g_plat.root, XCB_CW_EVENT_MASK, &mask);
		mask = win_em;
		g_wm_backend->change_attr(&g_plat, w, XCB_CW_EVENT_MASK, &mask);
	}
	g_wm_backend->ungrab_server(&g_plat);

	c->ishidden = 1;
	focus(NULL);
	arrange(c->mon);
	barsdirty = 1;
	wmstate_update();
}

void
hidewin(const Arg *arg)
{
	Client *c = (Client *) arg->v;
	if (!c)
		c = g_awm_selmon->sel;
	if (!c)
		return;
	hide(c);
}

void
show(Client *c)
{
	if (!c || !c->ishidden)
		return;

	g_wm_backend->map(&g_plat, c->win);
	setclientstate(c, AWM_WM_STATE_NORMAL);
	c->ishidden = 0;
	focus(c);
	arrange(c->mon);
	barsdirty = 1;
	wmstate_update();
}

void
restorewin(const Arg *arg)
{
	Client *c = (Client *) arg->v;
	if (!c)
		for (c = g_awm.stack_head; c && !c->ishidden; c = c->snext)
			;
	if (!c)
		return;
	show(c);
}

void
showall(const Arg *arg)
{
	Client *c;

	for (c = g_awm.clients_head; c; c = c->next)
		if (c->ishidden &&
		    (c->tags & g_awm_selmon->tagset[g_awm_selmon->seltags]))
			show(c);
}

void
incnmaster(const Arg *arg)
{
	g_awm_selmon->nmaster =
	    g_awm_selmon->pertag.nmasters[g_awm_selmon->pertag.curtag] =
	        MAX(g_awm_selmon->nmaster + arg->i, 0);
	arrange(g_awm_selmon);
}

void
killclient(const Arg *arg)
{
	if (!g_awm_selmon->sel)
		return;

	if (!wmprop_send_event(g_awm_selmon->sel->win, g_plat.wmatom[WMDelete], 0,
	        g_plat.wmatom[WMDelete], XCB_CURRENT_TIME, 0, 0, 0)) {

		g_wm_backend->kill_client_hard(&g_plat, g_awm_selmon->sel->win);
		g_wm_backend->flush(&g_plat);
	}
}

void
manage(xcb_window_t w, int x, int y, int ww, int wh, int bw)
{
	Client      *c, *t = NULL;
	xcb_window_t trans = XCB_WINDOW_NONE;

	c      = ecalloc(1, sizeof(Client));
	c->win = w;
	c->x = c->oldx = x;
	c->y = c->oldy = y;
	c->w = c->oldw = ww;
	c->h = c->oldh       = wh;
	c->oldbw             = bw;
	c->opacity           = 1.0;
	c->bypass_compositor = 0;

	updatetitle(c);
	c->icon = getwmicon(w, (int) g_plat.ui_iconsize);
	if ((trans = g_wm_backend->get_wm_transient_for(&g_plat, w)) !=
	        XCB_WINDOW_NONE &&
	    (t = wintoclient(trans))) {
		c->mon  = t->mon;
		c->tags = t->tags;
	} else {
		c->mon = g_awm_selmon;
		applyrules(c);
	}
	assert(c->mon != NULL);
#ifdef COMPOSITOR
	/* If the window already has _NET_WM_WINDOW_OPACITY set (common for apps
	 * that manage their own translucency), let it override the rule value so
	 * the window always wins over the rule default. */
	{
		unsigned long raw =
		    (unsigned long) getatomprop(c, g_plat.netatom[NetWMWindowOpacity]);
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
	c->bw = (int) g_plat.ui_borderpx;

	{
		uint32_t bw = (uint32_t) c->bw;
		g_wm_backend->configure_win(
		    &g_plat, w, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
		uint32_t pix = scheme[SchemeNorm][ColBorder].pixel;
		g_wm_backend->change_attr(&g_plat, w, XCB_CW_BORDER_PIXEL, &pix);
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
		uint32_t mask = XCB_EVENT_MASK_ENTER_WINDOW |
		    XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE |
		    XCB_EVENT_MASK_STRUCTURE_NOTIFY;
		g_wm_backend->change_attr(&g_plat, w, XCB_CW_EVENT_MASK, &mask);
	}
	grabbuttons(c, 0);
	if (!c->isfloating)
		c->isfloating = c->oldstate = t != NULL || c->isfixed;
	if (c->isfloating) {
		uint32_t stack = XCB_STACK_MODE_ABOVE;
		g_wm_backend->configure_win(
		    &g_plat, c->win, XCB_CONFIG_WINDOW_STACK_MODE, &stack);
	}
	attach(c);
	attachstack(c);
	{
		uint32_t winxid = (uint32_t) c->win;
		g_wm_backend->change_prop(&g_plat, g_plat.root,
		    g_plat.netatom[NetClientList], XCB_ATOM_WINDOW, 32,
		    XCB_PROP_MODE_APPEND, 1, &winxid);
	}

	wmprop_set_client_desktop(c);
	wmprop_set_wm_state(c);

	{
		uint32_t extents[4] = { (uint32_t) c->bw, (uint32_t) c->bw,
			(uint32_t) c->bw, (uint32_t) c->bw };
		g_wm_backend->change_prop(&g_plat, c->win,
		    g_plat.netatom[NetFrameExtents], XCB_ATOM_CARDINAL, 32,
		    XCB_PROP_MODE_REPLACE, 4, extents);
	}

	{
		uint32_t vals[4] = { (uint32_t) (c->x + 2 * g_plat.sw),
			(uint32_t) c->y, (uint32_t) c->w, (uint32_t) c->h };
		g_wm_backend->configure_win(&g_plat, c->win,
		    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
		    vals);
	}
	setclientstate(c, AWM_WM_STATE_NORMAL);
	if (c->mon == g_awm_selmon)
		unfocus(g_awm_selmon->sel, 0);
	/* Don't make a hidden scratchpad the selected client */
	if (!c->scratchkey)
		c->mon->sel = c;
	arrange(c->mon);
	g_wm_backend->map(&g_plat, c->win);
#ifdef COMPOSITOR
	compositor_add_window(c);
	/* Force-sync the CompWin geometry to the client struct.  During a
	 * restart, arrange() runs before the CompWin exists (comp_add_by_xid
	 * skips unmapped windows), so compositor_configure_window() was a
	 * no-op.  If comp_add_by_xid later captured stale X server geometry,
	 * this call corrects it. */
	compositor_configure_window(c, c->bw);
	c->bypass_compositor =
	    (int) getatomprop(c, g_plat.netatom[NetWMBypassCompositor]);
	if (c->bypass_compositor == 1)
		compositor_bypass_window(c, 1);
#endif
	focus(NULL);
	wmstate_update();
}

void
movemouse(const Arg *arg)
{
	int         x, y, ocx, ocy, nx, ny;
	Client     *c;
	Monitor    *m;
	AwmEvent   *xe;
	WmTimestamp lasttime = 0;

	if (!(c = g_awm_selmon->sel))
		return;
	if (c->isfullscreen)
		return;
	restack(g_awm_selmon);
	ocx = c->x;
	ocy = c->y;
	{
		if (!g_wm_backend->grab_pointer(
		        &g_plat, (xcb_cursor_t) cursor[CurMove]->cursor))
			return;
	}
	if (!getrootptr(&x, &y)) {
		g_wm_backend->ungrab_pointer(&g_plat);
		return;
	}
	g_wm_backend->flush(&g_plat);
	for (;;) {
		while (!(xe = g_wm_backend->next_event(&g_plat)))
			;
		{
			if (xe->type == AWM_EV_BUTTON_RELEASE) {
				free(xe);
				break;
			}
			switch (xe->type) {
			case AWM_EV_CONFIGURE_REQUEST:
			case AWM_EV_EXPOSE:
			case AWM_EV_MAP_REQUEST:
				if (handler[xe->type])
					handler[xe->type](xe);
				break;
			case AWM_EV_MOTION: {
				if ((xe->motion.time - lasttime) <= (1000 / motionfps)) {
					free(xe);
					continue;
				}
				lasttime = xe->motion.time;

				nx = ocx + (xe->motion.root_x - x);
				ny = ocy + (xe->motion.root_y - y);
				if (abs(g_awm_selmon->wx - nx) < (int) g_plat.ui_snap)
					nx = g_awm_selmon->wx;
				else if (abs((g_awm_selmon->wx + g_awm_selmon->ww) -
				             (nx + WIDTH(c))) < (int) g_plat.ui_snap)
					nx = g_awm_selmon->wx + g_awm_selmon->ww - WIDTH(c);
				if (abs(g_awm_selmon->wy - ny) < (int) g_plat.ui_snap)
					ny = g_awm_selmon->wy;
				else if (abs((g_awm_selmon->wy + g_awm_selmon->wh) -
				             (ny + HEIGHT(c))) < (int) g_plat.ui_snap)
					ny = g_awm_selmon->wy + g_awm_selmon->wh - HEIGHT(c);
				if (!c->isfloating &&
				    g_awm_selmon->lt[g_awm_selmon->sellt]->arrange &&
				    (abs(nx - c->x) > (int) g_plat.ui_snap ||
				        abs(ny - c->y) > (int) g_plat.ui_snap))
					togglefloating(NULL);
				if (!g_awm_selmon->lt[g_awm_selmon->sellt]->arrange ||
				    c->isfloating)
					resize(c, nx, ny, c->w, c->h, 1);
#ifdef COMPOSITOR
				compositor_repaint_now();
#endif
				break;
			}
			default:
#ifdef COMPOSITOR
				/* Flush any pending compositor repaint synchronously —
				 * g_idle_add never fires inside the grab loop. */
				compositor_repaint_now();
#endif
				break;
			}
		}
		free(xe);
	}
	g_wm_backend->ungrab_pointer(&g_plat);
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != g_awm_selmon) {
		sendmon(c, m);
		g_awm_set_selmon(m);
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
	int bw;

	assert(c != NULL);
	assert(c->mon != NULL);
	assert(w > 0);
	assert(h > 0);
	c->oldx = c->x;
	c->x    = x;
	c->oldy = c->y;
	c->y    = y;
	c->oldw = c->w;
	c->w    = w;
	c->oldh = c->h;
	c->h    = h;
	bw      = c->bw;
	if (!c->mon->pertag.drawwithgaps[c->mon->pertag.curtag] &&
	    (((nexttiled(g_awm.clients_head, c->mon) == c &&
	          !nexttiled(c->next, c->mon)) ||
	        &monocle == c->mon->lt[c->mon->sellt]->arrange)) &&
	    !c->isfullscreen && !c->isfloating &&
	    NULL != c->mon->lt[c->mon->sellt]->arrange) {
		c->w = w += c->bw * 2;
		c->h = h += c->bw * 2;
		bw   = 0;
	}
	{
		uint32_t vals[5] = { (uint32_t) x, (uint32_t) y, (uint32_t) w,
			(uint32_t) h, (uint32_t) bw };
		g_wm_backend->configure_win(&g_plat, c->win,
		    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
		        XCB_CONFIG_WINDOW_BORDER_WIDTH,
		    vals);
	}
	configure(c);
	g_wm_backend->flush(&g_plat);
#ifdef COMPOSITOR
	compositor_configure_window(c, bw);
#endif
}

void
resizemouse(const Arg *arg)
{
	int         ocx, ocy, nw, nh;
	Client     *c;
	Monitor    *m;
	AwmEvent   *xe;
	WmTimestamp lasttime = 0;

	if (!(c = g_awm_selmon->sel))
		return;
	if (c->isfullscreen)
		return;
	restack(g_awm_selmon);
	ocx = c->x;
	ocy = c->y;
	{
		if (!g_wm_backend->grab_pointer(
		        &g_plat, (xcb_cursor_t) cursor[CurResize]->cursor))
			return;
	}
	g_wm_backend->warp_pointer(&g_plat, c->win, (int16_t) (c->w + c->bw - 1),
	    (int16_t) (c->h + c->bw - 1));
	g_wm_backend->flush(&g_plat);
	for (;;) {
		while (!(xe = g_wm_backend->next_event(&g_plat)))
			;
		{
			if (xe->type == AWM_EV_BUTTON_RELEASE) {
				free(xe);
				break;
			}
			switch (xe->type) {
			case AWM_EV_CONFIGURE_REQUEST:
			case AWM_EV_EXPOSE:
			case AWM_EV_MAP_REQUEST:
				if (handler[xe->type])
					handler[xe->type](xe);
				break;
			case AWM_EV_MOTION: {
				if ((xe->motion.time - lasttime) <= (1000 / motionfps)) {
					free(xe);
					continue;
				}
				lasttime = xe->motion.time;

				nw = MAX(xe->motion.root_x - ocx - 2 * c->bw + 1, 1);
				nh = MAX(xe->motion.root_y - ocy - 2 * c->bw + 1, 1);
				if (c->mon->wx + nw >= g_awm_selmon->wx &&
				    c->mon->wx + nw <= g_awm_selmon->wx + g_awm_selmon->ww &&
				    c->mon->wy + nh >= g_awm_selmon->wy &&
				    c->mon->wy + nh <= g_awm_selmon->wy + g_awm_selmon->wh) {
					if (!c->isfloating &&
					    g_awm_selmon->lt[g_awm_selmon->sellt]->arrange &&
					    (abs(nw - c->w) > (int) g_plat.ui_snap ||
					        abs(nh - c->h) > (int) g_plat.ui_snap))
						togglefloating(NULL);
				}
				if (!g_awm_selmon->lt[g_awm_selmon->sellt]->arrange ||
				    c->isfloating)
					resize(c, c->x, c->y, nw, nh, 1);
#ifdef COMPOSITOR
				compositor_repaint_now();
#endif
				break;
			}
			default:
#ifdef COMPOSITOR
				/* Flush any pending compositor repaint synchronously —
				 * g_idle_add never fires inside the grab loop. */
				compositor_repaint_now();
#endif
				break;
			}
		}
		free(xe);
	}
	g_wm_backend->warp_pointer(&g_plat, c->win, (int16_t) (c->w + c->bw - 1),
	    (int16_t) (c->h + c->bw - 1));
	g_wm_backend->ungrab_pointer(&g_plat);
	/* Discard stale EnterNotify events accumulated during the grab so
	 * we don't spuriously change focus after a resize.  Non-EnterNotify
	 * events are dispatched through the normal handler. */
	g_wm_backend->flush(&g_plat);
	while ((xe = g_wm_backend->poll_event(&g_plat))) {
		if (xe->type != AWM_EV_ENTER && xe->type != AWM_EV_NONE &&
		    handler[xe->type])
			handler[xe->type](xe);
		free(xe);
	}
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != g_awm_selmon) {
		sendmon(c, m);
		g_awm_set_selmon(m);
		focus(NULL);
	}
}

void
sendmon(Client *c, Monitor *m)
{
	assert(c != NULL);
	assert(m != NULL);
	if (c->mon == m)
		return;
	unfocus(c, 1);
	detach(c);
	detachstack(c);
	c->mon  = m;
	c->tags = m->tagset[m->seltags];
	attach(c);
	attachstack(c);
	focus(NULL);
	arrange(NULL);
	wmstate_update();
}

void
setclientstate(Client *c, long state)
{
	uint32_t data[2] = { (uint32_t) state, XCB_ATOM_NONE };

	g_wm_backend->change_prop(&g_plat, c->win, g_plat.wmatom[WMState],
	    g_plat.wmatom[WMState], 32, XCB_PROP_MODE_REPLACE, 2, data);
}

void
setfullscreen(Client *c, int fullscreen)
{
	assert(c != NULL);
	assert(c->mon != NULL);
	if (fullscreen && !c->isfullscreen) {
		c->isfullscreen = 1;
		c->oldstate     = c->isfloating;
		c->oldbw        = c->bw;
		c->bw           = 0;
		c->isfloating   = 1;
		wmprop_set_wm_state(c);
		resizeclient(c, c->mon->mx, c->mon->my, c->mon->mw, c->mon->mh);
#ifdef COMPOSITOR
		/* Defer the bypass+unredirect+overlay-lower sequence by ~40 ms so
		 * the client has time to process the ConfigureNotify from
		 * resizeclient() and fully repaint while still redirected.  The
		 * compositor paints one clean fullscreen frame first; the deferred
		 * callback then calls compositor_bypass_window, raises the window
		 * above the bar, compositor_check_unredirect, and xcb_clear_area. */
		compositor_defer_fullscreen_bypass(c);
#endif
	} else if (!fullscreen && c->isfullscreen) {
		c->isfullscreen = 0;
		c->isfloating   = c->oldstate;
		c->bw           = c->oldbw;
		c->x            = c->oldx;
		c->y            = c->oldy;
		c->w            = c->oldw;
		c->h            = c->oldh;
		wmprop_set_wm_state(c);
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
	wmstate_update();
}

void
setgaps(const Arg *arg)
{
	switch (arg->i) {
	case GAP_TOGGLE:
		g_awm_selmon->pertag.drawwithgaps[g_awm_selmon->pertag.curtag] =
		    !g_awm_selmon->pertag.drawwithgaps[g_awm_selmon->pertag.curtag];
		break;
	case GAP_RESET:
		if (g_awm_selmon->pertag.curtag > 0)
			g_awm_selmon->pertag.gappx[g_awm_selmon->pertag.curtag] =
			    g_plat.ui_gappx;
		else
			g_awm_selmon->pertag.gappx[0] = g_plat.ui_gappx;
		break;
	default:
		if (g_awm_selmon->pertag.gappx[g_awm_selmon->pertag.curtag] + arg->i <
		    0)
			g_awm_selmon->pertag.gappx[g_awm_selmon->pertag.curtag] = 0;
		else
			g_awm_selmon->pertag.gappx[g_awm_selmon->pertag.curtag] += arg->i;
	}
	arrange(g_awm_selmon);
}

void
setlayout(const Arg *arg)
{
	if (!arg || !arg->v || arg->v != g_awm_selmon->lt[g_awm_selmon->sellt])
		g_awm_selmon->sellt =
		    g_awm_selmon->pertag.sellts[g_awm_selmon->pertag.curtag] ^= 1;
	if (arg && arg->v)
		g_awm_selmon->lt[g_awm_selmon->sellt] =
		    g_awm_selmon->pertag.ltidxs[(g_awm_selmon->pertag.curtag) * 2 +
		        (g_awm_selmon->sellt)] = (Layout *) arg->v;
	strncpy(g_awm_selmon->ltsymbol,
	    g_awm_selmon->lt[g_awm_selmon->sellt]->symbol,
	    sizeof g_awm_selmon->ltsymbol);
	if (g_awm_selmon->sel)
		arrange(g_awm_selmon);
	else
		drawbar(g_awm_selmon);
	wmstate_update();
}

void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !g_awm_selmon->lt[g_awm_selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0 ? arg->f + g_awm_selmon->mfact : arg->f - 1.0;
	if (f < 0.05 || f > 0.95)
		return;
	g_awm_selmon->mfact =
	    g_awm_selmon->pertag.mfacts[g_awm_selmon->pertag.curtag] = f;
	arrange(g_awm_selmon);
}

void
seturgent(Client *c, int urg)
{
	c->isurgent = urg;
	{
		AwmWmHints wmh;
		if (g_wm_backend->get_wm_hints(&g_plat, c->win, &wmh)) {
			if (urg)
				wmh.flags |= AWM_WM_HINT_URGENCY;
			else
				wmh.flags &= ~AWM_WM_HINT_URGENCY;
			g_wm_backend->set_wm_hints(&g_plat, c->win, &wmh);
		}
	}
	wmprop_set_wm_state(c);
}

void
showhide(Client *c)
{
	Client *hidden[1024];
	int     nhidden = 0;
	int     i;

	/* First pass (forward): show visible clients and collect hidden ones.
	 * The original recursive implementation processed hidden clients in
	 * reverse stack order (recurse-then-act in the else branch), so we
	 * gather them here and replay in reverse order in the second pass. */
	for (; c; c = c->snext) {
		if (ISVISIBLE(c, c->mon) && !c->ishidden) {
			/* For monocle layout, skip the compositor unhide here —
			 * monocle() runs immediately after showhide() (inside
			 * arrangemon) and is the sole authority on which windows
			 * are hidden on that monitor.  Unhiding every visible
			 * window here would only trigger a wasted show→hide
			 * repaint cycle for the non-top windows. */
			if (c->mon->lt[c->mon->sellt]->arrange != monocle) {
#ifdef COMPOSITOR
				compositor_set_hidden(c, 0);
#endif
			}
			{
				uint32_t vals[2] = { (uint32_t) c->x, (uint32_t) c->y };
				g_wm_backend->configure_win(&g_plat, c->win,
				    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
			}
			if ((!c->mon->lt[c->mon->sellt]->arrange || c->isfloating) &&
			    !c->isfullscreen)
				resize(c, c->x, c->y, c->w, c->h, 0);
		} else {
			if (nhidden < (int) (sizeof hidden / sizeof hidden[0]))
				hidden[nhidden++] = c;
		}
	}

	/* Second pass (reverse): hide clients in the same bottom-up order as
	 * the original recursive implementation. */
	for (i = nhidden - 1; i >= 0; i--) {
		c = hidden[i];

#ifdef COMPOSITOR
		compositor_set_hidden(c, 1);
#endif
		{
			uint32_t vals[2] = { (uint32_t) (int32_t) (WIDTH(c) * -2),
				(uint32_t) c->y };
			g_wm_backend->configure_win(&g_plat, c->win,
			    XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, vals);
		}
	}
}

void
tag(const Arg *arg)
{
	Monitor     *m;
	unsigned int newtags;
	if (g_awm_selmon->sel && arg->ui & TAGMASK) {
		newtags = arg->ui & TAGMASK;
		FOR_EACH_MON(m)
		if (m != g_awm_selmon && m->tagset[m->seltags] & newtags) {
			if (newtags & g_awm_selmon->tagset[g_awm_selmon->seltags])
				return;
			g_awm_selmon->sel->tags = newtags;
			g_awm_selmon->sel->mon  = m;
			wmprop_set_client_desktop(g_awm_selmon->sel);
			arrange(m);
			break;
		}
		g_awm_selmon->sel->tags = arg->ui & TAGMASK;
		wmprop_set_client_desktop(g_awm_selmon->sel);
		focus(NULL);
		arrange(g_awm_selmon);
	}
}

void
tagmon(const Arg *arg)
{
	if (!g_awm_selmon->sel || g_awm.n_monitors <= 1)
		return;
	sendmon(g_awm_selmon->sel, dirtomon(arg->i));
}

void
togglefloating(const Arg *arg)
{
	if (!g_awm_selmon->sel)
		return;
	if (g_awm_selmon->sel->isfullscreen)
		return;
	g_awm_selmon->sel->isfloating =
	    !g_awm_selmon->sel->isfloating || g_awm_selmon->sel->isfixed;
	if (g_awm_selmon->sel->isfloating)
		resize(g_awm_selmon->sel, g_awm_selmon->sel->x, g_awm_selmon->sel->y,
		    g_awm_selmon->sel->w, g_awm_selmon->sel->h, 0);
	arrange(g_awm_selmon);
	wmstate_update();
}

void
togglescratch(const Arg *arg)
{
	Client      *c;
	unsigned int found = 0;

	for (c = g_awm.clients_head;
	     c && !(found = c->scratchkey == ((char **) arg->v)[0][0]);
	     c = c->next)
		;
	if (found) {
		if (ISVISIBLE(c, g_awm_selmon)) {
			/* Hide: remove from all tags */
			c->tags = 0;
			focus(NULL);
			arrange(g_awm_selmon);
		} else {
			/* Show: move to g_awm_selmon, re-centre if changing monitor */
			if (c->mon != g_awm_selmon) {
				detachstack(c);
				c->mon = g_awm_selmon;
				attachstack(c);
				/* Re-centre on the new monitor */
				c->x = g_awm_selmon->mx + (g_awm_selmon->mw - WIDTH(c)) / 2;
				c->y = g_awm_selmon->my + (g_awm_selmon->mh - HEIGHT(c)) / 2;
			}
			c->tags = g_awm_selmon->tagset[g_awm_selmon->seltags];
			arrange(g_awm_selmon);
			focus(c);
			restack(g_awm_selmon);
		}
	} else {
		spawnscratch(arg);
	}
}

void
toggletag(const Arg *arg)
{
	unsigned int newtags;

	if (!g_awm_selmon->sel)
		return;
	newtags = g_awm_selmon->sel->tags ^ (arg->ui & TAGMASK);
	if (newtags) {
		g_awm_selmon->sel->tags = newtags;
		wmprop_set_client_desktop(g_awm_selmon->sel);
		focus(NULL);
		arrange(g_awm_selmon);
	}
	wmprop_update_current_desktop();
	wmstate_update();
}

void
toggleview(const Arg *arg)
{
	Monitor     *m;
	unsigned int newtagset =
	    g_awm_selmon->tagset[g_awm_selmon->seltags] ^ (arg->ui & TAGMASK);
	int i;

	if (newtagset) {
		FOR_EACH_MON(m)
		if (m != g_awm_selmon && newtagset & m->tagset[m->seltags]) {
			int selmon_curtag, m_curtag, j;

			if (g_awm_selmon->tagset[g_awm_selmon->seltags] == ~0)
				selmon_curtag = 0;
			else {
				for (i = 0;
				     !(g_awm_selmon->tagset[g_awm_selmon->seltags] & 1 << i);
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

			g_awm_selmon->pertag.nmasters[m_curtag] =
			    m->pertag.nmasters[m_curtag];
			g_awm_selmon->pertag.mfacts[m_curtag] = m->pertag.mfacts[m_curtag];
			g_awm_selmon->pertag.sellts[m_curtag] = m->pertag.sellts[m_curtag];
			g_awm_selmon->pertag.showbars[m_curtag] =
			    m->pertag.showbars[m_curtag];
			g_awm_selmon->pertag.drawwithgaps[m_curtag] =
			    m->pertag.drawwithgaps[m_curtag];
			g_awm_selmon->pertag.gappx[m_curtag] = m->pertag.gappx[m_curtag];
			for (j = 0; j < 2; j++)
				g_awm_selmon->pertag.ltidxs[(m_curtag) * 2 + (j)] =
				    m->pertag.ltidxs[(m_curtag) * 2 + (j)];

			m->pertag.nmasters[selmon_curtag] =
			    g_awm_selmon->pertag.nmasters[selmon_curtag];
			m->pertag.mfacts[selmon_curtag] =
			    g_awm_selmon->pertag.mfacts[selmon_curtag];
			m->pertag.sellts[selmon_curtag] =
			    g_awm_selmon->pertag.sellts[selmon_curtag];
			m->pertag.showbars[selmon_curtag] =
			    g_awm_selmon->pertag.showbars[selmon_curtag];
			m->pertag.drawwithgaps[selmon_curtag] =
			    g_awm_selmon->pertag.drawwithgaps[selmon_curtag];
			m->pertag.gappx[selmon_curtag] =
			    g_awm_selmon->pertag.gappx[selmon_curtag];
			for (j = 0; j < 2; j++)
				m->pertag.ltidxs[(selmon_curtag) * 2 + (j)] =
				    g_awm_selmon->pertag.ltidxs[(selmon_curtag) * 2 + (j)];

			m->sel = g_awm_selmon->sel;
			m->seltags ^= 1;
			m->tagset[m->seltags] =
			    g_awm_selmon->tagset[g_awm_selmon->seltags];
			m->pertag.curtag = selmon_curtag;

			m->nmaster = m->pertag.nmasters[m->pertag.curtag];
			m->mfact   = m->pertag.mfacts[m->pertag.curtag];
			m->sellt   = m->pertag.sellts[m->pertag.curtag];
			m->lt[m->sellt] =
			    m->pertag.ltidxs[(m->pertag.curtag) * 2 + (m->sellt)];
			m->lt[m->sellt ^ 1] =
			    m->pertag.ltidxs[(m->pertag.curtag) * 2 + (m->sellt ^ 1)];
			if (m->showbar != m->pertag.showbars[m->pertag.curtag])
				togglebar(NULL);

			/* Update g_awm_selmon's tagset before calling attachclients on
			 * either monitor.  If we called attachclients(m) first,
			 * it would steal clients whose tag matches m's new tagset
			 * (which is g_awm_selmon's old tag) — including clients that
			 * were sitting on g_awm_selmon and should stay there once
			 * g_awm_selmon picks up m's old tag.  Setting both tagsets
			 * atomically first lets attachclients see the final state and
			 * assign each client to the correct monitor. */
			g_awm_selmon->tagset[g_awm_selmon->seltags] = newtagset;
			g_awm_selmon->pertag.prevtag = g_awm_selmon->pertag.curtag;
			g_awm_selmon->pertag.curtag  = m_curtag;

			g_awm_selmon->nmaster =
			    g_awm_selmon->pertag.nmasters[g_awm_selmon->pertag.curtag];
			g_awm_selmon->mfact =
			    g_awm_selmon->pertag.mfacts[g_awm_selmon->pertag.curtag];
			g_awm_selmon->sellt =
			    g_awm_selmon->pertag.sellts[g_awm_selmon->pertag.curtag];
			g_awm_selmon->lt[g_awm_selmon->sellt] =
			    g_awm_selmon->pertag.ltidxs[(g_awm_selmon->pertag.curtag) * 2 +
			        (g_awm_selmon->sellt)];
			g_awm_selmon->lt[g_awm_selmon->sellt ^ 1] =
			    g_awm_selmon->pertag.ltidxs[(g_awm_selmon->pertag.curtag) * 2 +
			        (g_awm_selmon->sellt ^ 1)];
			if (g_awm_selmon->showbar !=
			    g_awm_selmon->pertag.showbars[g_awm_selmon->pertag.curtag])
				togglebar(NULL);

			attachclients(m);
			arrange(m);

#ifdef COMPOSITOR
			compositor_check_unredirect();
#endif

			attachclients(g_awm_selmon);
			arrange(g_awm_selmon);
			focus(NULL);
			wmprop_update_current_desktop();
			return;
		}

		g_awm_selmon->tagset[g_awm_selmon->seltags] = newtagset;

		if (newtagset == ~0) {
			g_awm_selmon->pertag.prevtag = g_awm_selmon->pertag.curtag;
			g_awm_selmon->pertag.curtag  = 0;
		}

		if (!(newtagset & 1 << (g_awm_selmon->pertag.curtag - 1))) {
			g_awm_selmon->pertag.prevtag = g_awm_selmon->pertag.curtag;
			for (i = 0; !(newtagset & 1 << i); i++)
				;
			g_awm_selmon->pertag.curtag = i + 1;
		}

		g_awm_selmon->nmaster =
		    g_awm_selmon->pertag.nmasters[g_awm_selmon->pertag.curtag];
		g_awm_selmon->mfact =
		    g_awm_selmon->pertag.mfacts[g_awm_selmon->pertag.curtag];
		g_awm_selmon->sellt =
		    g_awm_selmon->pertag.sellts[g_awm_selmon->pertag.curtag];
		g_awm_selmon->lt[g_awm_selmon->sellt] =
		    g_awm_selmon->pertag.ltidxs[(g_awm_selmon->pertag.curtag) * 2 +
		        (g_awm_selmon->sellt)];
		g_awm_selmon->lt[g_awm_selmon->sellt ^ 1] =
		    g_awm_selmon->pertag.ltidxs[(g_awm_selmon->pertag.curtag) * 2 +
		        (g_awm_selmon->sellt ^ 1)];

		if (g_awm_selmon->showbar !=
		    g_awm_selmon->pertag.showbars[g_awm_selmon->pertag.curtag])
			togglebar(NULL);

		attachclients(g_awm_selmon);
		arrange(g_awm_selmon);
		focus(NULL);
	}
	wmprop_update_current_desktop();
	wmstate_update();
}

void
unfocus(Client *c, int setfocus)
{
	if (!c)
		return;
	grabbuttons(c, 0);
	{
		uint32_t pix = scheme[SchemeNorm][ColBorder].pixel;
		g_wm_backend->change_attr(&g_plat, c->win, XCB_CW_BORDER_PIXEL, &pix);
	}
#ifdef COMPOSITOR
	compositor_focus_window(c);
#endif
	if (setfocus) {
		g_wm_backend->set_input_focus(&g_plat, g_plat.root, XCB_CURRENT_TIME);
		g_wm_backend->delete_prop(
		    &g_plat, g_plat.root, g_plat.netatom[NetActiveWindow]);
	}
}

void
unmanage(Client *c, int destroyed)
{
	Monitor *m = c->mon;

	detach(c);
	detachstack(c);
	if (!destroyed) {

		g_wm_backend->grab_server(&g_plat);
		{
			uint32_t no_events = XCB_EVENT_MASK_NO_EVENT;
			g_wm_backend->change_attr(
			    &g_plat, c->win, XCB_CW_EVENT_MASK, &no_events);
		}
		{
			uint32_t bw = (uint32_t) c->oldbw;
			g_wm_backend->configure_win(
			    &g_plat, c->win, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
		}
		g_wm_backend->ungrab_button(&g_plat, c->win);
		setclientstate(c, AWM_WM_STATE_WITHDRAWN);
		g_wm_backend->ungrab_server(&g_plat);
		g_wm_backend->flush(&g_plat);
	}
	freeicon(c);
#ifdef COMPOSITOR
	compositor_remove_window(c);
#endif
	free(c);
	focus(NULL);
	wmprop_update_client_list();
	arrange(m);
#ifdef COMPOSITOR
	/* If the destroyed window was fullscreen and had bypassed the compositor,
	 * paused_mask is still set.  Recompute unredirect state now so compositing
	 * resumes immediately rather than waiting for the 5-second watchdog. */
	compositor_check_unredirect();
#endif
	wmstate_update();
}

void
updatesizehints(Client *c)
{
	AwmSizeHints size;

	if (!g_wm_backend->get_wm_normal_hints(&g_plat, c->win, &size))
		size.flags = AWM_SIZE_P_SIZE;
	if (size.flags & AWM_SIZE_P_BASE_SIZE) {
		c->basew = size.base_w;
		c->baseh = size.base_h;
	} else if (size.flags & AWM_SIZE_P_MIN_SIZE) {
		c->basew = size.min_w;
		c->baseh = size.min_h;
	} else
		c->basew = c->baseh = 0;
	if (size.flags & AWM_SIZE_P_RESIZE_INC) {
		c->incw = size.inc_w;
		c->inch = size.inc_h;
	} else
		c->incw = c->inch = 0;
	if (size.flags & AWM_SIZE_P_MAX_SIZE) {
		c->maxw = size.max_w;
		c->maxh = size.max_h;
	} else
		c->maxw = c->maxh = 0;
	if (size.flags & AWM_SIZE_P_MIN_SIZE) {
		c->minw = size.min_w;
		c->minh = size.min_h;
	} else if (size.flags & AWM_SIZE_P_BASE_SIZE) {
		c->minw = size.base_w;
		c->minh = size.base_h;
	} else
		c->minw = c->minh = 0;
	if (size.flags & AWM_SIZE_P_ASPECT) {
		c->mina =
		    (size.min_aspect > 0.0) ? (float) (1.0 / size.min_aspect) : 0.0f;
		c->maxa = (float) size.max_aspect;
	} else
		c->maxa = c->mina = 0.0;
	c->isfixed =
	    (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
	c->hintsvalid = 1;
}

void
updatetitle(Client *c)
{
	if (!gettextprop(
	        c->win, g_plat.netatom[NetWMName], c->name, sizeof c->name))
		gettextprop(c->win, AWM_ATOM_WM_NAME, c->name, sizeof c->name);
	if (c->name[0] == '\0')
		strcpy(c->name, broken);
}

void
updatewindowtype(Client *c)
{
	xcb_atom_t state = getatomprop(c, g_plat.netatom[NetWMState]);
	xcb_atom_t wtype = getatomprop(c, g_plat.netatom[NetWMWindowType]);

	if (state == g_plat.netatom[NetWMFullscreen])
		setfullscreen(c, 1);
	if (wtype == g_plat.netatom[NetWMWindowTypeDialog]) {
		c->iscentered = 1;
		c->isfloating = 1;
	}
	if (wtype == g_plat.netatom[NetWMWindowTypeDock] ||
	    wtype == g_plat.netatom[NetWMWindowTypeToolbar] ||
	    wtype == g_plat.netatom[NetWMWindowTypeUtility] ||
	    wtype == g_plat.netatom[NetWMWindowTypeSplash])
		c->isfloating = 1;
}

void
updatewmhints(Client *c)
{
	AwmWmHints wmh;

	if (g_wm_backend->get_wm_hints(&g_plat, c->win, &wmh)) {
		if (c == g_awm_selmon->sel && (wmh.flags & AWM_WM_HINT_URGENCY)) {
			wmh.flags &= ~AWM_WM_HINT_URGENCY;
			g_wm_backend->set_wm_hints(&g_plat, c->win, &wmh);
		} else
			c->isurgent = (wmh.flags & AWM_WM_HINT_URGENCY) ? 1 : 0;
		if (wmh.flags & AWM_WM_HINT_INPUT)
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
	unsigned int newtagset = g_awm_selmon->tagset[g_awm_selmon->seltags ^ 1];

	if ((arg->ui & TAGMASK) == g_awm_selmon->tagset[g_awm_selmon->seltags])
		return;
	if (arg->ui & TAGMASK)
		newtagset = arg->ui & TAGMASK;
	FOR_EACH_MON(m)
	if (m != g_awm_selmon && newtagset & m->tagset[m->seltags]) {
		if (newtagset & g_awm_selmon->tagset[g_awm_selmon->seltags])
			return;
		int selmon_curtag, m_curtag, j;

		if (g_awm_selmon->tagset[g_awm_selmon->seltags] == ~0)
			selmon_curtag = 0;
		else {
			for (i = 0;
			     !(g_awm_selmon->tagset[g_awm_selmon->seltags] & 1 << i); i++)
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

		g_awm_selmon->pertag.nmasters[m_curtag] = m->pertag.nmasters[m_curtag];
		g_awm_selmon->pertag.mfacts[m_curtag]   = m->pertag.mfacts[m_curtag];
		g_awm_selmon->pertag.sellts[m_curtag]   = m->pertag.sellts[m_curtag];
		g_awm_selmon->pertag.showbars[m_curtag] = m->pertag.showbars[m_curtag];
		g_awm_selmon->pertag.drawwithgaps[m_curtag] =
		    m->pertag.drawwithgaps[m_curtag];
		g_awm_selmon->pertag.gappx[m_curtag] = m->pertag.gappx[m_curtag];
		for (j = 0; j < 2; j++)
			g_awm_selmon->pertag.ltidxs[(m_curtag) * 2 + (j)] =
			    m->pertag.ltidxs[(m_curtag) * 2 + (j)];

		m->pertag.nmasters[selmon_curtag] =
		    g_awm_selmon->pertag.nmasters[selmon_curtag];
		m->pertag.mfacts[selmon_curtag] =
		    g_awm_selmon->pertag.mfacts[selmon_curtag];
		m->pertag.sellts[selmon_curtag] =
		    g_awm_selmon->pertag.sellts[selmon_curtag];
		m->pertag.showbars[selmon_curtag] =
		    g_awm_selmon->pertag.showbars[selmon_curtag];
		m->pertag.drawwithgaps[selmon_curtag] =
		    g_awm_selmon->pertag.drawwithgaps[selmon_curtag];
		m->pertag.gappx[selmon_curtag] =
		    g_awm_selmon->pertag.gappx[selmon_curtag];
		for (j = 0; j < 2; j++)
			m->pertag.ltidxs[(selmon_curtag) * 2 + (j)] =
			    g_awm_selmon->pertag.ltidxs[(selmon_curtag) * 2 + (j)];

		m->sel = g_awm_selmon->sel;
		m->seltags ^= 1;
		m->tagset[m->seltags] = g_awm_selmon->tagset[g_awm_selmon->seltags];
		m->pertag.curtag      = selmon_curtag;

		m->nmaster = m->pertag.nmasters[m->pertag.curtag];
		m->mfact   = m->pertag.mfacts[m->pertag.curtag];
		m->sellt   = m->pertag.sellts[m->pertag.curtag];
		m->lt[m->sellt] =
		    m->pertag.ltidxs[(m->pertag.curtag) * 2 + (m->sellt)];
		m->lt[m->sellt ^ 1] =
		    m->pertag.ltidxs[(m->pertag.curtag) * 2 + (m->sellt ^ 1)];
		if (m->showbar != m->pertag.showbars[m->pertag.curtag])
			togglebar(NULL);

		/* Set g_awm_selmon's tagset before calling attachclients on
		 * either monitor — same ordering fix as in toggleview(). */
		g_awm_selmon->seltags ^= 1;
		g_awm_selmon->tagset[g_awm_selmon->seltags] = newtagset;
		g_awm_selmon->pertag.prevtag = g_awm_selmon->pertag.curtag;
		g_awm_selmon->pertag.curtag  = m_curtag;

		g_awm_selmon->nmaster =
		    g_awm_selmon->pertag.nmasters[g_awm_selmon->pertag.curtag];
		g_awm_selmon->mfact =
		    g_awm_selmon->pertag.mfacts[g_awm_selmon->pertag.curtag];
		g_awm_selmon->sellt =
		    g_awm_selmon->pertag.sellts[g_awm_selmon->pertag.curtag];
		g_awm_selmon->lt[g_awm_selmon->sellt] =
		    g_awm_selmon->pertag.ltidxs[(g_awm_selmon->pertag.curtag) * 2 +
		        (g_awm_selmon->sellt)];
		g_awm_selmon->lt[g_awm_selmon->sellt ^ 1] =
		    g_awm_selmon->pertag.ltidxs[(g_awm_selmon->pertag.curtag) * 2 +
		        (g_awm_selmon->sellt ^ 1)];
		if (g_awm_selmon->showbar !=
		    g_awm_selmon->pertag.showbars[g_awm_selmon->pertag.curtag])
			togglebar(NULL);

		attachclients(m);
		arrange(m);

#ifdef COMPOSITOR
		compositor_check_unredirect();
#endif

		attachclients(g_awm_selmon);
		arrange(g_awm_selmon);
		focus(NULL);
		wmprop_update_current_desktop();
		return;
	}
	g_awm_selmon->seltags ^= 1;
	if (arg->ui & TAGMASK) {
		g_awm_selmon->tagset[g_awm_selmon->seltags] = arg->ui & TAGMASK;
		g_awm_selmon->pertag.prevtag = g_awm_selmon->pertag.curtag;

		if (arg->ui == ~0)
			g_awm_selmon->pertag.curtag = 0;
		else {
			for (i = 0; !(arg->ui & 1 << i); i++)
				;
			g_awm_selmon->pertag.curtag = i + 1;
		}
	} else {
		tmptag                       = g_awm_selmon->pertag.prevtag;
		g_awm_selmon->pertag.prevtag = g_awm_selmon->pertag.curtag;
		g_awm_selmon->pertag.curtag  = tmptag;
	}

	g_awm_selmon->nmaster =
	    g_awm_selmon->pertag.nmasters[g_awm_selmon->pertag.curtag];
	g_awm_selmon->mfact =
	    g_awm_selmon->pertag.mfacts[g_awm_selmon->pertag.curtag];
	g_awm_selmon->sellt =
	    g_awm_selmon->pertag.sellts[g_awm_selmon->pertag.curtag];
	g_awm_selmon->lt[g_awm_selmon->sellt] =
	    g_awm_selmon->pertag
	        .ltidxs[(g_awm_selmon->pertag.curtag) * 2 + (g_awm_selmon->sellt)];
	g_awm_selmon->lt[g_awm_selmon->sellt ^ 1] =
	    g_awm_selmon->pertag.ltidxs[(g_awm_selmon->pertag.curtag) * 2 +
	        (g_awm_selmon->sellt ^ 1)];

	if (g_awm_selmon->showbar !=
	    g_awm_selmon->pertag.showbars[g_awm_selmon->pertag.curtag])
		togglebar(NULL);

	attachclients(g_awm_selmon);
	arrange(g_awm_selmon);
	focus(NULL);
	wmprop_update_current_desktop();
	wmstate_update();
}

void
warp(const Client *c)
{
	int x, y;

	if (!c) {
		g_wm_backend->warp_pointer(&g_plat, g_plat.root,
		    (int16_t) (g_awm_selmon->wx + g_awm_selmon->ww / 2),
		    (int16_t) (g_awm_selmon->wy + g_awm_selmon->wh / 2));
		return;
	}

	if (!getrootptr(&x, &y) ||
	    (x > c->x - c->bw && y > c->y - c->bw && x < c->x + c->w + c->bw * 2 &&
	        y < c->y + c->h + c->bw * 2) ||
	    (y > c->mon->by && y < c->mon->by + g_plat.bh) ||
	    (c->mon->topbar && !y))
		return;

	g_wm_backend->warp_pointer(
	    &g_plat, c->win, (int16_t) (c->w / 2), (int16_t) (c->h / 2));
}

Client *
wintoclient(xcb_window_t w)
{
	if (!win_to_client)
		return NULL;
	return g_hash_table_lookup(win_to_client, GUINT_TO_POINTER(w));
}

void
zoom(const Arg *arg)
{
	Client *c = g_awm_selmon->sel;

	if (!g_awm_selmon->lt[g_awm_selmon->sellt]->arrange || !c || c->isfloating)
		return;
	if (c == nexttiled(g_awm.clients_head, g_awm_selmon) &&
	    !(c = nexttiled(c->next, g_awm_selmon)))
		return;
	pop(c);
}

void
movestack(const Arg *arg)
{
	Client *c = NULL, *p = NULL, *pc = NULL, *i;

	if (arg->i > 0) {
		/* find the client after g_awm_selmon->sel */
		for (c = g_awm_selmon->sel->next;
		     c && (!ISVISIBLE(c, g_awm_selmon) || c->isfloating); c = c->next)
			;
		if (!c)
			for (c = g_awm.clients_head;
			     c && (!ISVISIBLE(c, g_awm_selmon) || c->isfloating);
			     c = c->next)
				;

	} else {
		/* find the client before g_awm_selmon->sel */
		for (i = g_awm.clients_head; i != g_awm_selmon->sel; i = i->next) {
			if (ISVISIBLE(i, g_awm_selmon) && !i->isfloating)
				c = i;
		}
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i, g_awm_selmon) && !i->isfloating)
					c = i;
	}
	/* find the client before g_awm_selmon->sel and c */
	for (i = g_awm.clients_head; i && (!p || !pc); i = i->next) {
		if (i->next == g_awm_selmon->sel)
			p = i;
		if (i->next == c)
			pc = i;
	}

	/* swap c and g_awm_selmon->sel in the clients list */
	if (c && c != g_awm_selmon->sel) {
		Client *temp = g_awm_selmon->sel->next == c ? g_awm_selmon->sel
		                                            : g_awm_selmon->sel->next;
		g_awm_selmon->sel->next = c->next == g_awm_selmon->sel ? c : c->next;
		c->next                 = temp;

		if (p && p != c)
			p->next = c;
		if (pc && pc != g_awm_selmon->sel)
			pc->next = g_awm_selmon->sel;

		if (g_awm_selmon->sel == g_awm.clients_head)
			g_awm.clients_head = c;
		else if (c == g_awm.clients_head)
			g_awm.clients_head = g_awm_selmon->sel;

		arrange(g_awm_selmon);
	}
}
