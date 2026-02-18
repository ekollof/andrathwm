/* See LICENSE file for copyright and license details. */

#ifndef SNI_H
#define SNI_H

#ifdef STATUSNOTIFIER

#include "drw.h"
#include <cairo/cairo-xlib.h>
#include <cairo/cairo.h>
#include <dbus/dbus.h>
#include <gtk/gtk.h>

/* StatusNotifier item status */
#define SNI_STATUS_PASSIVE 0
#define SNI_STATUS_ACTIVE 1
#define SNI_STATUS_NEEDSATTENTION 2

/* StatusNotifier item category */
#define SNI_CATEGORY_APPLICATIONSTATUS 0
#define SNI_CATEGORY_COMMUNICATIONS 1
#define SNI_CATEGORY_SYSTEMSERVICES 2
#define SNI_CATEGORY_HARDWARE 3

/* Icon structure for pixmap data */
typedef struct {
	int            width;
	int            height;
	unsigned char *pixels; /* ARGB32 format */
} SNIIcon;

/* DBusMenu item structure */
typedef struct SNIMenuItem {
	int                 id;
	char               *label;
	int                 enabled;
	int                 visible;
	int                 toggle_type; /* 0=none, 1=checkmark, 2=radio */
	int                 toggle_state;
	struct SNIMenuItem *submenu;
	struct SNIMenuItem *next;
} SNIMenuItem;

/* StatusNotifier item structure */
typedef struct SNIItem {
	char *service;  /* D-Bus service name */
	char *path;     /* D-Bus object path */
	char *id;       /* Item ID */
	char *title;    /* Item title */
	char *category; /* Category string */
	int   status;   /* Passive, Active, NeedsAttention */

	/* Icon data */
	char    *icon_name;   /* Icon theme name */
	SNIIcon *icon_pixmap; /* Icon pixmap array */
	int      icon_pixmap_count;
	char    *attention_icon_name;
	SNIIcon *attention_pixmap;
	int      attention_pixmap_count;

	/* Tooltip */
	char    *tooltip_title;
	char    *tooltip_text;
	SNIIcon *tooltip_icon;
	int      tooltip_icon_count;

	/* Menu */
	char        *menu_path; /* DBusMenu object path */
	SNIMenuItem *menu;      /* Parsed menu structure */

	/* Internal state */
	Window           win;                /* X11 window for this item */
	cairo_surface_t *surface;            /* Cairo surface for rendering */
	int              w, h;               /* Window size */
	int              mapped;             /* Window mapped state */
	int              properties_fetched; /* Flag to prevent infinite retry */

	/* Pending click: queued when click arrives before properties are ready */
	int pending_click;  /* 1 if a click is waiting */
	int pending_button; /* button number */
	int pending_x;      /* root x coordinate */
	int pending_y;      /* root y coordinate */

	struct SNIItem *next;
} SNIItem;

/* StatusNotifierWatcher state */
typedef struct {
	DBusConnection *conn;
	char           *unique_name;
	SNIItem        *items;
	int             item_count;
	int             host_registered;
} SNIWatcher;

/* Function declarations */

/* Initialization and cleanup */
int  sni_init(Display *display, Window rootwin, Drw *drw, Clr **scheme,
     unsigned int icon_size);
void sni_cleanup(void);

/* D-Bus event handling */
void sni_handle_dbus(void);
int  sni_get_fd(void);

/* Item management */
SNIItem *sni_find_item(const char *service);
void     sni_add_item(const char *service, const char *path);
void     sni_remove_item(SNIItem *item);
void     sni_update_item(SNIItem *item);

/* Icon rendering */
void sni_render_item(SNIItem *item);

/* Event handling */
void     sni_handle_click(Window win, int button, int x, int y);
SNIItem *sni_find_item_by_window(Window win);

/* Event handling */
void sni_scroll(SNIItem *item, int delta, const char *orientation);

/* Menu support */
void sni_show_menu(SNIItem *item, int x, int y);
void sni_free_menu(SNIMenuItem *menu);
int  sni_handle_menu_event(XEvent *ev);

/* D-Bus helpers */
DBusMessage *sni_call_method(const char *service, const char *path,
    const char *interface, const char *method);
int          sni_get_property_string(const char *service, const char *path,
             const char *interface, const char *property, char **value);
int          sni_get_property_int(const char *service, const char *path,
             const char *interface, const char *property, int *value);

/* Utility functions */
void sni_free_icon(SNIIcon *icon);
void sni_free_icons(SNIIcon *icons, int count);

/* Global state */
extern SNIWatcher *sni_watcher;

#endif /* STATUSNOTIFIER */

#endif /* SNI_H */
