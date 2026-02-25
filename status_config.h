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
 * Widget geometry
 * ---------------------------------------------------------------------- */

/* Width of a single per-core vbar column (pixels). */
#define S2D_VBAR_W 2
/* Gap between adjacent vbar columns (pixels). */
#define S2D_VBAR_GAP 1
/* Height of each vbar, relative to bh (bh - S2D_VBAR_PAD*2). */
#define S2D_VBAR_PAD 2

/* Total width (body only, no nub) of a horizontal bordered bar (pixels). */
#define S2D_HBAR_W 40
/* Height of the horizontal bordered bar inner fill area (pixels). */
#define S2D_HBAR_H 6
/* Border colour for hbar (matches bar norm-bg). */
#define S2D_BORDER_COL "444444"

/* shared scratch buffer — written before return, never nested */
static char s2d_buf[STATUS_MAXLEN];

/* -------------------------------------------------------------------------
 * perc_color — smooth gradient between green and red via amber.
 *
 * Writes a "#RRGGBB\0" string (8 bytes) into out[].
 * hi_bad=1: high percentage is bad (CPU, RAM).
 * hi_bad=0: low percentage is bad (battery).
 * ---------------------------------------------------------------------- */
static void
perc_color(char *out, int percent, int hi_bad)
{
	int p = hi_bad ? percent : (100 - percent);
	/* p=0   → green  #00F000
	 * p=50  → amber  #F0F000
	 * p=100 → red    #F00000 */
	int r = (p >= 50) ? 0xF0 : (p * 0xF0 * 2 / 100);
	int g = (p <= 50) ? 0xF0 : ((100 - p) * 0xF0 * 2 / 100);

	snprintf(out, 8, "%02X%02X00", r, g);
}

/* -------------------------------------------------------------------------
 * vbar_s2d — emit status2d escapes for one vertical bar column.
 *
 * Draws a background rect (full height) and a filled fg rect from the
 * bottom up proportional to percent.  Both are vertically centred within bh.
 * Does NOT emit ^f^ — caller must advance the cursor.
 *
 * Returns number of bytes written (not including NUL), or 0 on error.
 * ---------------------------------------------------------------------- */
static int
vbar_s2d(char *buf, size_t sz, int percent, int w, int h, const char *bg_col,
    const char *fg_col)
{
	int bar_h  = (percent * h) / 100;
	int y_top  = (bh - h) / 2; /* centre bar vertically within bh */
	int y_fill = y_top + (h - bar_h);

	if (bar_h < 0)
		bar_h = 0;
	if (bar_h > h)
		bar_h = h;

	/* background rect then filled rect (drawn on top via same coords) */
	return snprintf(buf, sz,
	    "^c%s^^r0,%d,%d,%d^"  /* bg full rect  */
	    "^c%s^^r0,%d,%d,%d^", /* fg fill rect  */
	    bg_col, y_top, w, h, fg_col, y_fill, w, bar_h);
}

/* -------------------------------------------------------------------------
 * hbar_bordered_s2d — emit status2d escapes for a bordered horizontal bar.
 *
 * Layout (total cursor advance = w + 3 pixels for nub):
 *   [border rect w×(h+2)][inner fill][nub 3px]
 *
 * The border is drawn first (full rect), then the fill is drawn inside it,
 * then a small 3px nub at the right end (battery positive terminal).
 * After this call the caller should emit ^f(w+3)^ to advance the cursor.
 *
 * Returns number of bytes written, or 0 on error.
 * ---------------------------------------------------------------------- */
static int
hbar_bordered_s2d(char *buf, size_t sz, int percent, int w, int h,
    const char *fg_col, const char *border_col)
{
	int inner_w = w - 2; /* 1px border each side */
	int fill_w  = (percent * inner_w) / 100;
	int empty_w = inner_w - fill_w;
	int y_border =
	    (bh - h - 2) / 2; /* centre vertically, extra 2 for border */
	int y_inner = y_border + 1;
	/* nub: 3px wide, 4px tall, centred vertically */
	int nub_h   = (h > 4) ? 4 : h;
	int y_nub   = y_inner + (h - nub_h) / 2;
	int written = 0;
	int ret;

	if (fill_w < 0)
		fill_w = 0;
	if (fill_w > inner_w)
		fill_w = inner_w;
	if (empty_w < 0)
		empty_w = 0;

	/* border outline */
	ret = snprintf(buf + written, sz - (size_t) written, "^c%s^^r0,%d,%d,%d^",
	    border_col, y_border, w, h + 2);
	if (ret < 0 || (size_t) ret >= sz - (size_t) written)
		return written;
	written += ret;

	/* inner fill */
	if (fill_w > 0) {
		ret = snprintf(buf + written, sz - (size_t) written,
		    "^c%s^^r1,%d,%d,%d^", fg_col, y_inner, fill_w, h);
		if (ret < 0 || (size_t) ret >= sz - (size_t) written)
			return written;
		written += ret;
	}

	/* inner empty (bg) */
	if (empty_w > 0) {
		ret = snprintf(buf + written, sz - (size_t) written,
		    "^c222222^^r%d,%d,%d,%d^", 1 + fill_w, y_inner, empty_w, h);
		if (ret < 0 || (size_t) ret >= sz - (size_t) written)
			return written;
		written += ret;
	}

	/* nub at right side of bar body */
	ret = snprintf(buf + written, sz - (size_t) written, "^c%s^^r%d,%d,3,%d^",
	    border_col, w, y_nub, nub_h);
	if (ret < 0 || (size_t) ret >= sz - (size_t) written)
		return written;
	written += ret;

	return written;
}

/* -------------------------------------------------------------------------
 * s2d_cpu — per-core vertical bars, one column per logical CPU.
 *
 * Layout: [core0][core1]...[coreN] CPU
 *   Each core = S2D_VBAR_W px column + S2D_VBAR_GAP px gap (except last).
 * ---------------------------------------------------------------------- */
static const char *
s2d_cpu(const char *unused)
{
	int  cores[64];
	int  n, i;
	int  h   = bh - S2D_VBAR_PAD * 2;
	int  pos = 0;
	char fg_col[8];
	int  ret;

	(void) unused;

	if (h < 1)
		h = 1;

	n = cpu_percpu(cores, 64);
	if (n <= 0) {
		/* Fallback: single aggregate bar on first call or non-Linux. */
		const char *raw  = cpu_perc(NULL);
		int         perc = raw ? atoi(raw) : 0;
		perc_color(fg_col, perc, 1);
		ret = vbar_s2d(
		    s2d_buf, sizeof(s2d_buf), perc, S2D_VBAR_W, h, "333333", fg_col);
		if (ret > 0) {
			snprintf(s2d_buf + ret, sizeof(s2d_buf) - (size_t) ret,
			    "^f%d^^d^CPU ", S2D_VBAR_W);
		}
		return s2d_buf;
	}

	for (i = 0; i < n; i++) {
		size_t rem = sizeof(s2d_buf) - (size_t) pos;
		int    step;

		perc_color(fg_col, cores[i], 1);
		step = vbar_s2d(
		    s2d_buf + pos, rem, cores[i], S2D_VBAR_W, h, "333333", fg_col);
		if (step <= 0 || (size_t) step >= rem)
			break;
		pos += step;

		/* advance cursor past this column */
		rem = sizeof(s2d_buf) - (size_t) pos;
		ret = snprintf(s2d_buf + pos, rem, "^f%d^",
		    S2D_VBAR_W + (i < n - 1 ? S2D_VBAR_GAP : 0));
		if (ret <= 0 || (size_t) ret >= rem)
			break;
		pos += ret;
	}

	/* label */
	snprintf(s2d_buf + pos, sizeof(s2d_buf) - (size_t) pos, "^d^CPU ");
	return s2d_buf;
}

/* -------------------------------------------------------------------------
 * s2d_ram — horizontal bordered bar for RAM usage.
 * ---------------------------------------------------------------------- */
static const char *
s2d_ram(const char *unused)
{
	static char rbuf[STATUS_MAXLEN];
	uintmax_t   total = 0, free_mem = 0, buffers = 0, cached = 0, used;
	int         perc, pos;
	char        fg_col[8];
	FILE       *fp;

	(void) unused;

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

	used = total - free_mem - buffers - cached;
	perc = (int) (used * 100 / total);
	perc_color(fg_col, perc, 1);

	pos = hbar_bordered_s2d(rbuf, sizeof(rbuf), perc, S2D_HBAR_W, S2D_HBAR_H,
	    fg_col, S2D_BORDER_COL);
	snprintf(rbuf + pos, sizeof(rbuf) - (size_t) pos, "^f%d^^d^RAM ",
	    S2D_HBAR_W + 3);
	return rbuf;
}

/* -------------------------------------------------------------------------
 * s2d_bat — horizontal bordered bar for battery with charging state colour.
 * ---------------------------------------------------------------------- */
static const char *
s2d_bat(const char *bat)
{
	static char bbuf[STATUS_MAXLEN];
	const char *perc_str;
	const char *state_str;
	int         perc, pos;
	char        fg_col[8];
	const char *label;

	perc_str = battery_perc(bat);
	if (!perc_str)
		return NULL;
	perc      = atoi(perc_str);
	state_str = battery_state(bat);

	if (!state_str || state_str[0] == '+') {
		/* Charging — use a fixed cyan/teal to indicate charging. */
		label = "CHG";
		snprintf(fg_col, sizeof(fg_col), "00CCCC");
	} else if (state_str[0] == 'o') {
		label = "FUL";
		snprintf(fg_col, sizeof(fg_col), "42BA68");
	} else {
		label = "BAT";
		perc_color(fg_col, perc, 0); /* low% is bad for battery */
	}

	pos = hbar_bordered_s2d(bbuf, sizeof(bbuf), perc, S2D_HBAR_W, S2D_HBAR_H,
	    fg_col, S2D_BORDER_COL);
	snprintf(bbuf + pos, sizeof(bbuf) - (size_t) pos, "^f%d^^d^%s ",
	    S2D_HBAR_W + 3, label);
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
