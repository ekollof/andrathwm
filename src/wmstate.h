/* See LICENSE file for copyright and license details. */
#ifndef WMSTATE_H
#define WMSTATE_H

/* -------------------------------------------------------------------------
 * wmstate.h — consolidated WM + compositor state.
 *
 * AWMState is the single source of truth for all restartable, observable
 * window manager state.  The goal is disambiguation: every field that
 * matters lives here, state transitions are explicit, and invariants can
 * be enforced at the struct boundary rather than scattered across call sites.
 *
 * What lives here:
 *   - Monitor flat array (g_awm.monitors[]) — the authoritative monitor list
 *   - Focus: g_awm.selmon_num (index into monitors[])
 *   - Client list: g_awm.cl (live pointer)
 *   - Compositor bypass/redirect state
 *
 * What does NOT live here (runtime-only, set-once in setup()):
 *   - Rendering resources (Drw, scheme, cursors)
 *   - Atom caches (wmatom, netatom, xatom)
 *   - Input state (keysyms, numlockmask)
 *   - IPC fds (ui_fd, ui_pid)
 *   - X connection (xc, root, screen)
 *
 * The single global instance is:
 *   extern AWMState g_awm;   (defined in wmstate.c)
 *
 * Monitor access macros (use these instead of g_awm.mons / g_awm.selmon):
 *
 *   g_awm_selmon
 *       Pointer to the currently focused Monitor.  Replaces g_awm.selmon
 *       reads.  Evaluates to &g_awm.monitors[g_awm.selmon_num].
 *
 *   g_awm_set_selmon(m)
 *       Record monitor m as the focused monitor.  Replaces
 *       g_awm.selmon = m assignments.
 *
 *   FOR_EACH_MON(var)
 *       Loop over every monitor in creation order, assigning a Monitor *
 *       to var on each iteration.  Replaces
 *       for (m = g_awm.mons; m; m = m->next) loops.
 *       var must be declared as Monitor * before the loop.
 *
 * Serialisation (session save/restore, JSON dump) is NOT implemented here.
 * That is a later step, once the consolidation is complete and stable.
 * ---------------------------------------------------------------------- */

#include <stdint.h>
#include <xcb/xcb.h>
/* awm.h provides the full Monitor and Clientlist definitions needed for the
 * inline monitors[] array.  Every .c file that uses wmstate.h already
 * includes awm.h first; including it here as well is safe because awm.h
 * has its own include guard. */
#include "awm.h"

/* Maximum number of monitors supported in the flat monitors[] array. */
#define WMSTATE_MAX_MONITORS 8

/* Maximum number of clients tracked in AWMState. */
#define WMSTATE_MAX_CLIENTS 512

/* Client name max length (matches Client.name). */
#define WMSTATE_NAME_LEN 256

/* -------------------------------------------------------------------------
 * AWMStateClient — persistent per-client state (snapshot for future use).
 * ---------------------------------------------------------------------- */
typedef struct {
	xcb_window_t win; /* X window XID */
	char         name[WMSTATE_NAME_LEN];
	unsigned int tags;        /* tag bitmask */
	int          monitor_num; /* owning monitor number */
	int          x, y, w, h;  /* current geometry */
	double       opacity;     /* compositing opacity 0.0–1.0 */
	int          isfloating;
	int          isfullscreen;
	int          ishidden;
	int          issteam;
	char         scratchkey;        /* '\0' = not a scratchpad */
	int          bypass_compositor; /* _NET_WM_BYPASS_COMPOSITOR hint */
} AWMStateClient;

/* -------------------------------------------------------------------------
 * AWMState — the top-level consolidated state struct.
 * ---------------------------------------------------------------------- */
typedef struct {
	/* ---- Monitor flat array — authoritative monitor state ----
	 * monitors[0..n_monitors-1] are live Monitor values.
	 * selmon_num is the index of the focused monitor (-1 = none).
	 * These replace the old g_awm.mons linked list and g_awm.selmon
	 * pointer.  Use the access macros below instead of these fields
	 * directly where possible. */
	unsigned int n_monitors;
	int          selmon_num;
	Monitor      monitors[WMSTATE_MAX_MONITORS];

	/* ---- Client list — live pointer ----
	 * g_awm.cl is the shared client list (all clients + stack).
	 * Set in setup() and never replaced. */
	Clientlist *cl;

	/* ---- Compositor bypass/unredirect state ----
	 * paused_mask: bitmask — bit N set means monitor N is bypassed.
	 * paused: 1 when ALL monitors are bypassed (repaint loop stopped).
	 * The live authority is CompShared comp in compositor.c; these
	 * fields are synced by wmstate_update() for observability. */
	uint32_t comp_paused_mask;
	int      comp_paused;

	/* ---- Client list snapshot (for future serialisation) ---- */
	unsigned int   n_clients;
	AWMStateClient clients[WMSTATE_MAX_CLIENTS];
} AWMState;

/* The single global instance — defined in wmstate.c. */
extern AWMState g_awm;

/* -------------------------------------------------------------------------
 * Monitor access macros.
 * ---------------------------------------------------------------------- */

/* Pointer to the currently focused monitor. */
#define g_awm_selmon (&g_awm.monitors[g_awm.selmon_num])

/* Set focused monitor by pointer (pointer must be into g_awm.monitors[]). */
#define g_awm_set_selmon(m) (g_awm.selmon_num = (int) ((m) - g_awm.monitors))

/* Iterate over all monitors.  var must be declared as Monitor * before use.
 * _femi is a hidden loop index; the body sees var as a valid Monitor *. */
#define FOR_EACH_MON(var)                                 \
	for (int _femi = 0; _femi < (int) g_awm.n_monitors && \
	    ((var) = &g_awm.monitors[_femi], 1);              \
	    _femi++)

/* -------------------------------------------------------------------------
 * wmstate_update() — sync observable state into g_awm.
 *
 * Refreshes the client snapshot and compositor state fields.  Monitor state
 * is now authoritative directly in g_awm.monitors[], so no monitor snapshot
 * loop is needed here.
 * ---------------------------------------------------------------------- */
void wmstate_update(void);

#endif /* WMSTATE_H */
