/* See LICENSE file for copyright and license details.
 *
 * Reusable menu system for awm — GTK backend
 *
 * Public API is identical to the old XCB implementation so all callers
 * (sni.c, awm.c) require zero changes.  The drw/scheme parameters accepted
 * by menu_create() are silently ignored; all rendering is handled by GTK.
 */

#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <xcb/xcb.h>

#include "log.h"
#include "menu.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Data passed to each GtkMenuItem "activate" signal handler */
typedef struct {
	Menu *menu;
	int   item_id;
} MenuItemData;

static void
on_item_activate(GtkMenuItem *widget, gpointer user_data)
{
	MenuItemData *d = (MenuItemData *) user_data;
	(void) widget;

	if (d->menu->callback)
		d->menu->callback(d->item_id, d->menu->callback_data);
}

/* Build a GtkMenu from a linked list of MenuItem.
 * Returns the new GtkWidget* (a GtkMenu), or NULL on empty list. */
static GtkWidget *
build_gtk_menu(Menu *menu, MenuItem *items)
{
	MenuItem  *item;
	GtkWidget *gmenu;
	GSList    *radio_group = NULL;

	if (!items)
		return NULL;

	gmenu = gtk_menu_new();

	for (item = items; item; item = item->next) {
		GtkWidget *gitem = NULL;

		if (item->is_separator) {
			gitem = gtk_separator_menu_item_new();
		} else if (!item->label) {
			continue;
		} else if (item->toggle_type == MENU_TOGGLE_RADIO) {
			gitem =
			    gtk_radio_menu_item_new_with_label(radio_group, item->label);
			radio_group =
			    gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(gitem));
			gtk_check_menu_item_set_active(
			    GTK_CHECK_MENU_ITEM(gitem), item->toggle_state);
		} else if (item->toggle_type == MENU_TOGGLE_CHECKMARK) {
			radio_group = NULL;
			gitem       = gtk_check_menu_item_new_with_label(item->label);
			gtk_check_menu_item_set_active(
			    GTK_CHECK_MENU_ITEM(gitem), item->toggle_state);
		} else {
			radio_group = NULL;
			gitem       = gtk_menu_item_new_with_label(item->label);
		}

		if (!item->enabled)
			gtk_widget_set_sensitive(gitem, FALSE);

		/* Attach submenu recursively */
		if (item->submenu && !item->is_separator) {
			GtkWidget *sub = build_gtk_menu(menu, item->submenu);
			if (sub)
				gtk_menu_item_set_submenu(GTK_MENU_ITEM(gitem), sub);
		}

		/* Connect activate signal for leaf items (no submenu) */
		if (!item->is_separator && !item->submenu && item->enabled) {
			MenuItemData *d = g_new(MenuItemData, 1);
			d->menu         = menu;
			d->item_id      = item->id;
			g_signal_connect_data(gitem, "activate",
			    G_CALLBACK(on_item_activate), d, (GClosureNotify) g_free,
			    (GConnectFlags) 0);
		}

		gtk_menu_shell_append(GTK_MENU_SHELL(gmenu), gitem);
	}

	gtk_widget_show_all(gmenu);
	return gmenu;
}

/* Called when the GtkMenu is deactivated (dismissed by any means) */
static void
on_menu_deactivate(GtkMenuShell *shell, gpointer user_data)
{
	Menu *menu = (Menu *) user_data;
	(void) shell;
	menu->visible = 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

Menu *
menu_create(xcb_connection_t *xc, xcb_window_t root, Drw *drw, Clr **scheme)
{
	Menu *menu;

	/* Suppress unused-parameter warnings — these args exist for API compat */
	(void) root;
	(void) drw;
	(void) scheme;

	menu = calloc(1, sizeof(Menu));
	if (!menu)
		return NULL;

	menu->xc         = xc;
	menu->owns_items = 1;
	menu->visible    = 0;
	menu->gtk_menu   = NULL;

	return menu;
}

void
menu_free(Menu *menu)
{
	if (!menu)
		return;

	if (menu->visible)
		menu_hide(menu);

	if (menu->gtk_menu) {
		gtk_widget_destroy(menu->gtk_menu);
		menu->gtk_menu = NULL;
	}

	if (menu->owns_items && menu->items)
		menu_items_free(menu->items);

	free(menu);
}

void
menu_set_items(Menu *menu, MenuItem *items)
{
	if (!menu)
		return;

	if (menu->owns_items && menu->items)
		menu_items_free(menu->items);

	menu->items      = items;
	menu->item_count = menu_items_count(items);

	/* Destroy old GTK menu so it gets rebuilt fresh on next show */
	if (menu->gtk_menu) {
		gtk_widget_destroy(menu->gtk_menu);
		menu->gtk_menu = NULL;
	}
}

void
menu_show(Menu *menu, int x, int y, MenuCallback callback, void *data,
    xcb_timestamp_t event_time)
{
	GdkRectangle    rect;
	GdkEvent       *trigger;
	GdkEventButton *btn;

	if (!menu || !menu->items)
		return;

	menu->callback      = callback;
	menu->callback_data = data;

	/* (Re)build the GTK widget tree from the MenuItem list */
	if (menu->gtk_menu) {
		gtk_widget_destroy(menu->gtk_menu);
		menu->gtk_menu = NULL;
	}

	menu->gtk_menu = build_gtk_menu(menu, menu->items);
	if (!menu->gtk_menu)
		return;

	g_signal_connect(
	    menu->gtk_menu, "deactivate", G_CALLBACK(on_menu_deactivate), menu);

	menu->visible = 1;

	/* Position at (x, y) using a zero-size anchor rectangle */
	rect.x      = x;
	rect.y      = y;
	rect.width  = 1;
	rect.height = 1;

	/* Build a synthetic GdkEventButton carrying the real X server timestamp
	 * and root coordinates.  Without this, gtk_menu_popup_at_rect uses
	 * GDK_CURRENT_TIME for its seat grab — which races against the XCB
	 * button-press awm already consumed — causing the grab to fail or the
	 * menu to appear without proper input ownership, leaving hotkeys dead. */
	trigger = gdk_event_new(GDK_BUTTON_PRESS);
	btn     = (GdkEventButton *) trigger;
	btn->window =
	    g_object_ref(gdk_screen_get_root_window(gdk_screen_get_default()));
	btn->send_event = TRUE;
	btn->time       = (guint32) event_time;
	btn->x_root     = (gdouble) x;
	btn->y_root     = (gdouble) y;
	btn->x          = (gdouble) x;
	btn->y          = (gdouble) y;
	btn->button     = 3;
	btn->state      = 0;
	btn->device     = gdk_seat_get_pointer(
        gdk_display_get_default_seat(gdk_display_get_default()));

	gtk_menu_popup_at_rect(GTK_MENU(menu->gtk_menu),
	    gdk_screen_get_root_window(gdk_screen_get_default()), &rect,
	    GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST, trigger);

	gdk_event_free(trigger);
}

void
menu_hide(Menu *menu)
{
	if (!menu || !menu->visible)
		return;

	if (menu->gtk_menu)
		gtk_menu_popdown(GTK_MENU(menu->gtk_menu));

	menu->visible = 0;
}

/* GTK processes all input through the GLib main loop — no XCB event
 * handling needed.  Always return 0 so the WM continues normal dispatch. */
int
menu_handle_event(Menu *menu, xcb_generic_event_t *ev)
{
	(void) menu;
	(void) ev;
	return 0;
}

/* -------------------------------------------------------------------------
 * Menu item helpers  (unchanged from original)
 * ---------------------------------------------------------------------- */

MenuItem *
menu_item_create(int id, const char *label, int enabled)
{
	MenuItem *item = calloc(1, sizeof(MenuItem));
	if (!item)
		return NULL;

	item->id           = id;
	item->label        = label ? strdup(label) : NULL;
	item->enabled      = enabled;
	item->is_separator = 0;
	item->submenu      = NULL;
	item->next         = NULL;

	return item;
}

MenuItem *
menu_separator_create(void)
{
	MenuItem *item = calloc(1, sizeof(MenuItem));
	if (!item)
		return NULL;

	item->is_separator = 1;
	item->enabled      = 0;
	item->next         = NULL;

	return item;
}

void
menu_item_free(MenuItem *item)
{
	if (!item)
		return;

	if (item->label)
		free(item->label);
	if (item->submenu)
		menu_items_free(item->submenu);
	free(item);
}

void
menu_items_free(MenuItem *head)
{
	MenuItem *item, *next;

	for (item = head; item; item = next) {
		next = item->next;
		menu_item_free(item);
	}
}

int
menu_items_count(MenuItem *head)
{
	MenuItem *item;
	int       count = 0;

	for (item = head; item; item = item->next) {
		if (!item->is_separator && item->label)
			count++;
	}

	return count;
}
