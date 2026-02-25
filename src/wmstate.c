/* See LICENSE file for copyright and license details. */

/* -------------------------------------------------------------------------
 * wmstate.c — consolidated WM + compositor state implementation.
 *
 * wmstate_update() snapshots observable state (clients, compositor bypass)
 * into g_awm.  Monitor state is authoritative directly in g_awm.monitors[],
 * so no monitor snapshot loop is required here.
 * ---------------------------------------------------------------------- */

#include <string.h>

#include "awm.h"
#include "client.h"
#include "monitor.h"
#ifdef COMPOSITOR
#include "compositor.h"
#endif
#include "wmstate.h"

/* The single global AWMState instance. */
AWMState g_awm;

/* -------------------------------------------------------------------------
 * wmstate_update
 * ---------------------------------------------------------------------- */

void
wmstate_update(void)
{
	Client      *c;
	unsigned int ci;

	/* ---- Clients ---- */
	ci = 0;
	for (c = g_awm.cl->clients; c && ci < WMSTATE_MAX_CLIENTS;
	    c  = c->next, ci++) {
		AWMStateClient *wc = &g_awm.clients[ci];

		wc->win = c->win;
		strncpy(wc->name, c->name, WMSTATE_NAME_LEN - 1);
		wc->tags              = c->tags;
		wc->monitor_num       = c->mon ? c->mon->num : -1;
		wc->x                 = c->x;
		wc->y                 = c->y;
		wc->w                 = c->w;
		wc->h                 = c->h;
		wc->opacity           = c->opacity;
		wc->isfloating        = c->isfloating;
		wc->isfullscreen      = c->isfullscreen;
		wc->ishidden          = c->ishidden;
		wc->issteam           = c->issteam;
		wc->scratchkey        = c->scratchkey;
		wc->bypass_compositor = c->bypass_compositor;
	}
	g_awm.n_clients = ci;

	/* ---- Compositor bypass state ---- */
#ifdef COMPOSITOR
	g_awm.comp_paused_mask = compositor_paused_mask();
	g_awm.comp_paused      = compositor_is_paused();
#endif
}
