/* See LICENSE file for copyright and license details. */

/* -------------------------------------------------------------------------
 * wm_properties_stub.c — no-op stubs for non-X11 backends
 *
 * Compiled instead of wm_properties_x11.c when BACKEND != X11.
 * Replace individual stubs with real xdg-shell / Wayland equivalents
 * as each backend matures.
 * ---------------------------------------------------------------------- */

/* Suppress empty-translation-unit warning when built with -DBACKEND_X11. */
typedef int wm_properties_stub_dummy_t;

#ifndef BACKEND_X11

#include "awm.h"
#include "wm_properties.h"

void
wmprop_setup(void)
{
	(void) 0;
}
void
wmprop_set_focus(Client *c)
{
	(void) c;
}
void
wmprop_update_client_list(void)
{
	(void) 0;
}
void
wmprop_update_current_desktop(void)
{
	(void) 0;
}
void
wmprop_set_wm_state(Client *c)
{
	(void) c;
}
void
wmprop_set_client_desktop(Client *c)
{
	(void) c;
}
void
wmprop_update_workarea(Monitor *m)
{
	(void) m;
}

int
wmprop_send_event(xcb_window_t w, xcb_atom_t proto, int mask, long d0, long d1,
    long d2, long d3, long d4)
{
	(void) w;
	(void) proto;
	(void) mask;
	(void) d0;
	(void) d1;
	(void) d2;
	(void) d3;
	(void) d4;
	return 0;
}

#endif /* !BACKEND_X11 */
