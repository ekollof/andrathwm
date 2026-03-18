/* See LICENSE file for copyright and license details. */

/* -------------------------------------------------------------------------
 * wm_properties_x11.c — X11 implementation of the wm_properties interface
 *
 * Thin wrappers that forward to ewmh.c.  On X11 this is the only
 * implementation compiled; wm_properties_stub.c is provided for future
 * non-X11 backends.
 * ---------------------------------------------------------------------- */

#ifdef BACKEND_X11

#include "awm.h"
#include "ewmh.h"
#include "wm_properties.h"

void
wmprop_setup(void)
{
	setnumdesktops();
	setcurrentdesktop();
	setdesktopnames();
	setviewport();
}

void
wmprop_set_focus(Client *c)
{
	setfocus(c);
}

void
wmprop_update_client_list(void)
{
	updateclientlist();
}

void
wmprop_update_current_desktop(void)
{
	updatecurrentdesktop();
}

void
wmprop_set_wm_state(Client *c)
{
	setwmstate(c);
}

void
wmprop_set_client_desktop(Client *c)
{
	setewmhdesktop(c);
}

void
wmprop_update_workarea(Monitor *m)
{
	updateworkarea(m);
}

int
wmprop_send_event(xcb_window_t w, xcb_atom_t proto, int mask, long d0, long d1,
    long d2, long d3, long d4)
{
	return sendevent(w, proto, mask, d0, d1, d2, d3, d4);
}

#endif /* BACKEND_X11 */
