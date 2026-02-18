/* AndrathWM - EWMH/ICCCM protocol functions
 * See LICENSE file for copyright and license details. */

#include "awm.h"
#include "monitor.h"
#include "client.h"
#include "spawn.h"
#include "events.h"
#include "ewmh.h"
#include "systray.h"
#include "xrdb.h"
#include "config.h"
#include "pertag.h"

void
setcurrentdesktop(void)
{
	long data[] = { 0 };
	XChangeProperty(dpy, root, netatom[NetCurrentDesktop], XA_CARDINAL, 32,
	    PropModeReplace, (unsigned char *) data, 1);
}

void
setdesktopnames(void)
{
	XTextProperty text;
	Xutf8TextListToTextProperty(
	    dpy, (char **) tags, TAGSLENGTH, XUTF8StringStyle, &text);
	XSetTextProperty(dpy, root, &text, netatom[NetDesktopNames]);
}

int
sendevent(Window w, Atom proto, int mask, long d0, long d1, long d2, long d3,
    long d4)
{
	int    n;
	Atom  *protocols, mt;
	int    exists = 0;
	XEvent ev;

	if (proto == wmatom[WMTakeFocus] || proto == wmatom[WMDelete]) {
		mt = wmatom[WMProtocols];
		if (XGetWMProtocols(dpy, w, &protocols, &n)) {
			while (!exists && n--)
				exists = protocols[n] == proto;
			XFree(protocols);
		}
	} else {
		exists = True;
		mt     = proto;
	}

	if (exists) {
		ev.type                 = ClientMessage;
		ev.xclient.window       = w;
		ev.xclient.message_type = mt;
		ev.xclient.format       = 32;
		ev.xclient.data.l[0]    = d0;
		ev.xclient.data.l[1]    = d1;
		ev.xclient.data.l[2]    = d2;
		ev.xclient.data.l[3]    = d3;
		ev.xclient.data.l[4]    = d4;
		XSendEvent(dpy, w, False, mask, &ev);
	}
	return exists;
}

void
setnumdesktops(void)
{
	long data[] = { TAGSLENGTH };
	XChangeProperty(dpy, root, netatom[NetNumberOfDesktops], XA_CARDINAL, 32,
	    PropModeReplace, (unsigned char *) data, 1);
}

void
setfocus(Client *c)
{
	if (!c->neverfocus) {
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
		XChangeProperty(dpy, root, netatom[NetActiveWindow], XA_WINDOW, 32,
		    PropModeReplace, (unsigned char *) &(c->win), 1);
	}
	sendevent(c->win, wmatom[WMTakeFocus], NoEventMask, wmatom[WMTakeFocus],
	    CurrentTime, 0, 0, 0);
}

void
setviewport(void)
{
	long data[] = { 0, 0 };
	XChangeProperty(dpy, root, netatom[NetDesktopViewport], XA_CARDINAL, 32,
	    PropModeReplace, (unsigned char *) data, 2);
}

void
updateclientlist(void)
{
	Client  *c;
	Monitor *m;

	XDeleteProperty(dpy, root, netatom[NetClientList]);
	for (m = mons; m; m = m->next)
		if (m->cl) /* Safety check */
			for (c = m->cl->clients; c; c = c->next)
				XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW,
				    32, PropModeAppend, (unsigned char *) &(c->win), 1);

	/* Update _NET_CLIENT_LIST_STACKING in bottom-to-top order */
	XDeleteProperty(dpy, root, netatom[NetClientListStacking]);
	for (m = mons; m; m = m->next)
		if (m->cl) /* Safety check */
			for (c = m->cl->stack; c; c = c->snext)
				XChangeProperty(dpy, root, netatom[NetClientListStacking],
				    XA_WINDOW, 32, PropModeAppend, (unsigned char *) &(c->win),
				    1);
}

void
updatecurrentdesktop(void)
{
	long rawdata[] = { selmon->tagset[selmon->seltags] };
	int  i         = 0;
	while (*rawdata >> (i + 1)) {
		i++;
	}
	long data[] = { i };
	XChangeProperty(dpy, root, netatom[NetCurrentDesktop], XA_CARDINAL, 32,
	    PropModeReplace, (unsigned char *) data, 1);
}

void
setwmstate(Client *c)
{
	Atom state[8];
	int  i = 0;

	if (c->isfullscreen)
		state[i++] = netatom[NetWMFullscreen];
	if (c->isurgent)
		state[i++] = netatom[NetWMStateDemandsAttention];
	if (c->ishidden)
		state[i++] = netatom[NetWMStateHidden];

	XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
	    PropModeReplace, (unsigned char *) state, i);
}

void
setewmhdesktop(Client *c)
{
	long data[] = { 0 };
	int  i;

	/* Calculate desktop number from tags */
	for (i = 0; i < LENGTH(tags) && !(c->tags & (1 << i)); i++)
		;

	if (i < LENGTH(tags))
		data[0] = i;
	else
		data[0] = 0xFFFFFFFF; /* All desktops/sticky */

	XChangeProperty(dpy, c->win, netatom[NetWMDesktop], XA_CARDINAL, 32,
	    PropModeReplace, (unsigned char *) data, 1);
}

void
updateworkarea(Monitor *m)
{
	long data[4];

	/* Calculate workarea (screen minus bar) */
	data[0] = m->wx;
	data[1] = m->wy;
	data[2] = m->ww;
	data[3] = m->wh;

	XChangeProperty(dpy, root, netatom[NetWorkarea], XA_CARDINAL, 32,
	    PropModeReplace, (unsigned char *) data, 4);
}

unsigned long
getembedinfo(Client *c)
{
	int            di;
	unsigned long  dl;
	unsigned char *p = NULL;
	Atom           da;
	unsigned long  flags = 0;

	if (XGetWindowProperty(dpy, c->win, xatom[XembedInfo], 0L, 2, False,
	        xatom[XembedInfo], &da, &di, &dl, &dl, &p) == Success &&
	    p) {
		if (dl == 2)
			flags = ((unsigned long *) p)[1];
		XFree(p);
	}
	return flags;
}
