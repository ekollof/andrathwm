/* AndrathWM - Application Launcher
 * See LICENSE file for copyright and license details.
 *
 * rofi-style launcher that reads .desktop files and falls back to PATH.
 * GTK backend: GtkWindow + GtkSearchEntry + GtkListBox.
 *
 * In the IPC architecture this module runs inside awm-ui.  It sends the
 * selected command back to awm via a Unix SOCK_SEQPACKET socket (ui_fd)
 * using UI_MSG_LAUNCHER_EXEC; awm does the actual fork/exec.
 */

#ifndef LAUNCHER_H
#define LAUNCHER_H

#include <gtk/gtk.h>
#include <cairo/cairo.h>
#include <stddef.h>

#include "ui_proto.h"

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
	int         ui_fd;    /* socket fd back to awm — used to send EXEC */
	const char *terminal; /* Terminal emulator binary (from config) */

	LauncherItem *items;      /* All available items (linked list) */
	int           item_count; /* Total items */

	int  visible;
	char history_path[512]; /* Path to launch-history file */

	/* GTK widgets */
	GtkWidget *window;  /* GtkWindow (POPUP_MENU hint, undecorated) */
	GtkWidget *search;  /* GtkSearchEntry */
	GtkWidget *listbox; /* GtkListBox */
} Launcher;

/* ui_fd  — socket fd to awm (for UI_MSG_LAUNCHER_EXEC replies)
 * term   — terminal emulator binary (may be NULL to use "st") */
Launcher *launcher_create(int ui_fd, const char *term);
void      launcher_free(Launcher *launcher);
void      launcher_show(Launcher *launcher, int x, int y);
void      launcher_hide(Launcher *launcher);
void      launcher_update_theme(Launcher *launcher, const UiThemePayload *t);

/* Returns the underlying GdkWindow* (realized), or NULL if not yet realized.
 * Used by awm_ui.c to obtain a timing window for gdk_x11_get_server_time. */
static inline GdkWindow *
launcher_get_gdk_window(Launcher *launcher)
{
	if (!launcher || !launcher->window)
		return NULL;
	gtk_widget_realize(launcher->window);
	return gtk_widget_get_window(launcher->window);
}

#endif /* LAUNCHER_H */
