/* See LICENSE file for copyright and license details.
 *
 * Reusable menu system for awm-ui
 * Provides GTK-based popup menus with keyboard and mouse support.
 */

#ifndef MENU_H
#define MENU_H

#include <gtk/gtk.h>
#include <stdint.h>

/* xcb_timestamp_t is just uint32_t; define a compat alias so menu_show's
 * event_time parameter keeps a meaningful type without pulling in xcb headers
 * in awm-ui (which has no XCB connection).  If xcb/xcb.h is already included
 * (e.g. in sni.c) this typedef is skipped to avoid a redefinition warning. */
#ifndef __XCB_H__
typedef uint32_t xcb_timestamp_t;
#endif

/* Toggle types for DBusMenu checkbox/radio items */
typedef enum {
	MENU_TOGGLE_NONE      = 0,
	MENU_TOGGLE_CHECKMARK = 1,
	MENU_TOGGLE_RADIO     = 2
} MenuToggleType;

/* Menu item structure */
typedef struct MenuItem {
	int              id;
	char            *label;
	int              enabled;
	int              is_separator;
	MenuToggleType   toggle_type;  /* checkmark, radio, or none */
	int              toggle_state; /* 1 = checked/on, 0 = unchecked/off */
	struct MenuItem *submenu;
	struct MenuItem *next;
} MenuItem;

/* Menu callback - called when item is selected */
typedef void (*MenuCallback)(int item_id, void *data);

/* Dismiss callback - called when the menu is dismissed (with or without
 * item activation).  Optional; set to NULL if not needed. */
typedef void (*MenuDismissCallback)(void *data);

/* Menu structure */
typedef struct Menu {
	GtkWidget *gtk_menu;           /* GTK menu widget */
	gulong     deactivate_handler; /* signal handler ID for "deactivate" */

	MenuItem *items;
	int owns_items; /* Whether this menu should free items on destruction */

	MenuCallback        callback;
	void               *callback_data;
	MenuDismissCallback dismiss_callback;
	void               *dismiss_data;

	int        visible;
	GdkWindow *timing_win; /* realized window for gdk_x11_get_server_time */
	GdkWindow *grab_win;   /* 1x1 OR always-mapped window for seat grabs */
} Menu;

/* Menu API. */
Menu *menu_create(void);
void  menu_free(Menu *menu);
void  menu_set_items(Menu *menu, MenuItem *items);
void  menu_show(Menu *menu, int x, int y, MenuCallback callback, void *data,
     xcb_timestamp_t event_time);
void  menu_hide(Menu *menu);

/* Menu item helpers */
MenuItem *menu_item_create(int id, const char *label, int enabled);
MenuItem *menu_separator_create(void);
void      menu_item_free(MenuItem *item);
void      menu_items_free(MenuItem *head);
int       menu_items_count(MenuItem *head);

#endif /* MENU_H */
