/* See LICENSE file for copyright and license details.
 *
 * Generic D-Bus helper functions
 */

#ifndef DBUS_HELPERS_H
#define DBUS_HELPERS_H

#include <dbus/dbus.h>

/* Generic D-Bus method call helper (BLOCKING - use async version for WM) 
 * WARNING: This function blocks for up to DBUS_DEFAULT_TIMEOUT_MS (100ms).
 * For window managers, prefer dbus_helper_call_method_async() to avoid UI freezes.
 */
DBusMessage *dbus_helper_call_method(DBusConnection *conn,
				     const char *service,
				     const char *path,
				     const char *interface,
				     const char *method);

/* Async method call with callback */
typedef void (*dbus_async_reply_callback)(DBusMessage *reply, void *user_data);

int dbus_helper_call_method_async(DBusConnection *conn,
				  const char *service,
				  const char *path,
				  const char *interface,
				  const char *method,
				  dbus_async_reply_callback callback,
				  void *user_data);

/* Get string property via org.freedesktop.DBus.Properties (BLOCKING) */
int dbus_helper_get_property_string(DBusConnection *conn,
				    const char *service,
				    const char *path,
				    const char *interface,
				    const char *property,
				    char **value);

/* Get string property async */
int dbus_helper_get_property_string_async(DBusConnection *conn,
					  const char *service,
					  const char *path,
					  const char *interface,
					  const char *property,
					  dbus_async_reply_callback callback,
					  void *user_data);

/* Get int32 property via org.freedesktop.DBus.Properties (BLOCKING) */
int dbus_helper_get_property_int(DBusConnection *conn,
				 const char *service,
				 const char *path,
				 const char *interface,
				 const char *property,
				 int *value);

/* Get all properties async via GetAll - returns a{sv} dict */
int dbus_helper_get_all_properties_async(DBusConnection *conn,
					const char *service,
					const char *path,
					const char *interface,
					dbus_async_reply_callback callback,
					void *user_data);

/* Get int32 property async */
int dbus_helper_get_property_int_async(DBusConnection *conn,
				       const char *service,
				       const char *path,
				       const char *interface,
				       const char *property,
				       dbus_async_reply_callback callback,
				       void *user_data);

/* Iterator unwrapping helpers */
/* Unwrap a DBUS_TYPE_VARIANT - returns 1 on success, 0 on failure */
int dbus_iter_unwrap_variant(DBusMessageIter *iter, DBusMessageIter *variant);

/* Recurse into DBUS_TYPE_ARRAY - returns 1 on success, 0 on failure */
int dbus_iter_recurse_array(DBusMessageIter *iter, DBusMessageIter *array);

/* Recurse into DBUS_TYPE_STRUCT - returns 1 on success, 0 on failure */
int dbus_iter_recurse_struct(DBusMessageIter *iter, DBusMessageIter *strct);

/* Recurse into DBUS_TYPE_DICT_ENTRY - returns 1 on success, 0 on failure */
int dbus_iter_recurse_dict_entry(DBusMessageIter *iter, DBusMessageIter *entry);

/* Get string from variant (caller must free returned string) */
char *dbus_iter_get_variant_string(DBusMessageIter *variant);

/* Get boolean from variant */
int dbus_iter_get_variant_bool(DBusMessageIter *variant, int *value);

/* Get int32 from variant */
int dbus_iter_get_variant_int32(DBusMessageIter *variant, dbus_int32_t *value);

/* Parse a dictionary (a{sv}) and call callback for each entry */
typedef void (*dbus_dict_entry_callback)(const char *key,
		DBusMessageIter *value, void *user_data);
void dbus_iter_parse_dict(DBusMessageIter *dict,
			  dbus_dict_entry_callback callback, void *user_data);

/* Signal subscription helper */
int dbus_helper_add_match(DBusConnection *conn, const char *match_rule);

/* ============================================================================
 * Generic Message Dispatcher
 * ============================================================================ */

/* Forward declaration */
typedef struct DBusDispatcher DBusDispatcher;

/* Connection setup helpers */
/* Initialize connection to session bus and set up filter */
typedef DBusHandlerResult (*dbus_message_filter_func)(DBusConnection *conn,
		DBusMessage *msg,
		void *user_data);

DBusConnection *dbus_helper_session_connect(const char *well_known_name,
		dbus_message_filter_func filter,
		void *filter_data,
		char **unique_name_out);

/* Dispatcher-based connection (simpler API for most use cases) */
DBusConnection *dbus_helper_session_connect_dispatcher(const char
		*well_known_name,
		DBusDispatcher *dispatcher,
		char **unique_name_out);

/* Message reply helpers */
/* Send empty method return reply */
int dbus_helper_send_reply(DBusConnection *conn, DBusMessage *msg);

/* Send error reply */
int dbus_helper_send_error(DBusConnection *conn, DBusMessage *msg,
			   const char *error_name, const char *error_message);

/* Signal emission helper */
DBusMessage *dbus_helper_create_signal(const char *path,
				       const char *interface,
				       const char *name);

/* Check if message is a method call */
int dbus_is_method_call(DBusMessage *msg, const char *interface,
			const char *method);

/* Check if message is a signal */
int dbus_is_signal(DBusMessage *msg, const char *interface, const char *member);

/* Handler callback types */
typedef DBusHandlerResult (*dbus_method_handler)(DBusConnection *conn,
		DBusMessage *msg,
		void *user_data);

typedef DBusHandlerResult (*dbus_signal_handler)(DBusConnection *conn,
		DBusMessage *msg,
		void *user_data);

/* Create a new message dispatcher */
DBusDispatcher *dbus_dispatcher_new(void);

/* Free a dispatcher and all registered handlers */
void dbus_dispatcher_free(DBusDispatcher *dispatcher);

/* Register a method call handler */
int dbus_dispatcher_register_method(DBusDispatcher *dispatcher,
				    const char *interface,
				    const char *method,
				    dbus_method_handler handler,
				    void *user_data);

/* Register a signal handler */
int dbus_dispatcher_register_signal(DBusDispatcher *dispatcher,
				    const char *interface,
				    const char *member,
				    dbus_signal_handler handler,
				    void *user_data);

/* Unregister a method handler */
void dbus_dispatcher_unregister_method(DBusDispatcher *dispatcher,
				       const char *interface,
				       const char *method);

/* Unregister a signal handler */
void dbus_dispatcher_unregister_signal(DBusDispatcher *dispatcher,
				       const char *interface,
				       const char *member);

/* Dispatch a message (call from DBus filter function) */
DBusHandlerResult dbus_dispatcher_dispatch(DBusDispatcher *dispatcher,
		DBusConnection *conn,
		DBusMessage *msg);

/* Filter function that uses dispatcher */
DBusHandlerResult dbus_dispatcher_filter(DBusConnection *conn,
		DBusMessage *msg,
		void *user_data);

#endif /* DBUS_HELPERS_H */
