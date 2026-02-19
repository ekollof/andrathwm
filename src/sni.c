/* See LICENSE file for copyright and license details.
 *
 * StatusNotifier implementation for awm
 * Provides D-Bus based system tray support via StatusNotifier/AppIndicator
 * protocol
 */

#ifdef STATUSNOTIFIER

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cairo/cairo-xlib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dbus.h"
#include "drw.h"
#include "icon.h"
#include "log.h"
#include "menu.h"
#include "sni.h"

/* Forward declaration for awm integration */
extern void addsniiconsystray(Window w, int width, int height);
extern void removesniiconsystray(Window w);

/* D-Bus interface names */
#define WATCHER_BUS_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_OBJECT_PATH "/StatusNotifierWatcher"
#define WATCHER_INTERFACE "org.kde.StatusNotifierWatcher"
#define ITEM_INTERFACE "org.kde.StatusNotifierItem"
#define PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define DBUSMENU_INTERFACE "com.canonical.dbusmenu"

/* Maximum SNI items to prevent memory exhaustion from malicious apps */
#define SNI_MAX_ITEMS 64

/* Context struct for async GetAll calls — guards against use-after-free when
 * an SNIItem is removed while a GetAll reply is still in flight. */
typedef struct {
	SNIItem *item;
	uint32_t generation;
} SNIGetAllCtx;

/* Global state */
SNIWatcher            *sni_watcher    = NULL;
static DBusDispatcher *sni_dispatcher = NULL;

/* awm globals - set during sni_init() */
static Display     *sni_dpy    = NULL;
static Window       sni_root   = 0;
static Drw         *sni_drw    = NULL;
static Clr        **sni_scheme = NULL;
static unsigned int sniconsize = 22; /* Set during sni_init() */

/* Shared menu instance */
static Menu *sni_menu = NULL;

/* Forward declarations for internal functions */
static void sni_menu_item_activated(int item_id, SNIItem *item);
static void sni_register_host(void);

/* Forward declarations for DBus handlers */
static DBusHandlerResult sni_handle_register_item(
    DBusConnection *conn, DBusMessage *msg, void *data);
static DBusHandlerResult sni_handle_register_host(
    DBusConnection *conn, DBusMessage *msg, void *data);
static DBusHandlerResult sni_handle_properties_get(
    DBusConnection *conn, DBusMessage *msg, void *data);
static DBusHandlerResult sni_handle_properties_changed(
    DBusConnection *conn, DBusMessage *msg, void *data);
static DBusHandlerResult sni_handle_item_signal(
    DBusConnection *conn, DBusMessage *msg, void *data);
static DBusHandlerResult sni_handle_name_owner_changed(
    DBusConnection *conn, DBusMessage *msg, void *data);

/* Menu item activation callback */
static void
sni_menu_activated(int item_id, void *data)
{
	SNIItem *item = (SNIItem *) data;

	if (!item)
		return;

	awm_debug("SNI: Menu item %d selected for %s", item_id, item->service);
	sni_menu_item_activated(item_id, item);
}

static void sni_fetch_item_properties(SNIItem *item);

/* ============================================================================
 * Initialization and Cleanup
 * ============================================================================
 */

int
sni_init(Display *display, Window rootwin, Drw *drw, Clr **scheme,
    unsigned int icon_size)
{
	DBusError err;

	if (!display)
		return 0;

	sni_dpy    = display;
	sni_root   = rootwin;
	sni_drw    = drw;
	sni_scheme = scheme;
	sniconsize = icon_size;

	/* Disable glycin loaders - they use subprocesses which can deadlock
	 * with async operations and window manager event loops */
	setenv("GDK_PIXBUF_DISABLE_GLYCIN", "1", 1);

	sni_watcher = calloc(1, sizeof(SNIWatcher));
	if (!sni_watcher)
		return 0;

	/* Create message dispatcher */
	sni_dispatcher = dbus_dispatcher_new();
	if (!sni_dispatcher) {
		free(sni_watcher);
		sni_watcher = NULL;
		return 0;
	}

	/* Register method handlers */
	dbus_dispatcher_register_method(sni_dispatcher, WATCHER_INTERFACE,
	    "RegisterStatusNotifierItem", sni_handle_register_item, NULL);
	dbus_dispatcher_register_method(sni_dispatcher, WATCHER_INTERFACE,
	    "RegisterStatusNotifierHost", sni_handle_register_host, NULL);
	dbus_dispatcher_register_method(sni_dispatcher, PROPERTIES_INTERFACE,
	    "Get", sni_handle_properties_get, NULL);

	/* Register signal handlers */
	dbus_dispatcher_register_signal(sni_dispatcher,
	    "org.freedesktop.DBus.Properties", "PropertiesChanged",
	    sni_handle_properties_changed, NULL);
	dbus_dispatcher_register_signal(sni_dispatcher, ITEM_INTERFACE, "NewIcon",
	    sni_handle_item_signal, NULL);
	dbus_dispatcher_register_signal(sni_dispatcher, ITEM_INTERFACE,
	    "NewAttentionIcon", sni_handle_item_signal, NULL);
	dbus_dispatcher_register_signal(sni_dispatcher, ITEM_INTERFACE,
	    "NewStatus", sni_handle_item_signal, NULL);
	dbus_dispatcher_register_signal(sni_dispatcher, ITEM_INTERFACE,
	    "NewToolTip", sni_handle_item_signal, NULL);
	dbus_dispatcher_register_signal(sni_dispatcher, "org.freedesktop.DBus",
	    "NameOwnerChanged", sni_handle_name_owner_changed, NULL);

	/* Connect to session bus and register as StatusNotifierWatcher */
	sni_watcher->conn = dbus_helper_session_connect_dispatcher(
	    WATCHER_BUS_NAME, sni_dispatcher, &sni_watcher->unique_name);
	if (!sni_watcher->conn) {
		dbus_dispatcher_free(sni_dispatcher);
		sni_dispatcher = NULL;
		free(sni_watcher);
		sni_watcher = NULL;
		return 0;
	}

	/* Subscribe to NameOwnerChanged signals to detect when apps exit */
	dbus_error_init(&err);
	dbus_bus_add_match(sni_watcher->conn,
	    "type='signal',sender='org.freedesktop.DBus',"
	    "interface='org.freedesktop.DBus',member='NameOwnerChanged'",
	    &err);
	if (dbus_error_is_set(&err)) {
		awm_error("Failed to subscribe to NameOwnerChanged: %s", err.message);
		dbus_error_free(&err);
	}

	/* Register host */
	sni_register_host();

	/* Initialize icon module */
	icon_init();

	/* Create menu instance */
	sni_menu = menu_create(sni_dpy, sni_root, sni_drw, sni_scheme);
	if (!sni_menu)
		awm_warn("SNI: Failed to create menu");

	awm_debug("SNI: StatusNotifier support initialized (service: %s)",
	    sni_watcher->unique_name);

	return 1;
}

void
sni_cleanup(void)
{
	SNIItem *item, *next;

	if (!sni_watcher)
		return;

	/* Clean up all items */
	for (item = sni_watcher->items; item; item = next) {
		next = item->next;
		sni_remove_item(item);
	}

	/* Clean up icon module */
	icon_cleanup();

	/* Clean up menu */
	if (sni_menu) {
		menu_free(sni_menu);
		sni_menu = NULL;
	}

	/* Release D-Bus name */
	if (sni_watcher->conn) {
		dbus_bus_release_name(sni_watcher->conn, WATCHER_BUS_NAME, NULL);
		dbus_connection_unref(sni_watcher->conn);
	}

	/* Free dispatcher */
	if (sni_dispatcher) {
		dbus_dispatcher_free(sni_dispatcher);
		sni_dispatcher = NULL;
	}

	free(sni_watcher->unique_name);
	free(sni_watcher);
	sni_watcher = NULL;
}

/* ============================================================================
 * D-Bus Event Handling
 * ============================================================================
 */

/* Re-connect to D-Bus after a HUP/ERR disconnect.
 * Preserves the awm globals (display, root, drw, scheme, icon size) that were
 * set by the original sni_init() call and are never changed at runtime. */
int
sni_reconnect(void)
{
	Display     *dpy    = sni_dpy;
	Window       root   = sni_root;
	Drw         *drw    = sni_drw;
	Clr        **scheme = sni_scheme;
	unsigned int sz     = sniconsize;

	if (!dpy)
		return 0; /* never successfully initialised — cannot reconnect */

	sni_cleanup();
	return sni_init(dpy, root, drw, scheme, sz);
}

int
sni_get_fd(void)
{
	int fd;

	if (!sni_watcher || !sni_watcher->conn)
		return -1;

	if (!dbus_connection_get_unix_fd(sni_watcher->conn, &fd))
		return -1;

	return fd;
}

void
sni_handle_dbus(void)
{
	SNIItem *item;

	if (!sni_watcher || !sni_watcher->conn)
		return;

	/* Read data from socket into D-Bus internal buffers */
	dbus_connection_read_write(sni_watcher->conn, 0);

	/* Process pending D-Bus messages */
	while (dbus_connection_dispatch(sni_watcher->conn) ==
	    DBUS_DISPATCH_DATA_REMAINS)
		;

	/* After handling messages, fetch properties for items that need them.
	 * Guard with properties_fetching to prevent multiple in-flight GetAll
	 * requests for the same item (this function is called on every D-Bus
	 * readable event, which can fire many times before the reply arrives). */
	for (item = sni_watcher->items; item; item = item->next) {
		if (!item->properties_fetched && !item->properties_fetching) {
			sni_fetch_item_properties(item);
			/* Properties will be fetched async, render happens in callback */
		}
	}
}

static void
sni_register_host(void)
{
	DBusMessage    *msg;
	DBusMessageIter args;
	const char     *service;

	if (!sni_watcher || !sni_watcher->conn)
		return;

	service = sni_watcher->unique_name;

	msg = dbus_message_new_method_call(WATCHER_BUS_NAME, WATCHER_OBJECT_PATH,
	    WATCHER_INTERFACE, "RegisterStatusNotifierHost");
	if (!msg)
		return;

	dbus_message_iter_init_append(msg, &args);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &service);

	dbus_connection_send(sni_watcher->conn, msg, NULL);
	dbus_connection_flush(sni_watcher->conn);
	dbus_message_unref(msg);

	sni_watcher->host_registered = 1;
}

/* ============================================================================
 * D-Bus Message Handlers (used by dispatcher)
 * ============================================================================
 */

static DBusHandlerResult
sni_handle_register_item(DBusConnection *conn, DBusMessage *msg, void *data)
{
	const char     *param     = NULL;
	const char     *service   = NULL;
	const char     *item_path = NULL;
	DBusMessageIter args;

	/* Get the parameter - could be service name or object path */
	if (dbus_message_iter_init(msg, &args) &&
	    dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
		dbus_message_iter_get_basic(&args, &param);
	}

	/* Determine service and path */
	if (!param || param[0] == '/') {
		/* Parameter is a path (or empty) - use sender as service */
		service   = dbus_message_get_sender(msg);
		item_path = param ? param : "/StatusNotifierItem";
	} else {
		/* Parameter is a service name */
		service   = param;
		item_path = "/StatusNotifierItem";
	}

	if (service) {
		/* Send reply FIRST before doing any D-Bus calls */
		dbus_helper_send_reply(conn, msg);

		/* Now add the item (which will make D-Bus calls) */
		sni_add_item(service, item_path);

		/* Emit signal */
		DBusMessage *signal = dbus_helper_create_signal(WATCHER_OBJECT_PATH,
		    WATCHER_INTERFACE, "StatusNotifierItemRegistered");
		if (signal) {
			dbus_message_iter_init_append(signal, &args);
			dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &service);
			dbus_connection_send(conn, signal, NULL);
			dbus_connection_flush(conn);
			dbus_message_unref(signal);
		}

		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
sni_handle_register_host(DBusConnection *conn, DBusMessage *msg, void *data)
{
	/* Already handled in sni_register_host, just reply */
	dbus_helper_send_reply(conn, msg);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
sni_handle_properties_get(DBusConnection *conn, DBusMessage *msg, void *data)
{
	const char     *path  = dbus_message_get_path(msg);
	const char     *iface = NULL, *property = NULL;
	DBusMessageIter args;
	DBusMessage    *reply;

	/* Only handle requests for our watcher object */
	if (!path || strcmp(path, WATCHER_OBJECT_PATH) != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (dbus_message_iter_init(msg, &args) &&
	    dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
		dbus_message_iter_get_basic(&args, &iface);
		dbus_message_iter_next(&args);
		if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&args, &property);
		}
	}

	if (!iface || strcmp(iface, WATCHER_INTERFACE) != 0 || !property)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	reply = dbus_message_new_method_return(msg);
	if (reply) {
		DBusMessageIter iter, variant;
		dbus_message_iter_init_append(reply, &iter);

		if (strcmp(property, "RegisteredStatusNotifierItems") == 0) {
			/* Return array of registered items */
			dbus_message_iter_open_container(
			    &iter, DBUS_TYPE_VARIANT, "as", &variant);
			DBusMessageIter array;
			dbus_message_iter_open_container(
			    &variant, DBUS_TYPE_ARRAY, "s", &array);

			SNIItem *item;
			for (item = sni_watcher->items; item; item = item->next) {
				if (item->service)
					dbus_message_iter_append_basic(
					    &array, DBUS_TYPE_STRING, &item->service);
			}

			dbus_message_iter_close_container(&variant, &array);
			dbus_message_iter_close_container(&iter, &variant);
		} else if (strcmp(property, "IsStatusNotifierHostRegistered") == 0) {
			dbus_message_iter_open_container(
			    &iter, DBUS_TYPE_VARIANT, "b", &variant);
			dbus_message_iter_append_basic(
			    &variant, DBUS_TYPE_BOOLEAN, &sni_watcher->host_registered);
			dbus_message_iter_close_container(&iter, &variant);
		} else if (strcmp(property, "ProtocolVersion") == 0) {
			int version = 0;
			dbus_message_iter_open_container(
			    &iter, DBUS_TYPE_VARIANT, "i", &variant);
			dbus_message_iter_append_basic(
			    &variant, DBUS_TYPE_INT32, &version);
			dbus_message_iter_close_container(&iter, &variant);
		}

		dbus_connection_send(conn, reply, NULL);
		dbus_connection_flush(conn);
		dbus_message_unref(reply);
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
sni_handle_properties_changed(
    DBusConnection *conn, DBusMessage *msg, void *data)
{
	const char *sender = dbus_message_get_sender(msg);
	SNIItem    *item   = sni_find_item(sender);

	if (item) {
		/* Properties changed, update the item */
		sni_update_item(item);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
sni_handle_item_signal(DBusConnection *conn, DBusMessage *msg, void *data)
{
	const char *sender = dbus_message_get_sender(msg);
	SNIItem    *item   = sni_find_item(sender);

	if (item) {
		sni_update_item(item);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
sni_handle_name_owner_changed(
    DBusConnection *conn, DBusMessage *msg, void *data)
{
	const char     *name = NULL, *old_owner = NULL, *new_owner = NULL;
	DBusMessageIter args;

	if (dbus_message_iter_init(msg, &args) &&
	    dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
		dbus_message_iter_get_basic(&args, &name);
		dbus_message_iter_next(&args);
		if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&args, &old_owner);
			dbus_message_iter_next(&args);
			if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&args, &new_owner);
			}
		}
	}

	/* If new_owner is empty, the name was released (app exited) */
	if (name && new_owner && strlen(new_owner) == 0) {
		SNIItem *item = sni_find_item(name);
		if (item) {
			awm_debug("SNI: Item %s disappeared, removing", name);
			sni_remove_item(item);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ============================================================================
 * Item Management
 * ============================================================================
 */

SNIItem *
sni_find_item(const char *service)
{
	SNIItem *item;

	if (!sni_watcher || !service)
		return NULL;

	for (item = sni_watcher->items; item; item = item->next) {
		if (item->service && strcmp(item->service, service) == 0)
			return item;
	}

	return NULL;
}

void
sni_add_item(const char *service, const char *path)
{
	SNIItem *item;

	if (!sni_watcher || !service)
		return;

	if (sni_find_item(service))
		return;

	if (sni_watcher->item_count >= SNI_MAX_ITEMS) {
		awm_error("SNI: Maximum items reached (%d), rejecting %s",
		    SNI_MAX_ITEMS, service);
		return;
	}

	item = calloc(1, sizeof(SNIItem));
	if (!item)
		return;

	item->service = strdup(service);
	item->path    = strdup(path ? path : "/StatusNotifierItem");
	item->status  = SNI_STATUS_PASSIVE;

	item->next         = sni_watcher->items;
	sni_watcher->items = item;
	sni_watcher->item_count++;

	awm_info(
	    "SNI: StatusNotifier item registered: %s at %s (properties pending)",
	    service, item->path);
}

void
sni_remove_item(SNIItem *item)
{
	SNIItem **p;

	if (!sni_watcher || !item)
		return;

	/* Remove from list */
	for (p = &sni_watcher->items; *p; p = &(*p)->next) {
		if (*p == item) {
			*p = item->next;
			sni_watcher->item_count--;
			break;
		}
	}

	/* Clean up item */
	free(item->service);
	free(item->path);
	free(item->icon_name);
	free(item->menu_path);

	if (item->icon_pixmap)
		sni_free_icons(item->icon_pixmap, item->icon_pixmap_count);

	if (item->menu)
		sni_free_menu(item->menu);

	if (item->win) {
		removesniiconsystray(item->win);
		XDestroyWindow(sni_dpy, item->win);
	}

	item->generation++; /* invalidate any in-flight async ctx */
	free(item);
}

/* ============================================================================
 * D-Bus Helper Functions
 * ============================================================================
 */

/* Wrapper around generic dbus helper */
DBusMessage *
sni_call_method(const char *service, const char *path, const char *interface,
    const char *method)
{
	if (!sni_watcher || !sni_watcher->conn)
		return NULL;

	return dbus_helper_call_method(
	    sni_watcher->conn, service, path, interface, method);
}

/* Wrapper around generic dbus helper */
int
sni_get_property_string(const char *service, const char *path,
    const char *interface, const char *property, char **value)
{
	if (!sni_watcher || !sni_watcher->conn)
		return 0;

	return dbus_helper_get_property_string(
	    sni_watcher->conn, service, path, interface, property, value);
}

/* Wrapper around generic dbus helper */
int
sni_get_property_int(const char *service, const char *path,
    const char *interface, const char *property, int *value)
{
	if (!sni_watcher || !sni_watcher->conn)
		return 0;

	return dbus_helper_get_property_int(
	    sni_watcher->conn, service, path, interface, property, value);
}

/* Callback for async GetAll properties */
static void
sni_properties_received(DBusMessage *reply, void *user_data)
{
	SNIGetAllCtx   *ctx = (SNIGetAllCtx *) user_data;
	SNIItem        *item;
	DBusMessageIter args, dict_iter, entry, variant;
	const char     *key;

	/* Always free the context regardless of outcome */
	if (!ctx) {
		return;
	}
	item = ctx->item;

	/* Validate: if the item was removed while GetAll was in flight, its
	 * generation will have been incremented and the pointer is now freed. */
	if (!item || !reply || item->generation != ctx->generation) {
		free(ctx);
		return;
	}
	free(ctx);
	ctx = NULL; /* prevent accidental use */

	/* Reply is a{sv} - dict of string->variant */
	if (!dbus_message_iter_init(reply, &args) ||
	    dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
		awm_error("SNI: Invalid GetAll reply for %s", item->service);
		return;
	}

	/* Iterate over dictionary entries */
	dbus_message_iter_recurse(&args, &dict_iter);
	while (
	    dbus_message_iter_get_arg_type(&dict_iter) == DBUS_TYPE_DICT_ENTRY) {
		dbus_message_iter_recurse(&dict_iter, &entry);

		/* Get key */
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
			goto next_entry;
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);

		/* Get variant */
		if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
			goto next_entry;
		dbus_message_iter_recurse(&entry, &variant);

		/* Parse known properties */
		if (strcmp(key, "IconName") == 0) {
			char *val = dbus_iter_get_variant_string(&variant);
			if (val) {
				free(item->icon_name);
				item->icon_name = val;
			}
		} else if (strcmp(key, "Menu") == 0) {
			/* Menu can be STRING or OBJECT_PATH */
			int type = dbus_message_iter_get_arg_type(&variant);
			if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
				const char *val;
				dbus_message_iter_get_basic(&variant, &val);
				free(item->menu_path);
				item->menu_path = strdup(val);
			}
		} else if (strcmp(key, "ItemIsMenu") == 0) {
			if (dbus_message_iter_get_arg_type(&variant) ==
			    DBUS_TYPE_BOOLEAN) {
				dbus_bool_t val;
				dbus_message_iter_get_basic(&variant, &val);
				item->item_is_menu = val ? 1 : 0;
			}
		} else if (strcmp(key, "Status") == 0) {
			char *val = dbus_iter_get_variant_string(&variant);
			if (val) {
				if (strcmp(val, "Passive") == 0)
					item->status = SNI_STATUS_PASSIVE;
				else if (strcmp(val, "Active") == 0)
					item->status = SNI_STATUS_ACTIVE;
				else if (strcmp(val, "NeedsAttention") == 0)
					item->status = SNI_STATUS_NEEDSATTENTION;
				free(val);
			}
		} else if (strcmp(key, "IconPixmap") == 0) {
			/* Parse IconPixmap: array of (int32, int32, array of bytes) */
			if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
				DBusMessageIter array_iter, struct_iter, data_iter;
				int             icon_count = 0;
				SNIIcon        *icons      = NULL;

				/* Count icons first */
				DBusMessageIter tmp = variant;
				dbus_message_iter_recurse(&tmp, &struct_iter);
				while (dbus_message_iter_get_arg_type(&struct_iter) ==
				    DBUS_TYPE_STRUCT) {
					icon_count++;
					dbus_message_iter_next(&struct_iter);
				}

				if (icon_count > 0) {
					icons = calloc(icon_count, sizeof(SNIIcon));
					if (icons) {
						int i = 0;
						dbus_message_iter_recurse(&variant, &array_iter);
						while (dbus_message_iter_get_arg_type(&array_iter) ==
						        DBUS_TYPE_STRUCT &&
						    i < icon_count) {
							DBusMessageIter inner;
							dbus_int32_t    width, height;

							dbus_message_iter_recurse(&array_iter, &inner);

							/* Get width */
							if (dbus_message_iter_get_arg_type(&inner) ==
							    DBUS_TYPE_INT32) {
								dbus_message_iter_get_basic(&inner, &width);
								dbus_message_iter_next(&inner);
							} else {
								break;
							}

							/* Get height */
							if (dbus_message_iter_get_arg_type(&inner) ==
							    DBUS_TYPE_INT32) {
								dbus_message_iter_get_basic(&inner, &height);
								dbus_message_iter_next(&inner);
							} else {
								break;
							}

							icons[i].width  = width;
							icons[i].height = height;

							/* Get pixel data */
							if (dbus_message_iter_get_arg_type(&inner) ==
							    DBUS_TYPE_ARRAY) {
								dbus_message_iter_recurse(&inner, &data_iter);
								int            n_elements = 0;
								unsigned char *data;

								dbus_message_iter_get_fixed_array(
								    &data_iter, &data, &n_elements);

								if (n_elements == width * height * 4) {
									icons[i].pixels = malloc(n_elements);
									if (icons[i].pixels)
										memcpy(
										    icons[i].pixels, data, n_elements);
								}
							}

							i++;
							dbus_message_iter_next(&array_iter);
						}

						item->icon_pixmap       = icons;
						item->icon_pixmap_count = i;
						awm_debug("SNI: Parsed %d IconPixmap icons for %s", i,
						    item->service);
					}
				}
			}
		}

	next_entry:
		dbus_message_iter_next(&dict_iter);
	}

	/* Debug output */
	if (item->menu_path)
		awm_debug(
		    "SNI: Item %s has menu at %s", item->service, item->menu_path);
	else
		awm_debug("SNI: Item %s has no menu", item->service);

	awm_debug("SNI: Properties fetched for %s (Icon: %s)", item->service,
	    item->icon_name ? item->icon_name : "none");

	/* Mark properties as fetched */
	item->properties_fetched  = 1;
	item->properties_fetching = 0;

	/* Render icon now that we have properties */
	sni_render_item(item);
	/* Note: systray will update automatically when window is mapped */

	/* Drain any click that arrived before properties were ready */
	if (item->pending_click) {
		awm_debug("SNI: Draining pending click (button %d) for %s",
		    item->pending_button, item->service);
		item->pending_click = 0;
		sni_handle_click(item->win, item->pending_button, item->pending_x,
		    item->pending_y, item->pending_time);
	}
}

static void
sni_fetch_item_properties(SNIItem *item)
{
	char          match[512];
	SNIGetAllCtx *ctx;

	if (!item || !item->service || !item->path)
		return;

	/* Allocate context to guard against use-after-free in the callback */
	ctx = malloc(sizeof(SNIGetAllCtx));
	if (!ctx) {
		awm_error("SNI: OOM allocating GetAll ctx for %s", item->service);
		return;
	}
	ctx->item       = item;
	ctx->generation = item->generation;

	/* Fetch all properties in one async call */
	if (!dbus_helper_get_all_properties_async(sni_watcher->conn, item->service,
	        item->path, ITEM_INTERFACE, sni_properties_received, ctx)) {
		awm_error("SNI: Failed to start GetAll for %s", item->service);
		free(ctx);
		return;
	}

	/* Mark in-flight so sni_handle_dbus() won't queue another GetAll
	 * before this reply arrives */
	item->properties_fetching = 1;

	/* Subscribe to property changes */
	snprintf(match, sizeof(match),
	    "type='signal',sender='%s',interface='org.freedesktop.DBus."
	    "Properties'",
	    item->service);
	if (!dbus_helper_add_match(sni_watcher->conn, match))
		awm_warn("SNI: Failed to add Properties match for %s", item->service);

	/* Also subscribe to item-specific signals */
	snprintf(match, sizeof(match), "type='signal',sender='%s',interface='%s'",
	    item->service, ITEM_INTERFACE);
	if (!dbus_helper_add_match(sni_watcher->conn, match))
		awm_warn("SNI: Failed to add item signal match for %s", item->service);
}

void
sni_update_item(SNIItem *item)
{
	if (!item)
		return;

	/* Free old data */
	free(item->icon_name);
	item->icon_name = NULL;

	if (item->icon_pixmap) {
		sni_free_icons(item->icon_pixmap, item->icon_pixmap_count);
		item->icon_pixmap       = NULL;
		item->icon_pixmap_count = 0;
	}

	/* Reset fetch guards so sni_fetch_item_properties() is allowed to send
	 * a new GetAll request.  Without this, a second call to sni_update_item()
	 * while a GetAll reply is still in flight (properties_fetching=1) would
	 * silently drop the re-fetch; and once properties_fetched=1 was set the
	 * guard in sni_handle_dbus() would never re-fetch at all. */
	item->properties_fetched  = 0;
	item->properties_fetching = 0;

	/* Re-fetch properties */
	sni_fetch_item_properties(item);
	/* Render will happen in async callback */
}

/* ============================================================================
 * Utility Functions
 * ============================================================================
 */

void
sni_free_icon(SNIIcon *icon)
{
	if (icon) {
		free(icon->pixels);
		icon->pixels = NULL;
	}
}

void
sni_free_icons(SNIIcon *icons, int count)
{
	int i;

	if (!icons)
		return;

	for (i = 0; i < count; i++)
		sni_free_icon(&icons[i]);

	free(icons);
}

/* ============================================================================
 * Async Icon Loading with GIO/GdkPixbuf
 * ============================================================================
 */

/* Callback data for async icon loading (SNI-specific) */
typedef struct {
	SNIItem *item;
	int      size;
} SNIIconLoadData;

/* Render an icon surface into the item's X window.
 * Called directly on the GLib main thread (from GIO async callbacks or
 * inline for pixmap icons), so no queue indirection is needed. */
static void
sni_icon_render(SNIItem *item, int icon_size, cairo_surface_t *icon_surface)
{
	cairo_t         *cr;
	Pixmap           pixmap;
	cairo_surface_t *pixmap_surface;

	if (!item || !item->win || !sni_dpy) {
		awm_debug("SNI: Icon loaded but item/window invalid");
		if (icon_surface)
			cairo_surface_destroy(icon_surface);
		return;
	}

	if (!icon_surface) {
		awm_error("SNI: Failed to load icon for %s", item->service);
		return;
	}

	awm_debug("SNI: Rendering icon for %s", item->service);

	/* Render the icon to window */
	pixmap         = XCreatePixmap(sni_dpy, item->win, icon_size, icon_size,
	            DefaultDepth(sni_dpy, DefaultScreen(sni_dpy)));
	pixmap_surface = cairo_xlib_surface_create(sni_dpy, pixmap,
	    DefaultVisual(sni_dpy, DefaultScreen(sni_dpy)), icon_size, icon_size);

	cr = cairo_create(pixmap_surface);
	/* Fill with bar background colour — the pixmap is 24-bit (no alpha
	 * channel) so a transparent clear would just produce opaque black. */
	if (sni_scheme) {
		Clr bg = sni_scheme[0][ColBg]; /* sni_scheme[SchemeNorm][ColBg] */
		cairo_set_source_rgb(cr, bg.color.red / 65535.0,
		    bg.color.green / 65535.0, bg.color.blue / 65535.0);
	} else {
		cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);

	/* Draw icon */
	cairo_set_source_surface(cr, icon_surface, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_paint(cr);

	cairo_destroy(cr);
	cairo_surface_destroy(icon_surface);

	/* Set as window background */
	XSetWindowBackgroundPixmap(sni_dpy, item->win, pixmap);
	XClearWindow(sni_dpy, item->win);
	cairo_surface_destroy(pixmap_surface);
	XFreePixmap(sni_dpy, pixmap);

	awm_debug("SNI: Icon rendered for %s", item->service);
}

/* Callback invoked by icon_load_async() on the GLib main loop once the
 * GIO async pipeline (file read + pixbuf decode) completes.  We are
 * already on the main thread, so render directly. */
static void
sni_icon_loaded_cb(cairo_surface_t *icon_surface, void *user_data)
{
	SNIIconLoadData *data = (SNIIconLoadData *) user_data;

	if (!data)
		return;

	sni_icon_render(data->item, data->size, icon_surface);
	free(data);
}

/* Start async icon loading for item.  For pixmap icons the surface is
 * built synchronously (pure CPU, no I/O) and rendered immediately.
 * For name/path icons the GIO async pipeline is launched; the callback
 * lands back on the main loop via the GLib main context. */
static void
sni_queue_icon_load(SNIItem *item)
{
	const char *icon_path = NULL;

	if (!item)
		return;

	awm_debug("SNI: Starting icon load for %s", item->service);

	/* If item has pixmap data, convert and render directly (CPU only, no I/O)
	 */
	if (item->icon_pixmap && item->icon_pixmap_count > 0) {
		cairo_surface_t *icon_surface;

		awm_debug("SNI: Using IconPixmap for %s (%d icons)", item->service,
		    item->icon_pixmap_count);

		/* SNIIcon and Icon are identical structures, safe to cast */
		icon_surface = icon_pixmap_to_surface(
		    (Icon *) item->icon_pixmap, item->icon_pixmap_count, sniconsize);
		if (!icon_surface) {
			awm_error("SNI: Failed to convert pixmap to surface for %s",
			    item->service);
			return;
		}

		sni_icon_render(item, sniconsize, icon_surface);
		return;
	}

	/* Resolve icon name/path */
	if (item->icon_name) {
		if (item->icon_name[0] == '/') {
			/* Absolute path — load synchronously via icon_load() so that
			 * SVG files are handled correctly (icon_load_async uses
			 * gdk_pixbuf which does not render SVGs). */
			cairo_surface_t *abs_surface =
			    icon_load(item->icon_name, sniconsize);
			if (abs_surface) {
				/* sni_icon_render() consumes the reference — do not
				 * cairo_surface_destroy here, that would double-free
				 * the surface still held by the icon cache and corrupt
				 * the cache entry for this path. */
				sni_icon_render(item, sniconsize, abs_surface);
			} else {
				awm_debug("SNI: Failed to load absolute icon %s for %s",
				    item->icon_name, item->service);
			}
			return;
		} else {
			/* Theme name — look up synchronously (fast index lookup, no I/O)
			 */
			GtkIconTheme *icon_theme = gtk_icon_theme_get_default();
			GtkIconInfo  *icon_info  = gtk_icon_theme_lookup_icon(icon_theme,
			      item->icon_name, sniconsize,
			      GTK_ICON_LOOKUP_USE_BUILTIN |
			          GTK_ICON_LOOKUP_GENERIC_FALLBACK);

			if (icon_info) {
				icon_path = gtk_icon_info_get_filename(icon_info);
				if (icon_path) {
					char *path_copy = strdup(icon_path);
					g_object_unref(icon_info);
					if (path_copy) {
						SNIIconLoadData *data =
						    malloc(sizeof(SNIIconLoadData));
						if (data) {
							data->item = item;
							data->size = sniconsize;
							icon_load_async(path_copy, sniconsize,
							    sni_icon_loaded_cb, data);
						}
						free(path_copy);
					}
					return;
				}
				g_object_unref(icon_info);
			}
		}
	}

	awm_debug(
	    "SNI: No icon path found for %s, keeping placeholder", item->service);
}

/* ============================================================================
 * Icon Rendering - Basic Implementation (to be completed)
 * ============================================================================
 */

void
sni_render_item(SNIItem *item)
{
	cairo_t         *cr;
	Pixmap           pixmap;
	cairo_surface_t *pixmap_surface;

	if (!item || !sni_dpy)
		return;

	awm_debug("SNI: Rendering item %s (icon_name=%s)", item->service,
	    item->icon_name ? item->icon_name : "NULL");

	/* Create window if needed */
	if (!item->win) {
		XSetWindowAttributes wa;
		wa.override_redirect = True;
		wa.background_pixmap = None;
		wa.border_pixel      = 0;
		wa.event_mask = ButtonPressMask | ExposureMask | StructureNotifyMask;

		item->win = XCreateWindow(sni_dpy, sni_root, 0, 0, sniconsize,
		    sniconsize, 0, CopyFromParent, InputOutput, CopyFromParent,
		    CWOverrideRedirect | CWBackPixmap | CWBorderPixel | CWEventMask,
		    &wa);

		if (!item->win) {
			awm_error("SNI: Failed to create window for %s", item->service);
			return;
		}

		awm_debug(
		    "SNI: Created window 0x%lx for %s", item->win, item->service);

		item->w = sniconsize;
		item->h = sniconsize;

		/* Add to systray BEFORE rendering so it's in the right parent */
		awm_debug("SNI: Adding window to systray before rendering");
		addsniiconsystray(item->win, item->w, item->h);
		item->mapped = 1;
	}

	/* Render placeholder immediately (non-blocking) */
	pixmap = XCreatePixmap(sni_dpy, item->win, sniconsize, sniconsize,
	    DefaultDepth(sni_dpy, DefaultScreen(sni_dpy)));

	pixmap_surface = cairo_xlib_surface_create(sni_dpy, pixmap,
	    DefaultVisual(sni_dpy, DefaultScreen(sni_dpy)), sniconsize,
	    sniconsize);

	if (cairo_surface_status(pixmap_surface) != CAIRO_STATUS_SUCCESS) {
		awm_error(
		    "SNI: Failed to create pixmap surface for %s", item->service);
		XFreePixmap(sni_dpy, pixmap);
		return;
	}

	cr = cairo_create(pixmap_surface);

	/* Fill with bar background colour as base — alpha is discarded on a
	 * 24-bit pixmap so a semi-transparent paint would become opaque black. */
	if (sni_scheme) {
		Clr bg = sni_scheme[0][ColBg]; /* sni_scheme[SchemeNorm][ColBg] */
		cairo_set_source_rgb(cr, bg.color.red / 65535.0,
		    bg.color.green / 65535.0, bg.color.blue / 65535.0);
	} else {
		cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(cr);

	/* Draw a subtle loading indicator circle on top */
	cairo_set_source_rgba(cr, 0.7, 0.7, 0.7, 0.5);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_arc(
	    cr, sniconsize / 2, sniconsize / 2, sniconsize / 4, 0, 2 * 3.14159);
	cairo_fill(cr);

	cairo_destroy(cr);
	cairo_surface_flush(pixmap_surface);
	cairo_surface_destroy(pixmap_surface);

	/* Set as window background */
	XSetWindowBackgroundPixmap(sni_dpy, item->win, pixmap);
	XClearWindow(sni_dpy, item->win);
	XFreePixmap(sni_dpy, pixmap);

	awm_debug("SNI: Placeholder rendered for %s", item->service);

	/* Queue actual icon loading for next event loop iteration */
	sni_queue_icon_load(item);
}

void
sni_scroll(SNIItem *item, int delta, const char *orientation)
{
	DBusMessage    *msg;
	DBusMessageIter args;
	dbus_int32_t    d = delta;

	if (!item || !sni_watcher || !sni_watcher->conn || !orientation)
		return;

	msg = dbus_message_new_method_call(
	    item->service, item->path, ITEM_INTERFACE, "Scroll");
	if (!msg)
		return;

	dbus_message_iter_init_append(msg, &args);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &d);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &orientation);

	dbus_connection_send(sni_watcher->conn, msg, NULL);
	dbus_connection_flush(sni_watcher->conn);
	dbus_message_unref(msg);
}

void
sni_free_menu(SNIMenuItem *menu)
{
	SNIMenuItem *item, *next;

	for (item = menu; item; item = next) {
		next = item->next;
		free(item->label);
		if (item->submenu)
			sni_free_menu(item->submenu);
		free(item);
	}
}

/* ============================================================================
 * Event Handling
 * ============================================================================
 */

/* Find SNI item by window ID */
SNIItem *
sni_find_item_by_window(Window win)
{
	SNIItem *item;

	if (!sni_watcher)
		return NULL;

	for (item = sni_watcher->items; item; item = item->next) {
		if (item->win == win)
			return item;
	}

	return NULL;
}

/* Handle click events on SNI icons */
void
sni_handle_click(Window win, int button, int x, int y, Time event_time)
{
	SNIItem     *item;
	DBusMessage *msg;
	const char  *method;

	item = sni_find_item_by_window(win);
	if (!item || !item->service || !item->path) {
		awm_debug("SNI: Click on unknown window 0x%lx", win);
		return;
	}

	/* Properties not yet fetched: queue the click and dispatch once ready */
	if (!item->properties_fetched) {
		awm_debug("SNI: Queuing click (button %d) for %s — properties pending",
		    button, item->service);
		item->pending_click  = 1;
		item->pending_button = button;
		item->pending_x      = x;
		item->pending_y      = y;
		item->pending_time   = event_time;
		return;
	}

	/* Determine which D-Bus method to call based on button */
	switch (button) {
	case Button1: /* Left click - Activate */
		method = "Activate";
		break;
	case Button2: /* Middle click - SecondaryActivate */
		method = "SecondaryActivate";
		break;
	case Button3: /* Right click - ContextMenu */
		method = "ContextMenu";
		break;
	default:
		return;
	}

	awm_debug("SNI: %s on %s at (%d,%d)", method, item->service, x, y);

	/* For right-click: show our DBusMenu if the app provides one,
	 * otherwise send ContextMenu and let the app render its own menu. */
	if (button == Button3) {
		if (item->menu_path) {
			awm_debug("SNI: Showing DBusMenu for %s", item->service);
			sni_show_menu(item, x, y, event_time);
			return;
		}
		awm_debug(
		    "SNI: No DBusMenu, sending ContextMenu to %s", item->service);
	}

	/* Send D-Bus method call for Activate/SecondaryActivate/ContextMenu */
	msg = dbus_message_new_method_call(item->service, /* destination */
	    item->path,                                   /* object path */
	    ITEM_INTERFACE,                               /* interface */
	    method);                                      /* method */

	if (!msg) {
		awm_error("SNI: Failed to create method call");
		return;
	}

	/* Append x, y coordinates as Int32 */
	dbus_message_append_args(
	    msg, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID);

	/* Send the message (no reply expected) */
	if (!dbus_connection_send(sni_watcher->conn, msg, NULL)) {
		awm_error("SNI: Failed to send %s", method);
	}

	dbus_connection_flush(sni_watcher->conn);
	dbus_message_unref(msg);
}

/* ============================================================================
 * DBusMenu Support
 * ============================================================================
 */

/* Callback for menu item activation */
static void
sni_menu_item_activated(int item_id, SNIItem *item)
{
	DBusMessage    *msg;
	const char     *event_type = "clicked";
	DBusMessageIter iter, variant_iter;
	dbus_uint32_t   timestamp;
	dbus_int32_t    data_dummy = 0; /* event data: empty INT32 variant */

	if (!item || !item->service || !item->menu_path)
		return;

	awm_debug("DBusMenu: Item %d clicked on %s", item_id, item->service);

	/* Call Event method on DBusMenu */
	msg = dbus_message_new_method_call(
	    item->service, item->menu_path, DBUSMENU_INTERFACE, "Event");
	if (!msg)
		return;

	/* DBusMenu Event signature: (id: INT32, eventId: STRING,
	 * data: VARIANT, timestamp: UINT32).  timestamp must be UINT32. */
	timestamp = (dbus_uint32_t) CurrentTime;

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &item_id);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &event_type);

	/* data variant: conventionally an empty INT32(0) for "clicked" */
	dbus_message_iter_open_container(
	    &iter, DBUS_TYPE_VARIANT, DBUS_TYPE_INT32_AS_STRING, &variant_iter);
	dbus_message_iter_append_basic(
	    &variant_iter, DBUS_TYPE_INT32, &data_dummy);
	dbus_message_iter_close_container(&iter, &variant_iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &timestamp);

	dbus_connection_send(sni_watcher->conn, msg, NULL);
	dbus_connection_flush(sni_watcher->conn);
	dbus_message_unref(msg);
}

/* Build menu structure from DBusMenu layout - returns MenuItem list */

/* Helper structure for parsing menu item properties */
struct MenuItemProperties {
	char          *label;
	int            enabled;
	int            visible;
	MenuToggleType toggle_type;
	int            toggle_state;
};

/*
 * Strip DBusMenu mnemonic underscores from a label in-place.
 * Per spec: "_X" -> "X" (mnemonic), "__" -> "_" (literal underscore).
 * The result is always <= the input length so no reallocation is needed.
 */
static void
sni_strip_mnemonics(char *s)
{
	char *r = s, *w = s;
	while (*r) {
		if (*r == '_') {
			r++;
			if (*r == '\0')
				break; /* trailing lone underscore: drop it */
			/* "__" -> "_", "_X" -> "X" */
			*w++ = (*r == '_') ? '_' : *r;
			r++;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

/* Callback for parsing menu item property dict */
static void
sni_parse_menu_property(
    const char *key, DBusMessageIter *value, void *user_data)
{
	struct MenuItemProperties *props = user_data;

	if (strcmp(key, "label") == 0) {
		props->label = dbus_iter_get_variant_string(value);
		if (props->label)
			sni_strip_mnemonics(props->label);
	} else if (strcmp(key, "enabled") == 0) {
		dbus_iter_get_variant_bool(value, &props->enabled);
	} else if (strcmp(key, "visible") == 0) {
		dbus_iter_get_variant_bool(value, &props->visible);
	} else if (strcmp(key, "toggle-type") == 0) {
		char *s = dbus_iter_get_variant_string(value);
		if (s) {
			if (strcmp(s, "checkmark") == 0)
				props->toggle_type = MENU_TOGGLE_CHECKMARK;
			else if (strcmp(s, "radio") == 0)
				props->toggle_type = MENU_TOGGLE_RADIO;
			free(s);
		}
	} else if (strcmp(key, "toggle-state") == 0) {
		/* toggle-state is an INT32: 0=off, 1=on, -1=indeterminate */
		DBusMessageIter inner = *value;
		if (dbus_message_iter_get_arg_type(&inner) == DBUS_TYPE_VARIANT)
			dbus_message_iter_recurse(&inner, &inner);
		if (dbus_message_iter_get_arg_type(&inner) == DBUS_TYPE_INT32) {
			dbus_int32_t v = 0;
			dbus_message_iter_get_basic(&inner, &v);
			props->toggle_state = (v == 1) ? 1 : 0;
		}
	}
}

static MenuItem *
sni_build_menu_from_layout(SNIItem *item, DBusMessageIter *iter, int depth)
{
	MenuItem       *head = NULL, *tail = NULL;
	DBusMessageIter array_iter, struct_iter;

	if (depth > 10) {
		awm_debug("DBusMenu: Max depth reached");
		return NULL; /* Prevent infinite recursion */
	}

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY) {
		awm_debug("DBusMenu: Expected ARRAY, got type %c at depth %d",
		    dbus_message_iter_get_arg_type(iter), depth);
		return NULL;
	}

	dbus_message_iter_recurse(iter, &array_iter);

	while (dbus_message_iter_get_arg_type(&array_iter) != DBUS_TYPE_INVALID) {
		dbus_int32_t    id;
		char           *label   = NULL;
		int             enabled = 1;
		int             visible = 1;
		MenuItem       *mi;
		DBusMessageIter item_iter;

		/* Check if we need to unwrap a variant */
		if (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_VARIANT) {
			dbus_message_iter_recurse(&array_iter, &item_iter);
		} else if (dbus_message_iter_get_arg_type(&array_iter) ==
		    DBUS_TYPE_STRUCT) {
			item_iter = array_iter;
		} else {
			dbus_message_iter_next(&array_iter);
			continue;
		}

		/* Now item_iter should point to a struct */
		if (dbus_message_iter_get_arg_type(&item_iter) != DBUS_TYPE_STRUCT) {
			dbus_message_iter_next(&array_iter);
			continue;
		}

		dbus_message_iter_recurse(&item_iter, &struct_iter);

		/* Get item ID */
		if (dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_INT32) {
			dbus_message_iter_get_basic(&struct_iter, &id);
			dbus_message_iter_next(&struct_iter);
		} else {
			dbus_message_iter_next(&array_iter);
			continue;
		}

		/* Parse properties dictionary using helper */
		struct MenuItemProperties props = { NULL, 1, 1, MENU_TOGGLE_NONE, 0 };
		if (dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_ARRAY) {
			DBusMessageIter dict_iter;
			dbus_message_iter_recurse(&struct_iter, &dict_iter);
			dbus_iter_parse_dict(&dict_iter, sni_parse_menu_property, &props);
			dbus_message_iter_next(&struct_iter);
		}

		label   = props.label;
		enabled = props.enabled;
		visible = props.visible;

		/* Skip if not visible */
		if (!visible) {
			free(label);
			dbus_message_iter_next(&array_iter);
			continue;
		}

		/* Create menu item - separator if no label */
		if (!label || strcmp(label, "") == 0) {
			mi = menu_separator_create();
		} else {
			mi = menu_item_create(id, label, enabled);
			if (mi) {
				mi->toggle_type  = props.toggle_type;
				mi->toggle_state = props.toggle_state;
			}
		}

		/* Free the label string now that menu_item_create has copied it */
		free(label);

		if (!mi) {
			dbus_message_iter_next(&array_iter);
			continue;
		}

		/* Check for submenu (children) */
		if (dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_ARRAY) {
			mi->submenu =
			    sni_build_menu_from_layout(item, &struct_iter, depth + 1);
		}

		/* Add to linked list */
		if (!head) {
			head = tail = mi;
		} else {
			tail->next = mi;
			tail       = mi;
		}

		dbus_message_iter_next(&array_iter);
	}

	return head;
}

/* Context passed to the async GetLayout reply callback */
typedef struct {
	SNIItem *item;
	int      x;
	int      y;
	Time     event_time;
} SNIMenuContext;

/* DBusPendingCall notify function for GetLayout reply */
static void
sni_get_layout_notify(DBusPendingCall *pending, void *user_data)
{
	SNIMenuContext *ctx = (SNIMenuContext *) user_data;
	SNIItem        *item;
	int             x, y;
	Time            event_time;
	DBusMessage    *reply;
	DBusMessageIter iter, struct_iter;
	MenuItem       *menu_items;

	reply = dbus_pending_call_steal_reply(pending);
	dbus_pending_call_unref(pending);

	if (!ctx)
		goto done;

	item       = ctx->item;
	x          = ctx->x;
	y          = ctx->y;
	event_time = ctx->event_time;
	free(ctx);

	if (!reply) {
		awm_error("DBusMenu: No reply to GetLayout");
		return;
	}

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		awm_error("DBusMenu: GetLayout failed: %s",
		    dbus_message_get_error_name(reply));
		goto done;
	}

	/* Parse reply: (uint revision, (id, props, children)) */
	dbus_message_iter_init(reply, &iter);

	/* Skip revision number */
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32)
		dbus_message_iter_next(&iter);

	/* Get root menu structure */
	if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRUCT) {
		dbus_message_iter_recurse(&iter, &struct_iter);

		/* Skip root ID and properties */
		dbus_message_iter_next(&struct_iter);
		dbus_message_iter_next(&struct_iter);

		/* Get children array */
		if (dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_ARRAY) {
			awm_debug("DBusMenu: Parsing menu items");
			menu_items = sni_build_menu_from_layout(item, &struct_iter, 0);

			if (menu_items) {
#ifdef AWM_DEBUG
				int count = menu_items_count(menu_items);
				awm_debug("DBusMenu: Built menu with %d items", count);
#endif

				/* Set items and show menu (menu_show handles monitor
				 * detection) */
				menu_set_items(sni_menu, menu_items);
				menu_show(
				    sni_menu, x, y, sni_menu_activated, item, event_time);

				awm_debug("DBusMenu: Menu shown");
			} else {
				awm_debug("DBusMenu: No menu items parsed");
			}
		} else {
			awm_debug("DBusMenu: Children is not an array (type=%c)",
			    dbus_message_iter_get_arg_type(&struct_iter));
		}
	} else {
		awm_debug("DBusMenu: Root layout is not a struct (type=%c)",
		    dbus_message_iter_get_arg_type(&iter));
	}

done:
	if (reply)
		dbus_message_unref(reply);
}

/* Show DBusMenu for an item — fully async, does not block the WM */
void
sni_show_menu(SNIItem *item, int x, int y, Time event_time)
{
	DBusMessage     *msg;
	DBusPendingCall *pending;
	SNIMenuContext  *ctx;
	dbus_int32_t     parent_id       = 0;
	dbus_int32_t     recursion_depth = -1; /* -1 = all levels */

	if (!item || !item->service || !item->menu_path || !sni_menu)
		return;

	awm_debug(
	    "DBusMenu: Fetching menu from %s%s", item->service, item->menu_path);

	/* Fire AboutToShow — fire-and-forget, no reply needed */
	msg = dbus_message_new_method_call(
	    item->service, item->menu_path, DBUSMENU_INTERFACE, "AboutToShow");
	if (msg) {
		dbus_message_append_args(
		    msg, DBUS_TYPE_INT32, &parent_id, DBUS_TYPE_INVALID);
		dbus_connection_send(sni_watcher->conn, msg, NULL);
		dbus_message_unref(msg);
	}

	/* Build GetLayout message with arguments */
	msg = dbus_message_new_method_call(
	    item->service, item->menu_path, DBUSMENU_INTERFACE, "GetLayout");
	if (!msg) {
		awm_debug("DBusMenu: Failed to create GetLayout message");
		return;
	}

	/* Arguments: parent_id (0 = root), recursion_depth (-1 = all),
	 * propertyNames (empty array) */
	{
		DBusMessageIter args, array_iter;

		dbus_message_iter_init_append(msg, &args);
		dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &parent_id);
		dbus_message_iter_append_basic(
		    &args, DBUS_TYPE_INT32, &recursion_depth);

		/* Empty array of strings means "all properties" */
		dbus_message_iter_open_container(
		    &args, DBUS_TYPE_ARRAY, "s", &array_iter);
		dbus_message_iter_close_container(&args, &array_iter);
	}

	ctx = malloc(sizeof(SNIMenuContext));
	if (!ctx) {
		dbus_message_unref(msg);
		return;
	}
	ctx->item       = item;
	ctx->x          = x;
	ctx->y          = y;
	ctx->event_time = event_time;

	/* Send with async reply — the pending call is dispatched via the D-Bus fd
	 * source already registered on the GLib main loop in awm.c:run(). */
	if (!dbus_connection_send_with_reply(
	        sni_watcher->conn, msg, &pending, -1)) {
		dbus_message_unref(msg);
		free(ctx);
		return;
	}
	dbus_message_unref(msg);

	if (!pending) {
		free(ctx);
		return;
	}

	if (!dbus_pending_call_set_notify(
	        pending, sni_get_layout_notify, ctx, NULL)) {
		dbus_pending_call_cancel(pending);
		dbus_pending_call_unref(pending);
		free(ctx);
	}
	/* pending is unref'd inside sni_get_layout_notify when reply arrives */
}

/* Public API for handling menu events from awm */
int
sni_handle_menu_event(XEvent *ev)
{
	if (!sni_menu)
		return 0;

	return menu_handle_event(sni_menu, ev);
}

#endif /* STATUSNOTIFIER */
