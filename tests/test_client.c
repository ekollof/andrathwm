/* See LICENSE file for copyright and license details. */
/*
 * Tests for pure-logic functions in src/client.c:
 *   nexttiled()    -- skip floating/invisible/hidden clients
 *   attachstack()  -- prepend client onto MRU stack
 *   detachstack()  -- remove client from MRU stack, update sel
 *
 * Strategy: include client.c directly after providing stubs for every
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
 * Globals that moved into PlatformCtx are provided via g_plat below.
 * ---------------------------------------------------------------------- */
int             awm_tagslength = 9;
Drw            *drw            = NULL;
Clr           **scheme         = NULL;
Cur            *cursor[CurLast];
Systray        *systray = NULL;
char            stext[STATUS_TEXT_LEN];
int             restart          = 0;
int             barsdirty        = 0;
int             launcher_visible = 0;
xcb_window_t    launcher_xwin    = 0;
xcb_timestamp_t last_event_time  = 0;
void (*handler[LASTEvent])(xcb_generic_event_t *);

/* Platform context — zero-initialised; tests do not open an X connection. */
PlatformCtx g_plat = {
	.xc          = NULL,
	.root        = 0,
	.wmcheckwin  = 0,
	.screen      = 0,
	.sw          = 1920,
	.sh          = 1080,
	.bh          = 20,
	.lrpad       = 0,
	.ui_dpi      = 96.0,
	.ui_scale    = 1.0,
	.ui_borderpx = 1,
	.ui_snap     = 32,
	.ui_iconsize = 16,
	.ui_gappx    = 0,
};

AWMState g_awm;

/* -------------------------------------------------------------------------
 * Stubs for log.h functions.
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
 * Stubs for util.h functions.
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
 * Stubs for drw.h functions (drawbar / updatebars call these).
 * ---------------------------------------------------------------------- */
Clr *
drw_scm_create(Drw *d, char *clrnames[], size_t clrcount)
{
	(void) d;
	(void) clrnames;
	(void) clrcount;
	return NULL;
}

void
drw_setscheme(Drw *d, Clr *scm)
{
	(void) d;
	(void) scm;
}

void
drw_rect(Drw *d, int x, int y, unsigned int w, unsigned int h, int filled,
    int invert)
{
	(void) d;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) filled;
	(void) invert;
}

int
drw_text(Drw *d, int x, int y, unsigned int w, unsigned int h,
    unsigned int lpad, const char *text, int invert)
{
	(void) d;
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
drw_pic(Drw *d, int x, int y, unsigned int w, unsigned int h,
    cairo_surface_t *surface)
{
	(void) d;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) surface;
}

int
drw_draw_statusd(
    Drw *d, int x, int y, unsigned int w, unsigned int h, const char *text)
{
	(void) d;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) text;
	return 0;
}

unsigned int
drw_fontset_getwidth(Drw *d, const char *text)
{
	(void) d;
	(void) text;
	return 0;
}

void
drw_map(Drw *d, xcb_window_t win, int x, int y, unsigned int w, unsigned int h)
{
	(void) d;
	(void) win;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
}

/* -------------------------------------------------------------------------
 * Stubs for monitor.h functions called by client.c but not under test.
 * ---------------------------------------------------------------------- */
void
arrange(Monitor *m)
{
	(void) m;
}

void
arrangemon(Monitor *m)
{
	(void) m;
}

void
restack(Monitor *m)
{
	(void) m;
}

void
drawbar(Monitor *m)
{
	(void) m;
}

void
drawbars(void)
{
}

Monitor *
dirtomon(int dir)
{
	(void) dir;
	return g_awm_selmon;
}

Monitor *
recttomon(int x, int y, int w, int h)
{
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	return g_awm_selmon;
}

Monitor *
wintomon(xcb_window_t w)
{
	(void) w;
	return g_awm_selmon;
}

void
togglebar(const Arg *arg)
{
	(void) arg;
}

/* -------------------------------------------------------------------------
 * Stubs for ewmh.h / events.h functions.
 * ---------------------------------------------------------------------- */
void
updateclientlist(void)
{
}

void
updatecurrentdesktop(void)
{
}

void
updateworkarea(Monitor *m)
{
	(void) m;
}

void
setfocus(Client *c)
{
	(void) c;
}

void
setwmstate(Client *c)
{
	(void) c;
}

void
setewmhdesktop(Client *c)
{
	(void) c;
}

int
sendevent(xcb_window_t w, xcb_atom_t proto, int mask, long d0, long d1,
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

void
updatenumlockmask(void)
{
}

/* -------------------------------------------------------------------------
 * Stubs for systray.h functions.
 * ---------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------
 * Stubs for compositor.h functions.
 * ---------------------------------------------------------------------- */
#ifdef COMPOSITOR
void
compositor_add_window(Client *c)
{
	(void) c;
}

void
compositor_remove_window(Client *c)
{
	(void) c;
}

void
compositor_configure_window(Client *c, int actual_bw)
{
	(void) c;
	(void) actual_bw;
}

void
compositor_bypass_window(Client *c, int bypass)
{
	(void) c;
	(void) bypass;
}

void
compositor_defer_fullscreen_bypass(Client *c)
{
	(void) c;
}

void
compositor_handle_event(xcb_generic_event_t *ev)
{
	(void) ev;
}

void
compositor_focus_window(Client *c)
{
	(void) c;
}

void
compositor_raise_overlay(void)
{
}

void
compositor_repaint_now(void)
{
}

void
compositor_set_hidden(Client *c, int hidden)
{
	(void) c;
	(void) hidden;
}

void
compositor_check_unredirect(void)
{
}
#endif /* COMPOSITOR */

/* -------------------------------------------------------------------------
 * Stubs for wmstate.h / xrdb.h functions.
 * ---------------------------------------------------------------------- */
void
wmstate_update(void)
{
}

void
xrdb(const Arg *arg)
{
	(void) arg;
}

/* -------------------------------------------------------------------------
 * Stub for ui communication.
 * ---------------------------------------------------------------------- */
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

void
spawn(const Arg *arg)
{
	(void) arg;
}

/* Layout functions defined in monitor.c */
void
tile(Monitor *m)
{
	(void) m;
}

void
monocle(Monitor *m)
{
	(void) m;
}

/* -------------------------------------------------------------------------
 * Include client.c directly to access all functions under test.
 * AWM_CONFIG_IMPL is required because client.c ends with #include "config.h".
 * ---------------------------------------------------------------------- */
#define AWM_CONFIG_IMPL
#include "../src/client.c"

/* -------------------------------------------------------------------------
 * Helper: build a minimal Monitor and a chain of n Client structs.
 * Each client is assigned:
 *   tags = 1 (tag 0 selected)
 *   mon  = the monitor
 * The function also sets up g_awm with the monitor as selmon.
 * Clients are stored in caller-provided array c[].
 * ---------------------------------------------------------------------- */
static void
setup_clients(Monitor *m, Client *c, unsigned int n)
{
	unsigned int i;

	memset(&g_awm, 0, sizeof g_awm);
	memset(m, 0, sizeof *m);
	m->tagset[0]     = 1;
	m->pertag.curtag = 1;
	g_awm.n_monitors = 1;
	memcpy(&g_awm.monitors[0], m, sizeof *m);
	g_awm.selmon_num = 0;

	for (i = 0; i < n; i++) {
		memset(&c[i], 0, sizeof c[i]);
		c[i].win  = (xcb_window_t) (i + 1); /* unique nonzero window ids */
		c[i].tags = 1;                      /* visible on tag 0 */
		c[i].mon  = &g_awm.monitors[0];
	}
	/* Link clients in order: c[0] -> c[1] -> ... -> c[n-1] -> NULL */
	for (i = 0; i + 1 < n; i++)
		c[i].next = &c[i + 1];
	g_awm.clients_head = (n > 0) ? &c[0] : NULL;
	g_awm.stack_head   = NULL;
}

/* =========================================================================
 * nexttiled tests
 * ====================================================================== */

TEST
nexttiled_empty_list(void)
{
	Monitor m;
	setup_clients(&m, NULL, 0);
	/* NULL input -> NULL output */
	ASSERT_EQ(NULL, nexttiled(NULL, &g_awm.monitors[0]));
	PASS();
}

TEST
nexttiled_single_tiled(void)
{
	Monitor m;
	Client  c[1];
	setup_clients(&m, c, 1);
	/* One visible, non-floating, non-hidden client -> returns it */
	ASSERT_EQ(&c[0], nexttiled(&c[0], &g_awm.monitors[0]));
	PASS();
}

TEST
nexttiled_skips_floating(void)
{
	Monitor m;
	Client  c[2];
	setup_clients(&m, c, 2);
	c[0].isfloating = 1;
	/* c[0] is floating, c[1] is tiled */
	ASSERT_EQ(&c[1], nexttiled(&c[0], &g_awm.monitors[0]));
	PASS();
}

TEST
nexttiled_skips_hidden(void)
{
	Monitor m;
	Client  c[2];
	setup_clients(&m, c, 2);
	c[0].ishidden = 1;
	ASSERT_EQ(&c[1], nexttiled(&c[0], &g_awm.monitors[0]));
	PASS();
}

TEST
nexttiled_skips_invisible(void)
{
	Monitor m;
	Client  c[2];
	setup_clients(&m, c, 2);
	/* c[0] has tag 2 but monitor shows tag 1 */
	c[0].tags = 2;
	ASSERT_EQ(&c[1], nexttiled(&c[0], &g_awm.monitors[0]));
	PASS();
}

TEST
nexttiled_all_floating_returns_null(void)
{
	Monitor      m;
	Client       c[3];
	unsigned int i;
	setup_clients(&m, c, 3);
	for (i = 0; i < 3; i++)
		c[i].isfloating = 1;
	ASSERT_EQ(NULL, nexttiled(&c[0], &g_awm.monitors[0]));
	PASS();
}

TEST
nexttiled_starts_from_given_client(void)
{
	Monitor m;
	Client  c[3];
	setup_clients(&m, c, 3);
	c[0].isfloating = 1;
	c[1].isfloating = 1;
	/* Starting from c[0]: skip c[0], c[1]; return c[2] */
	ASSERT_EQ(&c[2], nexttiled(&c[0], &g_awm.monitors[0]));
	/* Starting from c[2] directly */
	ASSERT_EQ(&c[2], nexttiled(&c[2], &g_awm.monitors[0]));
	PASS();
}

/* =========================================================================
 * attachstack tests
 * ====================================================================== */

TEST
attachstack_onto_empty(void)
{
	Monitor m;
	Client  c[1];
	setup_clients(&m, c, 1);
	g_awm.stack_head = NULL;

	attachstack(&c[0]);

	ASSERT_EQ(&c[0], g_awm.stack_head);
	ASSERT_EQ(NULL, c[0].snext);
	PASS();
}

TEST
attachstack_prepends(void)
{
	Monitor m;
	Client  c[3];
	setup_clients(&m, c, 3);
	g_awm.stack_head = NULL;

	attachstack(&c[2]);
	attachstack(&c[1]);
	attachstack(&c[0]);

	/* Stack order: c[0] -> c[1] -> c[2] -> NULL */
	ASSERT_EQ(&c[0], g_awm.stack_head);
	ASSERT_EQ(&c[1], c[0].snext);
	ASSERT_EQ(&c[2], c[1].snext);
	ASSERT_EQ(NULL, c[2].snext);
	PASS();
}

/* =========================================================================
 * detachstack tests
 * ====================================================================== */

TEST
detachstack_head_of_one(void)
{
	Monitor m;
	Client  c[1];
	setup_clients(&m, c, 1);
	g_awm.stack_head      = &c[0];
	c[0].snext            = NULL;
	g_awm.monitors[0].sel = NULL; /* c[0] is not sel */

	detachstack(&c[0]);

	ASSERT_EQ(NULL, g_awm.stack_head);
	PASS();
}

TEST
detachstack_head_of_many(void)
{
	Monitor m;
	Client  c[3];
	setup_clients(&m, c, 3);
	/* Stack: c[0] -> c[1] -> c[2] */
	g_awm.stack_head      = &c[0];
	c[0].snext            = &c[1];
	c[1].snext            = &c[2];
	c[2].snext            = NULL;
	g_awm.monitors[0].sel = NULL;

	detachstack(&c[0]);

	ASSERT_EQ(&c[1], g_awm.stack_head);
	PASS();
}

TEST
detachstack_middle(void)
{
	Monitor m;
	Client  c[3];
	setup_clients(&m, c, 3);
	/* Stack: c[0] -> c[1] -> c[2] */
	g_awm.stack_head      = &c[0];
	c[0].snext            = &c[1];
	c[1].snext            = &c[2];
	c[2].snext            = NULL;
	g_awm.monitors[0].sel = NULL;

	detachstack(&c[1]);

	/* Stack: c[0] -> c[2] */
	ASSERT_EQ(&c[0], g_awm.stack_head);
	ASSERT_EQ(&c[2], c[0].snext);
	ASSERT_EQ(NULL, c[2].snext);
	PASS();
}

TEST
detachstack_tail(void)
{
	Monitor m;
	Client  c[3];
	setup_clients(&m, c, 3);
	/* Stack: c[0] -> c[1] -> c[2] */
	g_awm.stack_head      = &c[0];
	c[0].snext            = &c[1];
	c[1].snext            = &c[2];
	c[2].snext            = NULL;
	g_awm.monitors[0].sel = NULL;

	detachstack(&c[2]);

	/* Stack: c[0] -> c[1] */
	ASSERT_EQ(&c[0], g_awm.stack_head);
	ASSERT_EQ(&c[1], c[0].snext);
	ASSERT_EQ(NULL, c[1].snext);
	PASS();
}

TEST
detachstack_sel_updated_to_visible(void)
{
	Monitor m;
	Client  c[2];
	setup_clients(&m, c, 2);
	/*
	 * Stack: c[0] -> c[1].
	 * c[0] is sel; detaching c[0] should set sel to c[1]
	 * (the first remaining visible client on the stack).
	 */
	g_awm.stack_head      = &c[0];
	c[0].snext            = &c[1];
	c[1].snext            = NULL;
	g_awm.monitors[0].sel = &c[0];

	detachstack(&c[0]);

	/* sel must now point to the next visible client */
	ASSERT_EQ(&c[1], g_awm.monitors[0].sel);
	PASS();
}

TEST
detachstack_sel_null_when_none_visible(void)
{
	Monitor m;
	Client  c[2];
	setup_clients(&m, c, 2);
	/*
	 * Stack: c[0] -> c[1], but c[1] is on a different tag (invisible).
	 * After detaching c[0] (sel), sel should become NULL.
	 */
	g_awm.stack_head      = &c[0];
	c[0].snext            = &c[1];
	c[1].snext            = NULL;
	c[1].tags             = 2; /* not shown on tag 1 */
	g_awm.monitors[0].sel = &c[0];

	detachstack(&c[0]);

	ASSERT_EQ(NULL, g_awm.monitors[0].sel);
	PASS();
}

/* =========================================================================
 * Suites
 * ====================================================================== */

SUITE(suite_nexttiled)
{
	RUN_TEST(nexttiled_empty_list);
	RUN_TEST(nexttiled_single_tiled);
	RUN_TEST(nexttiled_skips_floating);
	RUN_TEST(nexttiled_skips_hidden);
	RUN_TEST(nexttiled_skips_invisible);
	RUN_TEST(nexttiled_all_floating_returns_null);
	RUN_TEST(nexttiled_starts_from_given_client);
}

SUITE(suite_attachstack)
{
	RUN_TEST(attachstack_onto_empty);
	RUN_TEST(attachstack_prepends);
}

SUITE(suite_detachstack)
{
	RUN_TEST(detachstack_head_of_one);
	RUN_TEST(detachstack_head_of_many);
	RUN_TEST(detachstack_middle);
	RUN_TEST(detachstack_tail);
	RUN_TEST(detachstack_sel_updated_to_visible);
	RUN_TEST(detachstack_sel_null_when_none_visible);
}

/* =========================================================================
 * main
 * ====================================================================== */

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
	GREATEST_MAIN_BEGIN();
	RUN_SUITE(suite_nexttiled);
	RUN_SUITE(suite_attachstack);
	RUN_SUITE(suite_detachstack);
	GREATEST_MAIN_END();
}
