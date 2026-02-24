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
 *   - Monitor topology and per-tag layout/pertag state
 *   - Client persistent state (geometry, tags, flags)
 *   - Focus (selected monitor, selected client per monitor)
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
 *   extern AWMState g_awm;   (defined in awm.c)
 *
 * During the incremental migration, existing extern globals (mons, selmon,
 * cl, etc.) remain as they are.  Modules are migrated to g_awm field by
 * field; the build must stay green after every commit.
 *
 * Serialisation (session save/restore, JSON dump) is NOT implemented here.
 * That is a later step, once the consolidation is complete and stable.
 * ---------------------------------------------------------------------- */

#include <xcb/xcb.h>

/* Forward declarations — full definitions live in awm.h.
 * Guarded to avoid duplicate typedef when awm.h is already included. */
#ifndef AWM_H
typedef struct Monitor    Monitor;
typedef struct Clientlist Clientlist;
#endif

/* Maximum number of monitors supported in AWMState arrays. */
#define WMSTATE_MAX_MONITORS 8

/* Maximum number of clients tracked in AWMState. */
#define WMSTATE_MAX_CLIENTS 512

/* Maximum number of tags per monitor (must be >= TAGSLENGTH at runtime).
 * We use a compile-time constant here so AWMState is a plain value type with
 * no heap allocation — making it trivially copyable and comparable. */
#define WMSTATE_MAX_TAGS 16

/* Layout symbol max length (matches Monitor.ltsymbol). */
#define WMSTATE_LTSYM_LEN 16

/* Client name max length (matches Client.name). */
#define WMSTATE_NAME_LEN 256

/* -------------------------------------------------------------------------
 * AWMStateTag — per-tag (pertag slot) layout state for one monitor.
 * Mirrors the per-index fields of struct Pertag.
 * ---------------------------------------------------------------------- */
typedef struct {
	int          nmaster;
	float        mfact;
	unsigned int sellt;                /* index into lt_sym: 0 or 1 */
	char lt_sym[2][WMSTATE_LTSYM_LEN]; /* layout symbols for alt slots */
	int  showbar;
	int  drawwithgaps;
	unsigned int gappx;
} AWMStateTag;

/* -------------------------------------------------------------------------
 * AWMStateMonitor — per-monitor topology and layout state.
 * ---------------------------------------------------------------------- */
typedef struct {
	int          num;       /* monitor number (RandR index) */
	int          mx, my;    /* monitor origin (screen coords) */
	int          mw, mh;    /* monitor physical size */
	int          wx, wy;    /* window area origin (excludes bar) */
	int          ww, wh;    /* window area size */
	unsigned int seltags;   /* index into tagset[]: 0 or 1 */
	unsigned int tagset[2]; /* bitmask of visible tags (both slots) */
	unsigned int curtag;    /* current pertag index */
	unsigned int prevtag;   /* previous pertag index */
	char         ltsymbol[WMSTATE_LTSYM_LEN]; /* displayed layout symbol */
	xcb_window_t sel_win; /* XID of focused client (0 = none) */

	/* Per-tag state: indices 0..n_tags.
	 * Index 0 is the "all tags" slot; indices 1..n_tags are tag 1..n. */
	unsigned int n_tags; /* number of valid tag entries (= TAGSLENGTH) */
	AWMStateTag  tags[WMSTATE_MAX_TAGS + 1];
} AWMStateMonitor;

/* -------------------------------------------------------------------------
 * AWMStateClient — persistent per-client state.
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
	/* Live runtime pointers — the authoritative WM state.
	 * These replace the bare mons / selmon / cl globals.
	 * Set in setup() and kept current by wmstate_update(). */
	Monitor    *mons;   /* head of the monitor linked list          */
	Monitor    *selmon; /* currently focused monitor                */
	Clientlist *cl;     /* global client list (all clients + stack) */

	/* Monitor topology snapshot (for future serialisation) */
	unsigned int    n_monitors;
	AWMStateMonitor monitors[WMSTATE_MAX_MONITORS];
	int             selmon_num; /* number of focused monitor (-1 = none) */

	/* Client list snapshot (for future serialisation) */
	unsigned int   n_clients;
	AWMStateClient clients[WMSTATE_MAX_CLIENTS];
} AWMState;

/* The single global instance — defined in awm.c. */
extern AWMState g_awm;

/* -------------------------------------------------------------------------
 * wmstate_update() — snapshot current live globals into g_awm.
 *
 * Called after any significant state change to keep g_awm consistent.
 * During the incremental migration, the live globals (mons, selmon, cl,
 * comp.*) remain the authoritative runtime state; g_awm is kept in sync
 * by calling this function.  Once migration is complete, g_awm becomes
 * the authority and this function is replaced by direct writes.
 * ---------------------------------------------------------------------- */
void wmstate_update(void);

#endif /* WMSTATE_H */
