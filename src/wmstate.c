/* See LICENSE file for copyright and license details. */

/* -------------------------------------------------------------------------
 * wmstate.c — consolidated WM + compositor state implementation.
 *
 * wmstate_update() snapshots the current live globals (mons, selmon, cl,
 * comp.*) into the single g_awm instance.  During the incremental migration
 * the live globals remain authoritative; once migration is complete g_awm
 * becomes the authority and direct writes replace this snapshot approach.
 * ---------------------------------------------------------------------- */

#include <string.h>

#include "awm.h"
#include "client.h"
#include "monitor.h"
#include "wmstate.h"
#ifdef COMPOSITOR
#include "compositor.h"
#endif

/* The single global AWMState instance. */
AWMState g_awm;

/* -------------------------------------------------------------------------
 * comp_win_visitor — callback for compositor_for_each_window()
 * ud points to an unsigned int index counter.
 * ---------------------------------------------------------------------- */
#ifdef COMPOSITOR
static void
comp_win_visitor(xcb_window_t win, int redirected, int hidden, void *ud)
{
	unsigned int *idx = (unsigned int *) ud;

	if (*idx >= WMSTATE_MAX_CLIENTS)
		return;
	g_awm.comp.comp_windows[*idx].win        = win;
	g_awm.comp.comp_windows[*idx].redirected = redirected;
	g_awm.comp.comp_windows[*idx].hidden     = hidden;
	(*idx)++;
}
#endif

/* -------------------------------------------------------------------------
 * wmstate_update
 * ---------------------------------------------------------------------- */

void
wmstate_update(void)
{
	Monitor     *m;
	Client      *c;
	unsigned int mi, ci, ti;

	memset(&g_awm, 0, sizeof g_awm);

	/* ---- Live runtime pointers (authoritative) ---- */
	g_awm.mons   = mons;
	g_awm.selmon = selmon;
	g_awm.cl     = cl;

	/* ---- Monitors ---- */
	mi = 0;
	for (m = mons; m && mi < WMSTATE_MAX_MONITORS; m = m->next, mi++) {
		AWMStateMonitor *wm = &g_awm.monitors[mi];

		wm->num       = m->num;
		wm->mx        = m->mx;
		wm->my        = m->my;
		wm->mw        = m->mw;
		wm->mh        = m->mh;
		wm->wx        = m->wx;
		wm->wy        = m->wy;
		wm->ww        = m->ww;
		wm->wh        = m->wh;
		wm->seltags   = m->seltags;
		wm->tagset[0] = m->tagset[0];
		wm->tagset[1] = m->tagset[1];
		wm->curtag    = m->pertag->curtag;
		wm->prevtag   = m->pertag->prevtag;
		strncpy(wm->ltsymbol, m->ltsymbol, WMSTATE_LTSYM_LEN - 1);
		wm->sel_win = m->sel ? m->sel->win : 0;

		/* Per-tag pertag state */
		wm->n_tags = (unsigned int) awm_tagslength;
		for (ti = 0;
		    ti <= (unsigned int) awm_tagslength && ti <= WMSTATE_MAX_TAGS;
		    ti++) {
			AWMStateTag  *wt = &wm->tags[ti];
			const Layout *la = m->pertag->ltidxs[ti * 2 + 0];
			const Layout *lb = m->pertag->ltidxs[ti * 2 + 1];

			wt->nmaster      = m->pertag->nmasters[ti];
			wt->mfact        = m->pertag->mfacts[ti];
			wt->sellt        = m->pertag->sellts[ti];
			wt->showbar      = m->pertag->showbars[ti];
			wt->drawwithgaps = m->pertag->drawwithgaps[ti];
			wt->gappx        = m->pertag->gappx[ti];
			strncpy(
			    wt->lt_sym[0], la ? la->symbol : "", WMSTATE_LTSYM_LEN - 1);
			strncpy(
			    wt->lt_sym[1], lb ? lb->symbol : "", WMSTATE_LTSYM_LEN - 1);
		}
	}
	g_awm.n_monitors = mi;
	g_awm.selmon_num = selmon ? selmon->num : -1;

	/* ---- Clients ---- */
	ci = 0;
	for (c = cl->clients; c && ci < WMSTATE_MAX_CLIENTS; c = c->next, ci++) {
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

	/* ---- Compositor ---- */
#ifdef COMPOSITOR
	{
		unsigned int cwi = 0;

		g_awm.comp.active      = compositor_is_active();
		g_awm.comp.paused      = compositor_is_paused();
		g_awm.comp.paused_mask = compositor_paused_mask();
		compositor_for_each_window(comp_win_visitor, &cwi);
		g_awm.comp.n_comp_windows = cwi;
	}
#endif
}
