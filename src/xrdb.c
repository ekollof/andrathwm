/* AndrathWM - Xresources integration
 * See LICENSE file for copyright and license details. */

#include "xrdb.h"
#include "awm.h"
#include "client.h"
#include "monitor.h"
#include "spawn.h"
#include "systray.h"
#include "config.h"

void
loadxrdb(void)
{
	Display    *display;
	char       *resm;
	XrmDatabase xrdb;
	char       *type;
	XrmValue    value;

	display = XOpenDisplay(NULL);

	if (display != NULL) {
		resm = XResourceManagerString(display);

		if (resm != NULL) {
			xrdb = XrmGetStringDatabase(resm);

			if (xrdb != NULL) {
				XRDB_LOAD_COLOR("color2", normbordercolor);
				XRDB_LOAD_COLOR("color0", normbgcolor);
				XRDB_LOAD_COLOR("color8", normfgcolor);
				XRDB_LOAD_COLOR("color6", selbordercolor);
				XRDB_LOAD_COLOR("color1", selbgcolor);
				XRDB_LOAD_COLOR("color7", selfgcolor);
				XRDB_LOAD_COLOR("color0", termcol0);
				XRDB_LOAD_COLOR("color1", termcol1);
				XRDB_LOAD_COLOR("color2", termcol2);
				XRDB_LOAD_COLOR("color3", termcol3);
				XRDB_LOAD_COLOR("color4", termcol4);
				XRDB_LOAD_COLOR("color5", termcol5);
				XRDB_LOAD_COLOR("color6", termcol6);
				XRDB_LOAD_COLOR("color7", termcol7);
				XRDB_LOAD_COLOR("color8", termcol8);
				XRDB_LOAD_COLOR("color9", termcol9);
				XRDB_LOAD_COLOR("color10", termcol10);
				XRDB_LOAD_COLOR("color11", termcol11);
				XRDB_LOAD_COLOR("color12", termcol12);
				XRDB_LOAD_COLOR("color13", termcol13);
				XRDB_LOAD_COLOR("color14", termcol14);
				XRDB_LOAD_COLOR("color15", termcol15);
			}
		}
	}

	XCloseDisplay(display);
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
