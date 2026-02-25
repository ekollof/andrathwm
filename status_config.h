/* AndrathWM - embedded status configuration
 * See LICENSE file for copyright and license details. */

#ifndef STATUS_CONFIG_H
#define STATUS_CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "status.h"
#include "status_components.h"
#include "status_util.h"

/* bh is defined in awm.c; forward-declare so the s2d helpers can use the
 * real bar height instead of a hardcoded constant. */
extern int bh;

struct status_arg {
	const char *(*func)(const char *);
	const char  *fmt;
	const char  *args;
	unsigned int interval; /* interval in seconds to call this function */
	int          prime;    /* 1 = call once at startup to seed state */
};

/* interval between updates (in ms) */
static const unsigned int status_interval_ms = 1000;

/* text to show if no value can be retrieved */
static const char status_unknown_str[] = "n/a";

/* maximum output string length */
#define STATUS_MAXLEN 2048

/* -------------------------------------------------------------------------
 * Bar graph helpers — emit status2d escape sequences that drawbar() renders.
 *
 * Each helper produces a status2d string of the form:
 *
 *   ^cCOLOR^^r1,PAD,FILLW,BH-2PAD^^fGRAPH_W^^d^LABEL
 *
 * drawbar() pre-clears the entire status region with SchemeNorm bg before
 * calling drw_draw_statusd(), so no explicit bg rect is needed here.
 *
 * Layout per widget:
 *   [  S2D_GRAPH_W px fill bar  ][ label text ]
 *
 *   ^r^ draws the fill rect at cursor+1, ry=PAD, height=bh-2*PAD.
 *   ^f S2D_GRAPH_W^ advances the cursor past the bar.
 *   Label text is drawn at the new cursor position and advances naturally.
 * ---------------------------------------------------------------------- */

/* Bar graph geometry (pixels).
 *
 * Layout per widget:
 *   [  GRAPH_W px bar  ][ label text ]
 *
 * ^r^ draws the bar rects at the current cursor (cx), then ^f GRAPH_W^
 * advances the cursor past the bar.  The label text is then drawn at the
 * new cursor position and advances it naturally.  No trailing ^f^ needed.
 *
 * ^r^ does NOT advance the cursor itself — ^f^ is the only way to move it.
 *
 * Background rect spans full bh (ry=0).  Fill rect has a 2px vertical inset.
 */
#define S2D_GRAPH_W 40 /* width of the bar-graph portion in pixels */
#define S2D_BAR_PAD 2  /* vertical inset of fill bar from bar edge */

static char s2d_buf[STATUS_MAXLEN];

/* colour_for_perc — pick a hex colour string based on percentage and
 * direction.  hi_bad=1: high is bad (CPU/RAM); hi_bad=0: low is bad (bat).
 * warn/crit are the thresholds. */
static const char *
colour_for_perc(int perc, int warn, int crit, int hi_bad)
{
	if (hi_bad) {
		if (perc >= crit)
			return "DA382E"; /* red   */
		if (perc >= warn)
			return "F0A818"; /* amber */
	} else {
		if (perc <= crit)
			return "DA382E"; /* red   */
		if (perc <= warn)
			return "F0A818"; /* amber */
	}
	return "42BA68"; /* green */
}

static const char *
s2d_cpu(const char *unused)
{
	const char *raw;
	int         perc;
	int         fillw;
	const char *col;

	raw = cpu_perc(unused);
	if (!raw)
		return NULL;
	perc  = atoi(raw);
	fillw = (perc * (S2D_GRAPH_W - 2)) / 100;
	col   = colour_for_perc(perc, 70, 90, 1);
	/* bg is pre-cleared by drawbar(); just draw the fill rect, advance, label
	 */
	snprintf(s2d_buf, sizeof(s2d_buf), "^c%s^^r1,%d,%d,%d^^f%d^^d^CPU ", col,
	    S2D_BAR_PAD, fillw, bh - 2 * S2D_BAR_PAD, S2D_GRAPH_W);
	return s2d_buf;
}

static const char *
s2d_ram(const char *unused)
{
	static char rbuf[STATUS_MAXLEN];
	uintmax_t   total = 0, free_mem = 0, buffers = 0, cached = 0;
	uintmax_t   used;
	int         perc;
	int         fillw;
	const char *col;
	FILE       *fp;

	fp = fopen("/proc/meminfo", "r");
	if (!fp)
		return NULL;
	if (fscanf(fp,
	        "MemTotal: %ju kB\n"
	        "MemFree: %ju kB\n"
	        "MemAvailable: %ju kB\n"
	        "Buffers: %ju kB\n"
	        "Cached: %ju kB\n",
	        &total, &free_mem, &(uintmax_t) { 0 }, &buffers, &cached) != 5) {
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	if (total == 0)
		return NULL;
	used  = total - free_mem - buffers - cached;
	perc  = (int) (used * 100 / total);
	fillw = (perc * (S2D_GRAPH_W - 2)) / 100;
	col   = colour_for_perc(perc, 75, 90, 1);
	snprintf(rbuf, sizeof(rbuf), "^c%s^^r1,%d,%d,%d^^f%d^^d^RAM ", col,
	    S2D_BAR_PAD, fillw, bh - 2 * S2D_BAR_PAD, S2D_GRAPH_W);
	return rbuf;
}

static const char *
s2d_bat(const char *bat)
{
	static char bbuf[STATUS_MAXLEN];
	const char *perc_str;
	const char *state_str;
	int         perc;
	int         fillw;
	const char *col;
	const char *label;

	perc_str = battery_perc(bat);
	if (!perc_str)
		return NULL;
	perc      = atoi(perc_str);
	state_str = battery_state(bat);
	fillw     = (perc * (S2D_GRAPH_W - 2)) / 100;
	col       = colour_for_perc(perc, 20, 10, 0);

	if (!state_str || state_str[0] == '+')
		label = "CHG";
	else if (state_str[0] == 'o')
		label = "FUL";
	else
		label = "BAT";

	snprintf(bbuf, sizeof(bbuf), "^c%s^^r1,%d,%d,%d^^f%d^^d^%s ", col,
	    S2D_BAR_PAD, fillw, bh - 2 * S2D_BAR_PAD, S2D_GRAPH_W, label);
	return bbuf;
}

static const struct status_arg status_args[] = {
	/* function   format  argument  interval  prime */
	{ s2d_cpu, "%s", NULL, 2, 1 },
	{ s2d_ram, "%s", NULL, 10, 0 },
	{ s2d_bat, "%s", "BAT0", 30, 0 },
	{ datetime, "%s", " %a %d %b  %H:%M:%S ", 1, 0 },
};

#define STATUS_ARGS_LEN (sizeof(status_args) / sizeof(status_args[0]))

#endif /* STATUS_CONFIG_H */
