/* AndrathWM - Application Launcher
 * See LICENSE file for copyright and license details.
 *
 * rofi-style launcher that reads .desktop files and falls back to PATH
 * GTK backend: GtkWindow + GtkSearchEntry + GtkListBox
 */

#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <gtk/gtk.h>
#include <xcb/xcb.h>
#include <cairo/cairo.h>
#include <stddef.h>
#include "drw.h"

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
	xcb_connection_t *xc;       /* kept only so fork() can close the fd */
	const char       *terminal; /* Terminal emulator binary */

	LauncherItem *items;      /* All available items (linked list) */
	int           item_count; /* Total items */

	int  visible;
	char history_path[512]; /* Path to launch-history file */

	/* GTK widgets */
	GtkWidget *window;  /* GtkWindow (POPUP_MENU hint, undecorated) */
	GtkWidget *search;  /* GtkSearchEntry */
	GtkWidget *listbox; /* GtkListBox */
} Launcher;

Launcher *launcher_create(xcb_connection_t *xc, xcb_window_t root,
    Clr **scheme, const char **fonts, size_t fontcount, const char *term);
void      launcher_free(Launcher *launcher);
void      launcher_show(Launcher *launcher, int x, int y);
void      launcher_hide(Launcher *launcher);
int       launcher_handle_event(Launcher *launcher, xcb_generic_event_t *ev);
void      launcher_launch_selected(Launcher *launcher);

#endif /* LAUNCHER_H */
