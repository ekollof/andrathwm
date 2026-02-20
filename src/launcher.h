/* AndrathWM - Application Launcher
 * See LICENSE file for copyright and license details.
 *
 * rofi-style launcher that reads .desktop files and falls back to PATH
 */

#ifndef LAUNCHER_H
#define LAUNCHER_H

#include "drw.h"
#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include <stddef.h>

#define LAUNCHER_ICON_SIZE 20

typedef struct LauncherItem {
	char            *name;       /* Display name */
	char            *exec;       /* Command to execute */
	char            *icon_name;  /* Icon name from .desktop */
	cairo_surface_t *icon;       /* Loaded icon surface */
	int              is_desktop; /* 1 if from .desktop file, 0 if from PATH */
	int              terminal;   /* 1 if Terminal=true in .desktop */
	int launch_count;            /* Number of times launched (from history) */
	struct LauncherItem *next;
} LauncherItem;

typedef struct {
	xcb_connection_t *xc;
	xcb_window_t      win;
	Drw              *drw;
	Clr             **scheme;

	char input[256]; /* User input text */
	int  input_len;  /* Current input length */
	int  cursor_pos; /* Cursor position in input */

	LauncherItem  *items;         /* All available items */
	LauncherItem **filtered;      /* Filtered/matching items */
	int            item_count;    /* Total items */
	int            visible_count; /* Filtered items */
	int            selected;      /* Currently selected index */

	int          x, y;
	unsigned int w, h;

	int          visible;
	int          scroll_offset;
	char         history_path[512]; /* Path to launch-history file */
	unsigned int max_item_width; /* Widest item across all items, set once */
	const char  *terminal; /* Terminal emulator binary for Terminal=true */
} Launcher;

Launcher *launcher_create(xcb_connection_t *xc, xcb_window_t root,
    Clr **scheme, const char **fonts, size_t fontcount, const char *term);
void      launcher_free(Launcher *launcher);
void      launcher_show(Launcher *launcher, int x, int y);
void      launcher_hide(Launcher *launcher);
int       launcher_handle_event(Launcher *launcher, xcb_generic_event_t *ev);
void      launcher_launch_selected(Launcher *launcher);

#endif /* LAUNCHER_H */
