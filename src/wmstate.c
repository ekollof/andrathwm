/* See LICENSE file for copyright and license details. */

/* -------------------------------------------------------------------------
 * wmstate.c — consolidated WM + compositor state implementation.
 *
 * wmstate_update() syncs compositor bypass state into g_awm.  Monitor state
 * is authoritative directly in g_awm.monitors[]; client list heads are
 * inline in g_awm; no snapshot loops are needed here.
 * ---------------------------------------------------------------------- */

#include "awm.h"
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
	/* ---- Compositor bypass state ---- */
#ifdef COMPOSITOR
	g_awm.comp_paused_mask = compositor_paused_mask();
	g_awm.comp_paused      = compositor_is_paused();
#endif
}
