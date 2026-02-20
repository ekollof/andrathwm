/* AndrathWM - Xresources integration
 * See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "xrdb.h"
#include "awm.h"
#include "client.h"
#include "monitor.h"
#include "spawn.h"
#include "systray.h"
#include "config.h"

/*
 * Scan the RESOURCE_MANAGER string for a colour resource by name.
 *
 * RESOURCE_MANAGER is a newline-separated "key:\tvalue" list.  Keys are
 * prefixed with component qualifiers like "*.color0:" or "*color0:".
 * We match any line whose key component ends with the bare name, e.g.
 * "color0" matches "*.color0:" and "*color0:".
 *
 * Only copies value into dest if it is a valid #RRGGBB hex string.
 * dest must be at least 8 bytes.
 */
static void
xrdb_lookup(const char *resm, const char *key, char *dest)
{
	const char *p;
	size_t      klen;
	int         i;

	if (!resm || !key || !dest)
		return;

	klen = strlen(key);
	p    = resm;

	while (*p) {
		const char *eol = p;
		const char *col;

		/* find end of line */
		while (*eol && *eol != '\n')
			eol++;

		/* find ':' on this line */
		col = p;
		while (col < eol && *col != ':')
			col++;

		if (col < eol) {
			/* col points at ':'; check that it is followed by '\t' */
			if (col[1] == '\t') {
				/* check that the key ends exactly at col */
				if ((size_t) (col - p) >= klen) {
					const char *kstart = col - klen;
					/* char before kstart must be start, '*', or '.' */
					if (kstart == p || kstart[-1] == '*' ||
					    kstart[-1] == '.') {
						if (strncmp(kstart, key, klen) == 0) {
							const char *v = col + 2; /* skip ":\t" */
							/* validate #RRGGBB (need at least 7 chars before
							 * eol) */
							if (v + 7 <= eol && v[0] == '#') {
								for (i = 1; i <= 6; i++) {
									char c = v[i];
									if (c < '0')
										goto next;
									if (c > '9' && c < 'A')
										goto next;
									if (c > 'F' && c < 'a')
										goto next;
									if (c > 'f')
										goto next;
								}
								strncpy(dest, v, 7);
								dest[7] = '\0';
								return;
							}
						}
					}
				}
			}
		}
	next:
		p = (*eol == '\n') ? eol + 1 : eol;
	}
}

void
loadxrdb(void)
{
	xcb_connection_t         *xc;
	xcb_screen_t             *scr;
	xcb_intern_atom_cookie_t  ack;
	xcb_intern_atom_reply_t  *ar;
	xcb_atom_t                res_mgr;
	xcb_get_property_cookie_t pck;
	xcb_get_property_reply_t *pr;
	char                     *resm;
	int                       scrnum = 0;

	xc = xcb_connect(NULL, &scrnum);
	if (xcb_connection_has_error(xc)) {
		xcb_disconnect(xc);
		return;
	}

	/* Get root window for the default screen */
	{
		xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(xc));
		for (int i = 0; i < scrnum; i++)
			xcb_screen_next(&it);
		scr = it.data;
	}

	/* Intern RESOURCE_MANAGER */
	ack =
	    xcb_intern_atom(xc, 1, strlen("RESOURCE_MANAGER"), "RESOURCE_MANAGER");
	ar = xcb_intern_atom_reply(xc, ack, NULL);
	if (!ar || ar->atom == XCB_ATOM_NONE) {
		free(ar);
		xcb_disconnect(xc);
		return;
	}
	res_mgr = ar->atom;
	free(ar);

	/* Fetch RESOURCE_MANAGER from root window (STRING, format 8).
	 * 65536 longs = 256 KiB — far larger than any real xrdb database. */
	pck =
	    xcb_get_property(xc, 0, scr->root, res_mgr, XCB_ATOM_STRING, 0, 65536);
	pr = xcb_get_property_reply(xc, pck, NULL);

	if (!pr || pr->type == XCB_ATOM_NONE || pr->format != 8 ||
	    pr->value_len == 0) {
		free(pr);
		xcb_disconnect(xc);
		return;
	}

	/* Property is not NUL-terminated — make a copy that is */
	resm = malloc(pr->value_len + 1);
	if (!resm) {
		free(pr);
		xcb_disconnect(xc);
		return;
	}
	memcpy(resm, xcb_get_property_value(pr), pr->value_len);
	resm[pr->value_len] = '\0';
	free(pr);
	xcb_disconnect(xc);

	xrdb_lookup(resm, "color2", normbordercolor);
	xrdb_lookup(resm, "color0", normbgcolor);
	xrdb_lookup(resm, "color8", normfgcolor);
	xrdb_lookup(resm, "color6", selbordercolor);
	xrdb_lookup(resm, "color1", selbgcolor);
	xrdb_lookup(resm, "color7", selfgcolor);
	xrdb_lookup(resm, "color0", termcol0);
	xrdb_lookup(resm, "color1", termcol1);
	xrdb_lookup(resm, "color2", termcol2);
	xrdb_lookup(resm, "color3", termcol3);
	xrdb_lookup(resm, "color4", termcol4);
	xrdb_lookup(resm, "color5", termcol5);
	xrdb_lookup(resm, "color6", termcol6);
	xrdb_lookup(resm, "color7", termcol7);
	xrdb_lookup(resm, "color8", termcol8);
	xrdb_lookup(resm, "color9", termcol9);
	xrdb_lookup(resm, "color10", termcol10);
	xrdb_lookup(resm, "color11", termcol11);
	xrdb_lookup(resm, "color12", termcol12);
	xrdb_lookup(resm, "color13", termcol13);
	xrdb_lookup(resm, "color14", termcol14);
	xrdb_lookup(resm, "color15", termcol15);

	free(resm);
}

void
xrdb(const Arg *arg)
{
	loadxrdb();
	int i;
	for (i = 0; i < LENGTH(colors); i++)
		scheme[i] = drw_scm_create(drw, colors[i], 3);
	updatesystrayiconcolors();
	focus(NULL);
	arrange(NULL);
}
