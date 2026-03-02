/* See LICENSE file for copyright and license details. */
/*
 * Tests for pure-logic functions in src/monitor.c:
 *   updatebarpos() -- bar geometry calculation
 *   dirtomon()     -- directional monitor selection
 *   recttomon()    -- rectangle-to-monitor mapping
 *
 * Strategy: include monitor.c directly after providing stubs for every
 * external symbol it references.  No X connection is opened at runtime.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"

/* Pull in all required type definitions. */
#include "../src/awm.h"
#include "../src/wmstate.h"
#include "../src/drw.h"
#include "../src/systray.h"
#include "../src/monitor.h"
#include "../src/client.h"
#include "../src/spawn.h"

/* -------------------------------------------------------------------------
 * Globals required by extern declarations in awm.h / wmstate.h.
 * ---------------------------------------------------------------------- */
xcb_connection_t  *xc             = NULL;
xcb_window_t       root           = 0;
xcb_window_t       wmcheckwin     = 0;
int                screen         = 0;
int                sw             = 1920;
int                sh             = 1080;
int                bh             = 20;
int                lrpad          = 0;
int                awm_tagslength = 9;
double             ui_dpi         = 96.0;
double             ui_scale       = 1.0;
Drw               *drw            = NULL;
Clr              **scheme         = NULL;
Cur               *cursor[CurLast];
Systray           *systray = NULL;
xcb_atom_t         wmatom[WMLast];
xcb_atom_t         netatom[NetLast];
xcb_atom_t         xatom[XLast];
xcb_atom_t         utf8string_atom = 0;
char               stext[STATUS_TEXT_LEN];
int                restart          = 0;
int                barsdirty        = 0;
int                launcher_visible = 0;
xcb_window_t       launcher_xwin    = 0;
unsigned int       numlockmask      = 0;
xcb_timestamp_t    last_event_time  = 0;
xcb_key_symbols_t *keysyms          = NULL;
unsigned int       ui_borderpx      = 1;
unsigned int       ui_snap          = 32;
unsigned int       ui_iconsize      = 16;
unsigned int       ui_gappx         = 0;
#ifdef XRANDR
int randrbase = 0;
int rrerrbase = 0;
#endif
void (*handler[LASTEvent])(xcb_generic_event_t *);

AWMState g_awm;

/* -------------------------------------------------------------------------
 * Stub functions for symbols called by monitor.c but not under test.
 * Functions defined in monitor.c itself (arrange, arrangemon, drawbar, etc.)
 * are NOT stubbed here -- they are provided by the #include below.
 * ---------------------------------------------------------------------- */

/* drw */
Clr *
drw_scm_create(Drw *d, char *clrnames[], size_t clrcount)
{
	(void) d;
	(void) clrnames;
	(void) clrcount;
	return NULL;
}

/* systray */
void
updatesystrayiconcolors(void)
{
}

unsigned int
getsystraywidth(void)
{
	return 0;
}

void
updatesystray(void)
{
}

/* client */
void
focus(Client *c)
{
	(void) c;
}

void
unfocus(Client *c, int setfocus)
{
	(void) c;
	(void) setfocus;
}

void
warp(const Client *c)
{
	(void) c;
}

Client *
wintoclient(xcb_window_t w)
{
	(void) w;
	return NULL;
}

void
updateclientlist(void)
{
}

int
getrootptr(int *x, int *y)
{
	(void) x;
	(void) y;
	return 0;
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
resize(Client *c, int x, int y, int w, int h, int interact)
{
	(void) c;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) interact;
}

void
resizeclient(Client *c, int x, int y, int w, int h)
{
	(void) c;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
}

void
showhide(Client *c)
{
	(void) c;
}

void
attachclients(Monitor *m)
{
	(void) m;
}

void
setewmhdesktop(Client *c)
{
	(void) c;
}

void
setwmstate(Client *c)
{
	(void) c;
}

/* ewmh */
void
updateworkarea(Monitor *m)
{
	(void) m;
}

void
updatecurrentdesktop(void)
{
}

/* compositor */
#ifdef COMPOSITOR
void
compositor_handle_event(xcb_generic_event_t *xe)
{
	(void) xe;
}

void
compositor_raise_overlay(void)
{
}

void
compositor_check_unredirect(void)
{
}

void
compositor_set_hidden(Client *c, int hidden)
{
	(void) c;
	(void) hidden;
}
#endif

/* wmstate */
void
wmstate_update(void)
{
}

/* xrdb */
void
xrdb(const Arg *arg)
{
	(void) arg;
}

/* ui */
void
ui_send_theme(void)
{
}

/* spawn */
void
spawnscratch(const Arg *arg)
{
	(void) arg;
}

/* -------------------------------------------------------------------------
 * Stubs for log.h functions (monitor.c calls awm_error which calls log_error)
 * ---------------------------------------------------------------------- */
void
log_debug(const char *func, int line, const char *fmt, ...)
{
	(void) func;
	(void) line;
	(void) fmt;
}

void
log_info(const char *func, int line, const char *fmt, ...)
{
	(void) func;
	(void) line;
	(void) fmt;
}

void
log_warn(const char *func, int line, const char *fmt, ...)
{
	(void) func;
	(void) line;
	(void) fmt;
}

void
log_error(const char *func, int line, const char *fmt, ...)
{
	(void) func;
	(void) line;
	(void) fmt;
}

/* -------------------------------------------------------------------------
 * Stubs for util.h functions
 * ---------------------------------------------------------------------- */
void *
ecalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb, size);
	if (!p)
		abort();
	return p;
}

_Noreturn void
die(const char *fmt, ...)
{
	(void) fmt;
	abort();
}

/* -------------------------------------------------------------------------
 * Stubs for drw.h functions called by drawbar / updatebars in monitor.c
 * ---------------------------------------------------------------------- */
void
drw_setscheme(Drw *drw, Clr *scm)
{
	(void) drw;
	(void) scm;
}

void
drw_rect(Drw *drw, int x, int y, unsigned int w, unsigned int h, int filled,
    int invert)
{
	(void) drw;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) filled;
	(void) invert;
}

int
drw_text(Drw *drw, int x, int y, unsigned int w, unsigned int h,
    unsigned int lpad, const char *text, int invert)
{
	(void) drw;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) lpad;
	(void) text;
	(void) invert;
	return 0;
}

void
drw_pic(Drw *drw, int x, int y, unsigned int w, unsigned int h,
    cairo_surface_t *surface)
{
	(void) drw;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) surface;
}

int
drw_draw_statusd(
    Drw *drw, int x, int y, unsigned int w, unsigned int h, const char *text)
{
	(void) drw;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) text;
	return 0;
}

unsigned int
drw_fontset_getwidth(Drw *drw, const char *text)
{
	(void) drw;
	(void) text;
	return 0;
}

void
drw_map(
    Drw *drw, xcb_window_t win, int x, int y, unsigned int w, unsigned int h)
{
	(void) drw;
	(void) win;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
}

/* -------------------------------------------------------------------------
 * Include monitor.c directly to access all functions under test.
 * config.h must be the last include inside it (satisfied by monitor.c).
 * ---------------------------------------------------------------------- */
#define AWM_CONFIG_IMPL
#include "../src/monitor.c"

/* -------------------------------------------------------------------------
 * Helper: initialise g_awm with n monitors laid out horizontally.
 * Each monitor is 1280 x 800, spaced 1280 px apart.
 * bh=20, topbar=1, showbar=1.
 * ---------------------------------------------------------------------- */
static void
setup_monitors(unsigned int n)
{
	unsigned int i;

	memset(&g_awm, 0, sizeof g_awm);
	bh               = 20;
	g_awm.n_monitors = n;
	g_awm.selmon_num = 0;
	for (i = 0; i < n; i++) {
		Monitor *m = &g_awm.monitors[i];
		memset(m, 0, sizeof *m);
		m->mx = m->wx = (int) (i * 1280);
		m->my = m->wy = 0;
		m->mw = m->ww = 1280;
		m->mh = m->wh    = 800;
		m->showbar       = 1;
		m->topbar        = 1;
		m->pertag.curtag = 1;
		m->lt[0]         = &layouts[0];
		m->lt[1]         = &layouts[1 % LENGTH(layouts)];
	}
}

/* =========================================================================
 * updatebarpos tests
 * ====================================================================== */

TEST
updatebarpos_topbar_showbar(void)
{
	Monitor m;
	memset(&m, 0, sizeof m);
	bh        = 20;
	m.my      = 0;
	m.mh      = 800;
	m.showbar = 1;
	m.topbar  = 1;

	updatebarpos(&m);

	/* Work area: y=20, h=780.  Bar: y=0. */
	ASSERT_EQ(20, m.wy);
	ASSERT_EQ(780, m.wh);
	ASSERT_EQ(0, m.by);
	PASS();
}

TEST
updatebarpos_bottombar_showbar(void)
{
	Monitor m;
	memset(&m, 0, sizeof m);
	bh        = 20;
	m.my      = 0;
	m.mh      = 800;
	m.showbar = 1;
	m.topbar  = 0;

	updatebarpos(&m);

	/* Work area: y=0, h=780.  Bar: y=780. */
	ASSERT_EQ(0, m.wy);
	ASSERT_EQ(780, m.wh);
	ASSERT_EQ(780, m.by);
	PASS();
}

TEST
updatebarpos_nobar(void)
{
	Monitor m;
	memset(&m, 0, sizeof m);
	bh        = 20;
	m.my      = 0;
	m.mh      = 800;
	m.showbar = 0;
	m.topbar  = 1;

	updatebarpos(&m);

	/* Work area: y=0, h=800.  Bar: y=-bh (hidden). */
	ASSERT_EQ(0, m.wy);
	ASSERT_EQ(800, m.wh);
	ASSERT_EQ(-20, m.by);
	PASS();
}

TEST
updatebarpos_nonzero_my(void)
{
	Monitor m;
	memset(&m, 0, sizeof m);
	bh        = 24;
	m.my      = 100;
	m.mh      = 600;
	m.showbar = 1;
	m.topbar  = 1;

	updatebarpos(&m);

	/* wy = my + bh = 124.  wh = mh - bh = 576.  by = my = 100. */
	ASSERT_EQ(124, m.wy);
	ASSERT_EQ(576, m.wh);
	ASSERT_EQ(100, m.by);
	PASS();
}

TEST
updatebarpos_topbar_preserves_mx_mw(void)
{
	Monitor m;
	memset(&m, 0, sizeof m);
	bh        = 20;
	m.mx      = 50;
	m.mw      = 1280;
	m.my      = 0;
	m.mh      = 800;
	m.showbar = 1;
	m.topbar  = 1;

	updatebarpos(&m);

	/* mx and mw must not be touched */
	ASSERT_EQ(50, m.mx);
	ASSERT_EQ(1280, m.mw);
	PASS();
}

/* =========================================================================
 * dirtomon tests
 * ====================================================================== */

TEST
dirtomon_single_monitor_forward(void)
{
	setup_monitors(1);
	/* With only one monitor, dirtomon always returns selmon. */
	ASSERT_EQ(g_awm_selmon, dirtomon(1));
	PASS();
}

TEST
dirtomon_single_monitor_backward(void)
{
	setup_monitors(1);
	ASSERT_EQ(g_awm_selmon, dirtomon(-1));
	PASS();
}

TEST
dirtomon_two_monitors_forward(void)
{
	setup_monitors(2);
	g_awm.selmon_num = 0;
	/* Forward from 0 -> 1 */
	ASSERT_EQ(&g_awm.monitors[1], dirtomon(1));
	PASS();
}

TEST
dirtomon_two_monitors_backward(void)
{
	setup_monitors(2);
	g_awm.selmon_num = 0;
	/* Backward from 0 wraps to 1 */
	ASSERT_EQ(&g_awm.monitors[1], dirtomon(-1));
	PASS();
}

TEST
dirtomon_three_monitors_wraps_forward(void)
{
	setup_monitors(3);
	g_awm.selmon_num = 2;
	/* Forward from last wraps to 0 */
	ASSERT_EQ(&g_awm.monitors[0], dirtomon(1));
	PASS();
}

TEST
dirtomon_three_monitors_wraps_backward(void)
{
	setup_monitors(3);
	g_awm.selmon_num = 0;
	/* Backward from 0 wraps to 2 */
	ASSERT_EQ(&g_awm.monitors[2], dirtomon(-1));
	PASS();
}

/* =========================================================================
 * recttomon tests
 * ====================================================================== */

TEST
recttomon_single_monitor(void)
{
	setup_monitors(1);
	/* Any rectangle intersects the only monitor */
	ASSERT_EQ(&g_awm.monitors[0], recttomon(100, 100, 50, 50));
	PASS();
}

TEST
recttomon_left_monitor(void)
{
	setup_monitors(2);
	/* Rectangle well inside monitor 0 (x: 0..1279) */
	ASSERT_EQ(&g_awm.monitors[0], recttomon(100, 100, 200, 200));
	PASS();
}

TEST
recttomon_right_monitor(void)
{
	setup_monitors(2);
	/* Rectangle well inside monitor 1 (x: 1280..2559) */
	ASSERT_EQ(&g_awm.monitors[1], recttomon(1500, 100, 200, 200));
	PASS();
}

TEST
recttomon_spanning_picks_larger_area(void)
{
	setup_monitors(2);
	/* Rectangle x=1200, w=200: 80px on mon0 (1200..1279),
	 * 120px on mon1 (1280..1399).  Mon1 has larger intersection. */
	ASSERT_EQ(&g_awm.monitors[1], recttomon(1200, 100, 200, 200));
	PASS();
}

TEST
recttomon_off_screen_returns_selmon(void)
{
	setup_monitors(2);
	g_awm.selmon_num = 0;
	/* Rectangle entirely off-screen: INTERSECT returns 0 for all monitors;
	 * recttomon falls back to selmon. */
	ASSERT_EQ(g_awm_selmon, recttomon(-500, -500, 10, 10));
	PASS();
}

/* =========================================================================
 * Suites
 * ====================================================================== */

SUITE(suite_updatebarpos)
{
	RUN_TEST(updatebarpos_topbar_showbar);
	RUN_TEST(updatebarpos_bottombar_showbar);
	RUN_TEST(updatebarpos_nobar);
	RUN_TEST(updatebarpos_nonzero_my);
	RUN_TEST(updatebarpos_topbar_preserves_mx_mw);
}

SUITE(suite_dirtomon)
{
	RUN_TEST(dirtomon_single_monitor_forward);
	RUN_TEST(dirtomon_single_monitor_backward);
	RUN_TEST(dirtomon_two_monitors_forward);
	RUN_TEST(dirtomon_two_monitors_backward);
	RUN_TEST(dirtomon_three_monitors_wraps_forward);
	RUN_TEST(dirtomon_three_monitors_wraps_backward);
}

SUITE(suite_recttomon)
{
	RUN_TEST(recttomon_single_monitor);
	RUN_TEST(recttomon_left_monitor);
	RUN_TEST(recttomon_right_monitor);
	RUN_TEST(recttomon_spanning_picks_larger_area);
	RUN_TEST(recttomon_off_screen_returns_selmon);
}

/* =========================================================================
 * main
 * ====================================================================== */

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
	GREATEST_MAIN_BEGIN();
	RUN_SUITE(suite_updatebarpos);
	RUN_SUITE(suite_dirtomon);
	RUN_SUITE(suite_recttomon);
	GREATEST_MAIN_END();
}
