/* AndrathWM - EWMH/ICCCM protocol functions
 * See LICENSE file for copyright and license details. */

#include <stdint.h>

#include "awm.h"
#include "client.h"
#include "ewmh.h"
#include "monitor.h"
#include "spawn.h"
#include "xrdb.h"
#include "config.h"

void
setcurrentdesktop(void)
{
	uint32_t data[] = { 0 };
	xcb_change_property(xc, XCB_PROP_MODE_REPLACE, root,
	    netatom[NetCurrentDesktop], XCB_ATOM_CARDINAL, 32, 1, data);
}

void
setdesktopnames(void)
{
	char                     buf[1024];
	size_t                   off = 0;
	int                      i;
	xcb_intern_atom_cookie_t ck;
	xcb_intern_atom_reply_t *r;
	xcb_atom_t               utf8str;

	/* Build NUL-separated blob of tag names for _NET_DESKTOP_NAMES */
	for (i = 0; i < (int) TAGSLENGTH; i++) {
		size_t len = strlen(tags[i]);
		if (off + len + 1 > sizeof(buf))
			break;
		memcpy(buf + off, tags[i], len);
		off += len;
		buf[off++] = '\0';
	}
	ck      = xcb_intern_atom(xc, 0, 11, "UTF8_STRING");
	r       = xcb_intern_atom_reply(xc, ck, NULL);
	utf8str = r ? r->atom : XCB_ATOM_STRING;
	free(r);
	xcb_change_property(xc, XCB_PROP_MODE_REPLACE, root,
	    netatom[NetDesktopNames], utf8str, 8, (uint32_t) off, buf);
}

int
sendevent(xcb_window_t w, xcb_atom_t proto, int mask, long d0, long d1,
    long d2, long d3, long d4)
{
	xcb_atom_t mt;
	int        exists = 0;

	if (proto == wmatom[WMTakeFocus] || proto == wmatom[WMDelete]) {
		mt = wmatom[WMProtocols];
		{
			xcb_get_property_cookie_t ck = xcb_get_property(
			    xc, 0, w, wmatom[WMProtocols], XCB_ATOM_ATOM, 0, 1024);
			xcb_get_property_reply_t *r = xcb_get_property_reply(xc, ck, NULL);
			if (r) {
				int np = xcb_get_property_value_length(r) /
				    (int) sizeof(xcb_atom_t);
				xcb_atom_t *pa = xcb_get_property_value(r);
				while (!exists && np--)
					exists = pa[np] == proto;
				free(r);
			}
		}
	} else {
		exists = 1;
		mt     = proto;
	}

	if (exists) {
		xcb_client_message_event_t ev;
		ev.response_type  = XCB_CLIENT_MESSAGE;
		ev.format         = 32;
		ev.sequence       = 0;
		ev.window         = w;
		ev.type           = mt;
		ev.data.data32[0] = (uint32_t) d0;
		ev.data.data32[1] = (uint32_t) d1;
		ev.data.data32[2] = (uint32_t) d2;
		ev.data.data32[3] = (uint32_t) d3;
		ev.data.data32[4] = (uint32_t) d4;
		xcb_send_event(xc, 0, w, (uint32_t) mask, (const char *) &ev);
	}
	return exists;
}

void
setnumdesktops(void)
{
	uint32_t data[] = { TAGSLENGTH };
	xcb_change_property(xc, XCB_PROP_MODE_REPLACE, root,
	    netatom[NetNumberOfDesktops], XCB_ATOM_CARDINAL, 32, 1, data);
}

void
setfocus(Client *c)
{
	if (!c->neverfocus) {
		xcb_set_input_focus(xc, XCB_INPUT_FOCUS_POINTER_ROOT,
		    (xcb_window_t) c->win, XCB_CURRENT_TIME);
		uint32_t win32 = (uint32_t) c->win;
		xcb_change_property(xc, XCB_PROP_MODE_REPLACE, root,
		    netatom[NetActiveWindow], XCB_ATOM_WINDOW, 32, 1, &win32);
	}
	sendevent(c->win, wmatom[WMTakeFocus], 0, wmatom[WMTakeFocus],
	    XCB_CURRENT_TIME, 0, 0, 0);
}

void
setviewport(void)
{
	uint32_t data[] = { 0, 0 };
	xcb_change_property(xc, XCB_PROP_MODE_REPLACE, root,
	    netatom[NetDesktopViewport], XCB_ATOM_CARDINAL, 32, 2, data);
}

void
updateclientlist(void)
{
	Client  *c;
	Monitor *m;

	xcb_delete_property(xc, root, netatom[NetClientList]);
	for (m = mons; m; m = m->next)
		if (m->cl) /* Safety check */
			for (c = m->cl->clients; c; c = c->next) {
				uint32_t win32 = (uint32_t) c->win;
				xcb_change_property(xc, XCB_PROP_MODE_APPEND, root,
				    netatom[NetClientList], XCB_ATOM_WINDOW, 32, 1, &win32);
			}

	/* Update _NET_CLIENT_LIST_STACKING in bottom-to-top order */
	xcb_delete_property(xc, root, netatom[NetClientListStacking]);
	for (m = mons; m; m = m->next)
		if (m->cl) /* Safety check */
			for (c = m->cl->stack; c; c = c->snext) {
				uint32_t win32 = (uint32_t) c->win;
				xcb_change_property(xc, XCB_PROP_MODE_APPEND, root,
				    netatom[NetClientListStacking], XCB_ATOM_WINDOW, 32, 1,
				    &win32);
			}
}

void
updatecurrentdesktop(void)
{
	unsigned int rawdata = selmon->tagset[selmon->seltags];
	uint32_t     i       = 0;
	while (rawdata >> (i + 1))
		i++;
	xcb_change_property(xc, XCB_PROP_MODE_REPLACE, root,
	    netatom[NetCurrentDesktop], XCB_ATOM_CARDINAL, 32, 1, &i);
}

void
setwmstate(Client *c)
{
	xcb_atom_t state[8];
	uint32_t   n = 0;

	if (c->isfullscreen)
		state[n++] = netatom[NetWMFullscreen];
	if (c->isurgent)
		state[n++] = netatom[NetWMStateDemandsAttention];
	if (c->ishidden)
		state[n++] = netatom[NetWMStateHidden];

	xcb_change_property(xc, XCB_PROP_MODE_REPLACE, c->win, netatom[NetWMState],
	    XCB_ATOM_ATOM, 32, n, state);
}

void
setewmhdesktop(Client *c)
{
	uint32_t data;
	int      i;

	/* Calculate desktop number from tags */
	for (i = 0; i < LENGTH(tags) && !(c->tags & (1 << i)); i++)
		;

	data = (i < LENGTH(tags)) ? (uint32_t) i
	                          : 0xFFFFFFFFu; /* 0xFFFFFFFF = all desktops */

	xcb_change_property(xc, XCB_PROP_MODE_REPLACE, c->win,
	    netatom[NetWMDesktop], XCB_ATOM_CARDINAL, 32, 1, &data);
}

void
updateworkarea(Monitor *m)
{
	uint32_t data[4];

	/* Calculate workarea (screen minus bar) */
	data[0] = (uint32_t) m->wx;
	data[1] = (uint32_t) m->wy;
	data[2] = (uint32_t) m->ww;
	data[3] = (uint32_t) m->wh;

	xcb_change_property(xc, XCB_PROP_MODE_REPLACE, root, netatom[NetWorkarea],
	    XCB_ATOM_CARDINAL, 32, 4, data);
}

unsigned long
getembedinfo(Client *c)
{
	xcb_get_property_cookie_t ck;
	xcb_get_property_reply_t *rep;
	unsigned long             flags = 0;

	ck = xcb_get_property(
	    xc, 0, c->win, xatom[XembedInfo], xatom[XembedInfo], 0, 2);
	rep = xcb_get_property_reply(xc, ck, NULL);
	if (rep) {
		if (xcb_get_property_value_length(rep) >=
		    (int) (2 * sizeof(uint32_t))) {
			uint32_t *vals = xcb_get_property_value(rep);
			flags          = vals[1];
		}
		free(rep);
	}
	return flags;
}
