/* See LICENSE file for copyright and license details.
 *
 * Reusable menu system for awm
 * Provides X11-based popup menus with keyboard and mouse support
 */

#ifndef MENU_H
#define MENU_H

#include "drw.h"
#include <X11/Xlib.h>

/* Menu item structure */
typedef struct MenuItem {
	int              id;
	char            *label;
	int              enabled;
	int              is_separator;
	struct MenuItem *submenu;
	struct MenuItem *next;
} MenuItem;

/* Menu callback - called when item is selected */
typedef void (*MenuCallback)(int item_id, void *data);

/* Menu structure */
typedef struct Menu {
	Display *dpy;
	Window   win;
	Drw     *drw;
	Clr    **scheme;

	MenuItem *items;
	int       item_count;
	int owns_items; /* Whether this menu should free items on destruction */
	int selected;
	int x, y;
	unsigned int w, h;

	MenuCallback callback;
	void        *callback_data;

	int visible;
	int ignore_next_release; /* Ignore first ButtonRelease after showing */

	struct Menu *parent;                     /* Parent menu (for submenus) */
	struct Menu *active_submenu;             /* Currently open submenu */
	int          mon_x, mon_y, mon_w, mon_h; /* Monitor bounds */
} Menu;

/* Menu API */
Menu *menu_create(Display *dpy, Window root, Drw *drw, Clr **scheme);
void  menu_free(Menu *menu);
void  menu_set_items(Menu *menu, MenuItem *items);
void  menu_show(Menu *menu, int x, int y, MenuCallback callback, void *data,
     Time event_time);
void  menu_hide(Menu *menu);
int   menu_handle_event(Menu *menu, XEvent *ev);

/* Menu item helpers */
MenuItem *menu_item_create(int id, const char *label, int enabled);
MenuItem *menu_separator_create(void);
void      menu_item_free(MenuItem *item);
void      menu_items_free(MenuItem *head);
int       menu_items_count(MenuItem *head);

#endif /* MENU_H */
