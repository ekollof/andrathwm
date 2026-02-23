/* See LICENSE file for copyright and license details.
 *
 * Reusable menu system for awm — GTK backend
 *
 * Provides GTK-based popup menus with keyboard and mouse support.
 * All XCB dependencies have been removed; xcb_timestamp_t is typedef'd as
 * uint32_t in menu.h for the event_time parameter used by menu_show().
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

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
	awm_debug("menu: on_menu_deactivate fired");
	menu->visible = 0;

	if (menu->dismiss_callback)
		menu->dismiss_callback(menu->dismiss_data);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

Menu *
menu_create(void)
{
	Menu *menu;

	menu = calloc(1, sizeof(Menu));
	if (!menu)
		return NULL;

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
		if (menu->deactivate_handler) {
			g_signal_handler_disconnect(
			    menu->gtk_menu, menu->deactivate_handler);
			menu->deactivate_handler = 0;
		}
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

	menu->items = items;

	/* Destroy old GTK menu so it gets rebuilt fresh on next show */
	if (menu->gtk_menu) {
		if (menu->deactivate_handler) {
			g_signal_handler_disconnect(
			    menu->gtk_menu, menu->deactivate_handler);
			menu->deactivate_handler = 0;
		}
		gtk_widget_destroy(menu->gtk_menu);
		menu->gtk_menu = NULL;
	}
}

void
menu_show(Menu *menu, int x, int y, MenuCallback callback, void *data,
    xcb_timestamp_t event_time)
{
	if (!menu || !menu->items)
		return;

	awm_debug("menu_show: x=%d y=%d", x, y);

	menu->callback      = callback;
	menu->callback_data = data;

	/* (Re)build the GTK widget tree from the MenuItem list. */
	if (menu->gtk_menu) {
		if (menu->visible)
			gtk_menu_popdown(GTK_MENU(menu->gtk_menu));
		if (menu->deactivate_handler) {
			g_signal_handler_disconnect(
			    menu->gtk_menu, menu->deactivate_handler);
			menu->deactivate_handler = 0;
		}
		gtk_widget_destroy(menu->gtk_menu);
		menu->gtk_menu = NULL;
	}

	menu->gtk_menu = build_gtk_menu(menu, menu->items);
	if (!menu->gtk_menu)
		return;

	menu->deactivate_handler = g_signal_connect(
	    menu->gtk_menu, "deactivate", G_CALLBACK(on_menu_deactivate), menu);

	menu->visible = 1;

	/* Popup the GtkMenu.
	 *
	 * We do NOT call gdk_seat_grab() ourselves — GTK does it internally
	 * inside gtk_menu_popup_at_pointer().  Calling it beforehand causes
	 * GDK_GRAB_ALREADY_GRABBED because GTK registers passive XGrabButton
	 * handlers on every mapped GdkWindow (including our grab_win), and an
	 * explicit active gdk_seat_grab() on the same window triggers the
	 * AlreadyGrabbed error from the X server.
	 *
	 * We use:
	 *  - timing_win for gdk_x11_get_server_time (PropertyNotify round-trip)
	 *    to obtain a fresh server timestamp
	 *  - grab_win as ev->button.window — GTK passes this to its internal
	 *    gdk_seat_grab() call so it must be a viewable (mapped) window
	 */
	{
		GdkDisplay *dpy  = gtk_widget_get_display(menu->gtk_menu);
		GdkSeat    *seat = gdk_display_get_default_seat(dpy);
		GdkDevice  *ptr  = gdk_seat_get_pointer(seat);
		GdkWindow  *twin = menu->timing_win;
		GdkWindow  *gwin = menu->grab_win ? menu->grab_win : twin;
		guint32     ts =
            twin ? gdk_x11_get_server_time(twin) : (guint32) event_time;

		awm_debug("menu_show: ts=%u twin=%p gwin=%p", ts, (void *) twin,
		    (void *) gwin);

		/* Ungrab any stale GDK grab left by a previous failed popup
		 * (e.g. GTK's internal grab-transfer-window).  Do NOT probe with
		 * an explicit gdk_seat_grab — see comment above. */
		gdk_seat_ungrab(seat);
		gdk_display_flush(dpy);

		GdkEvent *ev      = gdk_event_new(GDK_BUTTON_PRESS);
		ev->button.time   = ts;
		ev->button.button = 3;
		ev->button.window = gwin ? g_object_ref(gwin) : NULL;
		gdk_event_set_device(ev, ptr);
		gtk_menu_popup_at_pointer(GTK_MENU(menu->gtk_menu), ev);
		gdk_event_free(ev);

		gdk_display_flush(dpy);
	}

	if (menu->visible && menu->gtk_menu &&
	    !gtk_widget_get_mapped(menu->gtk_menu)) {
		awm_warn(
		    "menu_show: menu not mapped after popup — forcing self-dismiss");
		on_menu_deactivate(GTK_MENU_SHELL(menu->gtk_menu), menu);
	} else if (!menu->visible) {
		awm_warn("menu_show: menu was immediately dismissed");
	} else {
		awm_debug("menu_show: menu mapped OK");
	}
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

/* -------------------------------------------------------------------------
 * Menu item helpers
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
