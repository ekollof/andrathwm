/* See LICENSE file for copyright and license details. */
/*
 * Tests for applysizehints() in src/client.c.
 *
 * applysizehints enforces ICCCM size constraints:
 *   - minw / minh clamping
 *   - maxw / maxh clamping
 *   - increment stepping (incw / inch)
 *   - aspect-ratio enforcement (mina / maxa)
 *   - base/min distinction (baseismin path)
 *
 * Strategy: resizehints = 1 in config.h, so the hint block always executes.
 * Set c->hintsvalid = 1 to bypass the XCB updatesizehints() call.
 * Set c->isfloating = 1 as belt-and-suspenders; with resizehints = 1 it is
 * redundant but makes intent clear.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "greatest.h"

#include "../src/awm.h"
#include "../src/wmstate.h"
#include "../src/drw.h"
#include "../src/systray.h"
#include "../src/monitor.h"
#include "../src/client.h"
#include "../src/spawn.h"

/* -------------------------------------------------------------------------
 * Globals.
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
 * Log stubs.
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
 * Util stubs.
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
 * Drw stubs.
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
drw_text(Drw *d, int x, int y, unsigned int w, unsigned int h, unsigned int lp,
    const char *text, int invert)
{
	(void) d;
	(void) x;
	(void) y;
	(void) w;
	(void) h;
	(void) lp;
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
 * Monitor stubs.
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
 * EWMH / events stubs.
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
 * Systray stubs.
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
 * Compositor stubs.
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
 * Other stubs.
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

void
ui_send_theme(void)
{
}

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

/* -------------------------------------------------------------------------
 * Include client.c for applysizehints().
 * ---------------------------------------------------------------------- */
#define AWM_CONFIG_IMPL
#include "../src/client.c"

/* -------------------------------------------------------------------------
 * Helper: initialise a floating Client with hintsvalid=1, mounted on a
 * simple 1280x800 monitor.  All hint fields are zero by default.
 * ---------------------------------------------------------------------- */
static void
make_client(Client *c, Monitor *m)
{
	memset(&g_awm, 0, sizeof g_awm);
	memset(m, 0, sizeof *m);
	m->mx = m->wx = 0;
	m->my = m->wy = 0;
	m->mw = m->ww = 1280;
	m->mh = m->wh    = 800;
	m->tagset[0]     = 1;
	m->pertag.curtag = 1;
	m->lt[0]         = &layouts[0];
	m->lt[1]         = &layouts[1 % LENGTH(layouts)];
	g_awm.n_monitors = 1;
	memcpy(&g_awm.monitors[0], m, sizeof *m);
	g_awm.selmon_num = 0;

	memset(c, 0, sizeof *c);
	c->win        = 1;
	c->tags       = 1;
	c->bw         = 0;
	c->isfloating = 1; /* ensures hint block runs even if resizehints=0 */
	c->hintsvalid = 1; /* skip updatesizehints XCB call */
	c->mon        = &g_awm.monitors[0];
}

/* =========================================================================
 * Tests
 * ====================================================================== */

/* No hints set: w and h pass through unchanged (no clamping by bh because
 * we pass values well above bh). */
TEST
sizehints_passthrough(void)
{
	Monitor m;
	Client  c;
	int     x = 100, y = 100, w = 400, h = 300;

	make_client(&c, &m);
	c.x = 0;
	c.y = 0;
	c.w = 0;
	c.h = 0;

	applysizehints(&c, &x, &y, &w, &h, 0);

	ASSERT_EQ(400, w);
	ASSERT_EQ(300, h);
	PASS();
}

/* minw / minh: requested size below minimum is clamped up. */
TEST
sizehints_minw_minh_clamp(void)
{
	Monitor m;
	Client  c;
	int     x = 100, y = 100, w = 50, h = 40;

	make_client(&c, &m);
	c.minw = 100;
	c.minh = 80;
	c.x    = 0;
	c.y    = 0;
	c.w    = 0;
	c.h    = 0;

	applysizehints(&c, &x, &y, &w, &h, 0);

	ASSERT_EQ(100, w);
	ASSERT_EQ(80, h);
	PASS();
}

/* maxw / maxh: requested size above maximum is clamped down. */
TEST
sizehints_maxw_maxh_clamp(void)
{
	Monitor m;
	Client  c;
	int     x = 100, y = 100, w = 600, h = 500;

	make_client(&c, &m);
	c.maxw = 400;
	c.maxh = 300;
	c.x    = 0;
	c.y    = 0;
	c.w    = 0;
	c.h    = 0;

	applysizehints(&c, &x, &y, &w, &h, 0);

	ASSERT_EQ(400, w);
	ASSERT_EQ(300, h);
	PASS();
}

/* Increment stepping: w and h are rounded DOWN to the nearest multiple
 * of incw / inch above basew / baseh. */
TEST
sizehints_increment_steps(void)
{
	Monitor m;
	Client  c;
	/*
	 * basew=0, incw=10, minw=0: w=47 -> 47 % 10 = 7 removed -> w=40.
	 * baseh=0, inch=8,  minh=0: h=35 -> 35 % 8  = 3 removed -> h=32.
	 */
	int w = 47, h = 35, x = 100, y = 100;

	make_client(&c, &m);
	c.incw = 10;
	c.inch = 8;
	c.x    = 0;
	c.y    = 0;
	c.w    = 0;
	c.h    = 0;

	applysizehints(&c, &x, &y, &w, &h, 0);

	ASSERT_EQ(40, w);
	ASSERT_EQ(32, h);
	PASS();
}

/*
 * base / min distinction (baseismin path):
 * When basew == minw && baseh == minh, the base is treated as the minimum
 * and the increment rounding is applied AFTER subtracting base. The final
 * w = stepped_w + basew, clamped to minw.
 *
 * basew=20, minw=20, incw=10:
 *   baseismin=1; base NOT subtracted first.
 *   incw step: w=67, 67 % 10 = 7 -> w=60.
 *   Then base subtracted: 60 - 20 = 40.
 *   Final: MAX(40 + 20, 20) = 60.
 */
TEST
sizehints_baseismin_path(void)
{
	Monitor m;
	Client  c;
	int     w = 67, h = 60, x = 100, y = 100;

	make_client(&c, &m);
	c.basew = 20;
	c.baseh = 20;
	c.minw  = 20;
	c.minh  = 20;
	c.incw  = 10;
	c.inch  = 10;
	c.x     = 0;
	c.y     = 0;
	c.w     = 0;
	c.h     = 0;

	applysizehints(&c, &x, &y, &w, &h, 0);

	ASSERT_EQ(60, w);
	ASSERT_EQ(60, h);
	PASS();
}

/*
 * Aspect ratio: maxa constrains maximum w/h ratio.
 * maxa=1.0, mina=1.0 -> square output.
 * Input w=200, h=100 -> w/h=2.0 > maxa=1.0 so w is reduced: w = h*maxa = 100.
 */
TEST
sizehints_aspect_maxa(void)
{
	Monitor m;
	Client  c;
	int     w = 200, h = 100, x = 100, y = 100;

	make_client(&c, &m);
	c.maxa = 1.0f;
	c.mina = 1.0f;
	c.x    = 0;
	c.y    = 0;
	c.w    = 0;
	c.h    = 0;

	applysizehints(&c, &x, &y, &w, &h, 0);

	ASSERT_EQ(100, w);
	ASSERT_EQ(100, h);
	PASS();
}

/*
 * Aspect ratio: mina constrains maximum h/w ratio (ICCCM min aspect).
 * When h/w > mina, h is reduced to w*mina.
 * mina=0.5, maxa=4.0, w=100, h=100:
 *   maxa check: 4.0 < 100/100=1.0? No.
 *   mina check: 0.5 < 100/100=1.0? Yes -> h = 100 * 0.5 + 0.5 = 50.
 */
TEST
sizehints_aspect_mina(void)
{
	Monitor m;
	Client  c;
	int     w = 100, h = 100, x = 100, y = 100;

	make_client(&c, &m);
	c.mina = 0.5f;
	c.maxa = 4.0f;
	c.x    = 0;
	c.y    = 0;
	c.w    = 0;
	c.h    = 0;

	applysizehints(&c, &x, &y, &w, &h, 0);

	ASSERT_EQ(100, w);
	ASSERT_EQ(50, h);
	PASS();
}

/* Return value: 1 if geometry changed, 0 if unchanged. */
TEST
sizehints_return_changed(void)
{
	Monitor m;
	Client  c;
	int     w = 400, h = 300, x = 10, y = 20;

	make_client(&c, &m);
	c.x = 10;
	c.y = 20;
	c.w = 400;
	c.h = 300;

	/* Geometry unchanged -> return 0 */
	ASSERT_EQ(0, applysizehints(&c, &x, &y, &w, &h, 0));
	PASS();
}

TEST
sizehints_return_unchanged(void)
{
	Monitor m;
	Client  c;
	int     w = 500, h = 400, x = 10, y = 20;

	make_client(&c, &m);
	c.x = 10;
	c.y = 20;
	c.w = 400; /* different */
	c.h = 300;

	/* Geometry changed -> return 1 */
	ASSERT_EQ(1, applysizehints(&c, &x, &y, &w, &h, 0));
	PASS();
}

/* =========================================================================
 * Suite
 * ====================================================================== */

SUITE(suite_applysizehints)
{
	RUN_TEST(sizehints_passthrough);
	RUN_TEST(sizehints_minw_minh_clamp);
	RUN_TEST(sizehints_maxw_maxh_clamp);
	RUN_TEST(sizehints_increment_steps);
	RUN_TEST(sizehints_baseismin_path);
	RUN_TEST(sizehints_aspect_maxa);
	RUN_TEST(sizehints_aspect_mina);
	RUN_TEST(sizehints_return_changed);
	RUN_TEST(sizehints_return_unchanged);
}

/* =========================================================================
 * main
 * ====================================================================== */

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
	GREATEST_MAIN_BEGIN();
	RUN_SUITE(suite_applysizehints);
	GREATEST_MAIN_END();
}
