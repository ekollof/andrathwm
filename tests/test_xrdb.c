/* See LICENSE file for copyright and license details. */
/*
 * Tests for static functions in src/xrdb.c:
 *   xrdb_lookup()        — scan RESOURCE_MANAGER for a #RRGGBB colour
 *   xrdb_lookup_double() — scan RESOURCE_MANAGER for a numeric value
 *
 * Strategy: include the .c file directly to access the static functions.
 * Provide stub globals/functions to satisfy every external symbol that
 * loadxrdb() / xrdb() reference (they are never called from tests).
 */

#include <stdlib.h>
#include <string.h>

#include "greatest.h"

/* Pull in all type definitions first (awm.h + wmstate.h) */
#include "../src/awm.h"
#include "../src/wmstate.h"
#include "../src/drw.h"
#include "../src/systray.h"
#include "../src/monitor.h"
#include "../src/client.h"

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

/* AWMState single global instance */
AWMState g_awm;

/* -------------------------------------------------------------------------
 * Stub functions for symbols called by xrdb() / loadxrdb().
 * Signatures must match the declarations in the included headers.
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
updatesystrayiconcolors(void)
{
}
void
focus(Client *c)
{
	(void) c;
}
void
arrange(Monitor *m)
{
	(void) m;
}
void
ui_send_theme(void)
{
}

/* wmstate_update() is called nowhere in the test path but wmstate.h
 * declares it; wmstate.c would normally provide it. */
void
wmstate_update(void)
{
}

/* -------------------------------------------------------------------------
 * Now include xrdb.c — config.h must be last include inside it, which is
 * satisfied because xrdb.c itself ends with #include "config.h".
 * ---------------------------------------------------------------------- */
#define AWM_CONFIG_IMPL
#include "../src/xrdb.c"

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Build a RESOURCE_MANAGER-format string from an array of lines.
 * Each element of lines[] becomes one "key:\tvalue\n" line.
 * Returns a malloc'd NUL-terminated string; caller must free(). */
static char *
make_resm(const char *lines[], size_t n)
{
	size_t total = 0;
	size_t i;
	char  *buf, *p;

	for (i = 0; i < n; i++)
		total += strlen(lines[i]) + 1; /* +1 for '\n' */
	buf = malloc(total + 1);
	if (!buf)
		return NULL;
	p = buf;
	for (i = 0; i < n; i++) {
		size_t len = strlen(lines[i]);
		memcpy(p, lines[i], len);
		p += len;
		*p++ = '\n';
	}
	*p = '\0';
	return buf;
}

/* -------------------------------------------------------------------------
 * xrdb_lookup — colour tests
 * ---------------------------------------------------------------------- */

TEST
lookup_star_dot_prefix(void)
{
	/* "*.color0:\t#aabbcc" — canonical xrdb format */
	const char *lines[] = { "*.color0:\t#aabbcc" };
	char       *resm    = make_resm(lines, 1);
	char        dest[8] = { 0 };

	xrdb_lookup(resm, "color0", dest);
	ASSERT_STR_EQ("#aabbcc", dest);
	free(resm);
	PASS();
}

TEST
lookup_star_prefix(void)
{
	/* "*color0:\t#112233" — no dot between * and key */
	const char *lines[] = { "*color0:\t#112233" };
	char       *resm    = make_resm(lines, 1);
	char        dest[8] = { 0 };

	xrdb_lookup(resm, "color0", dest);
	ASSERT_STR_EQ("#112233", dest);
	free(resm);
	PASS();
}

TEST
lookup_bare_key(void)
{
	/* "color0:\t#223344" — key at start of line, no qualifier */
	const char *lines[] = { "color0:\t#223344" };
	char       *resm    = make_resm(lines, 1);
	char        dest[8] = { 0 };

	xrdb_lookup(resm, "color0", dest);
	ASSERT_STR_EQ("#223344", dest);
	free(resm);
	PASS();
}

TEST
lookup_key_not_found(void)
{
	const char *lines[] = { "*.color1:\t#aabbcc" };
	char       *resm    = make_resm(lines, 1);
	char        dest[8] = "initial";

	xrdb_lookup(resm, "color0", dest);
	/* dest must be untouched when key is absent */
	ASSERT_STR_EQ("initial", dest);
	free(resm);
	PASS();
}

TEST
lookup_second_line(void)
{
	/* key appears on the second line */
	const char *lines[] = { "*.color1:\t#ffffff", "*.color0:\t#010203" };
	char       *resm    = make_resm(lines, 2);
	char        dest[8] = { 0 };

	xrdb_lookup(resm, "color0", dest);
	ASSERT_STR_EQ("#010203", dest);
	free(resm);
	PASS();
}

TEST
lookup_invalid_hex_char(void)
{
	/* 'G' is not a valid hex digit */
	const char *lines[] = { "*.color0:\t#GGBBCC" };
	char       *resm    = make_resm(lines, 1);
	char        dest[8] = "initial";

	xrdb_lookup(resm, "color0", dest);
	ASSERT_STR_EQ("initial", dest);
	free(resm);
	PASS();
}

TEST
lookup_value_too_short(void)
{
	/* value has only 6 chars including '#' — needs 7 */
	const char *lines[] = { "*.color0:\t#abcde" };
	char       *resm    = make_resm(lines, 1);
	char        dest[8] = "initial";

	xrdb_lookup(resm, "color0", dest);
	ASSERT_STR_EQ("initial", dest);
	free(resm);
	PASS();
}

TEST
lookup_missing_tab(void)
{
	/* ': ' instead of ':\t' — must not match */
	const char *lines[] = { "*.color0: #aabbcc" };
	char       *resm    = make_resm(lines, 1);
	char        dest[8] = "initial";

	xrdb_lookup(resm, "color0", dest);
	ASSERT_STR_EQ("initial", dest);
	free(resm);
	PASS();
}

TEST
lookup_null_resm(void)
{
	char dest[8] = "initial";
	xrdb_lookup(NULL, "color0", dest);
	ASSERT_STR_EQ("initial", dest);
	PASS();
}

TEST
lookup_accepts_uppercase_hex(void)
{
	const char *lines[] = { "*.color0:\t#AABBCC" };
	char       *resm    = make_resm(lines, 1);
	char        dest[8] = { 0 };

	xrdb_lookup(resm, "color0", dest);
	ASSERT_STR_EQ("#AABBCC", dest);
	free(resm);
	PASS();
}

TEST
lookup_accepts_lowercase_hex(void)
{
	const char *lines[] = { "*.color0:\t#aabbcc" };
	char       *resm    = make_resm(lines, 1);
	char        dest[8] = { 0 };

	xrdb_lookup(resm, "color0", dest);
	ASSERT_STR_EQ("#aabbcc", dest);
	free(resm);
	PASS();
}

/* -------------------------------------------------------------------------
 * xrdb_lookup_double — numeric tests
 * ---------------------------------------------------------------------- */

TEST
lookup_double_xft_dpi(void)
{
	const char *lines[] = { "Xft.dpi:\t96" };
	char       *resm    = make_resm(lines, 1);
	double      out     = 0.0;

	ASSERT_EQ(0, xrdb_lookup_double(resm, "Xft.dpi", &out));
	ASSERT_IN_RANGE(96.0, out, 0.001);
	free(resm);
	PASS();
}

TEST
lookup_double_fractional(void)
{
	const char *lines[] = { "Xft.dpi:\t192.5" };
	char       *resm    = make_resm(lines, 1);
	double      out     = 0.0;

	ASSERT_EQ(0, xrdb_lookup_double(resm, "Xft.dpi", &out));
	ASSERT_IN_RANGE(192.5, out, 0.001);
	free(resm);
	PASS();
}

TEST
lookup_double_not_found(void)
{
	const char *lines[] = { "*.color0:\t#aabbcc" };
	char       *resm    = make_resm(lines, 1);
	double      out     = -1.0;

	ASSERT_EQ(-1, xrdb_lookup_double(resm, "Xft.dpi", &out));
	/* out must be untouched */
	ASSERT_IN_RANGE(-1.0, out, 0.001);
	free(resm);
	PASS();
}

TEST
lookup_double_zero_rejected(void)
{
	/* d <= 0.0 is explicitly rejected in xrdb_lookup_double */
	const char *lines[] = { "Xft.dpi:\t0" };
	char       *resm    = make_resm(lines, 1);
	double      out     = -1.0;

	ASSERT_EQ(-1, xrdb_lookup_double(resm, "Xft.dpi", &out));
	free(resm);
	PASS();
}

TEST
lookup_double_negative_rejected(void)
{
	const char *lines[] = { "Xft.dpi:\t-96" };
	char       *resm    = make_resm(lines, 1);
	double      out     = -1.0;

	ASSERT_EQ(-1, xrdb_lookup_double(resm, "Xft.dpi", &out));
	free(resm);
	PASS();
}

TEST
lookup_double_non_numeric(void)
{
	const char *lines[] = { "Xft.dpi:\tabc" };
	char       *resm    = make_resm(lines, 1);
	double      out     = -1.0;

	ASSERT_EQ(-1, xrdb_lookup_double(resm, "Xft.dpi", &out));
	free(resm);
	PASS();
}

TEST
lookup_double_null_resm(void)
{
	double out = -1.0;
	ASSERT_EQ(-1, xrdb_lookup_double(NULL, "Xft.dpi", &out));
	PASS();
}

TEST
lookup_double_null_out(void)
{
	const char *lines[] = { "Xft.dpi:\t96" };
	char       *resm    = make_resm(lines, 1);

	ASSERT_EQ(-1, xrdb_lookup_double(resm, "Xft.dpi", NULL));
	free(resm);
	PASS();
}

/* -------------------------------------------------------------------------
 * Suites
 * ---------------------------------------------------------------------- */

SUITE(suite_xrdb_lookup)
{
	RUN_TEST(lookup_star_dot_prefix);
	RUN_TEST(lookup_star_prefix);
	RUN_TEST(lookup_bare_key);
	RUN_TEST(lookup_key_not_found);
	RUN_TEST(lookup_second_line);
	RUN_TEST(lookup_invalid_hex_char);
	RUN_TEST(lookup_value_too_short);
	RUN_TEST(lookup_missing_tab);
	RUN_TEST(lookup_null_resm);
	RUN_TEST(lookup_accepts_uppercase_hex);
	RUN_TEST(lookup_accepts_lowercase_hex);
}

SUITE(suite_xrdb_lookup_double)
{
	RUN_TEST(lookup_double_xft_dpi);
	RUN_TEST(lookup_double_fractional);
	RUN_TEST(lookup_double_not_found);
	RUN_TEST(lookup_double_zero_rejected);
	RUN_TEST(lookup_double_negative_rejected);
	RUN_TEST(lookup_double_non_numeric);
	RUN_TEST(lookup_double_null_resm);
	RUN_TEST(lookup_double_null_out);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

GREATEST_MAIN_DEFS();

int
main(int argc, char **argv)
{
	GREATEST_MAIN_BEGIN();
	RUN_SUITE(suite_xrdb_lookup);
	RUN_SUITE(suite_xrdb_lookup_double);
	GREATEST_MAIN_END();
}
