/* See LICENSE file for copyright and license details.
 *
 * Generic D-Bus helper functions
 */

#include <fcntl.h>

#include <stdlib.h>
#include <string.h>

#include "dbus.h"
#include "log.h"
#include "util.h"

#define PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

/* Configuration from awm config.h */
extern const unsigned int dbustimeout;

/* ============================================================================
 * Async Callback Management
 * ============================================================================
 */

typedef struct AsyncCallData {
	dbus_async_reply_callback callback;
	void                     *user_data;
} AsyncCallData;

static void
async_call_notify_function(DBusPendingCall *pending, void *user_data)
{
	AsyncCallData *data = user_data;
	DBusMessage   *reply;

	reply = dbus_pending_call_steal_reply(pending);
	if (reply) {
		if (data->callback)
			data->callback(reply, data->user_data);
		dbus_message_unref(reply);
	}

	dbus_pending_call_unref(pending);
	free(data);
}

/* Helper function to set up async callback data for a pending call
 * Returns 1 on success, 0 on failure (with cleanup already done) */
static int
setup_async_callback(DBusPendingCall *pending,
    dbus_async_reply_callback callback, void *user_data)
{
	AsyncCallData *data;

	if (!pending || !callback)
		return 0;

	data = malloc(sizeof(AsyncCallData));
	if (!data) {
		dbus_pending_call_cancel(pending);
		dbus_pending_call_unref(pending);
		return 0;
	}

	data->callback  = callback;
	data->user_data = user_data;

	if (!dbus_pending_call_set_notify(
	        pending, async_call_notify_function, data, NULL)) {
		free(data);
		dbus_pending_call_cancel(pending);
		dbus_pending_call_unref(pending);
		return 0;
	}

	return 1;
}

/* ============================================================================
 * Method Call Helpers
 * ============================================================================
 */

/* Generic D-Bus method call helper (BLOCKING - deprecated for WM use) */
DBusMessage *
dbus_helper_call_method(DBusConnection *conn, const char *service,
    const char *path, const char *interface, const char *method)
{
	DBusMessage *msg, *reply;
	DBusError    err;

	if (!conn)
		return NULL;

	msg = dbus_message_new_method_call(service, path, interface, method);
	if (!msg)
		return NULL;

	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(
	    conn, msg, dbustimeout, &err);
	dbus_message_unref(msg);

	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
		return NULL;
	}

	return reply;
}

/* Async method call with callback */
int
dbus_helper_call_method_async(DBusConnection *conn, const char *service,
    const char *path, const char *interface, const char *method,
    dbus_async_reply_callback callback, void *user_data)
{
	DBusMessage     *msg;
	DBusPendingCall *pending;

	if (!conn || !callback)
		return 0;

	msg = dbus_message_new_method_call(service, path, interface, method);
	if (!msg)
		return 0;

	if (!dbus_connection_send_with_reply(conn, msg, &pending, -1)) {
		dbus_message_unref(msg);
		return 0;
	}

	dbus_message_unref(msg);

	if (!pending)
		return 0;

	return setup_async_callback(pending, callback, user_data);
}

/* Get string property via org.freedesktop.DBus.Properties (BLOCKING) */
int
dbus_helper_get_property_string(DBusConnection *conn, const char *service,
    const char *path, const char *interface, const char *property,
    char **value)
{
	DBusMessage    *msg, *reply;
	DBusMessageIter args, variant;
	DBusError       err;
	const char     *str;

	if (!conn || !value)
		return 0;

	*value = NULL;

	msg = dbus_message_new_method_call(
	    service, path, PROPERTIES_INTERFACE, "Get");
	if (!msg)
		return 0;

	dbus_message_iter_init_append(msg, &args);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &interface);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &property);

	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(
	    conn, msg, dbustimeout, &err);
	dbus_message_unref(msg);

	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
		return 0;
	}

	if (!reply)
		return 0;

	if (dbus_message_iter_init(reply, &args) &&
	    dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_VARIANT) {
		int type;
		dbus_message_iter_recurse(&args, &variant);
		type = dbus_message_iter_get_arg_type(&variant);

		/* Accept both STRING and OBJECT_PATH types */
		if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
			dbus_message_iter_get_basic(&variant, &str);
			*value = strdup(str);
			if (!*value) {
				dbus_message_unref(reply);
				return 0;
			}
			dbus_message_unref(reply);
			return 1;
		}
	}

	dbus_message_unref(reply);
	return 0;
}

/* Async string property getter */
int
dbus_helper_get_property_string_async(DBusConnection *conn,
    const char *service, const char *path, const char *interface,
    const char *property, dbus_async_reply_callback callback, void *user_data)
{
	DBusMessage     *msg;
	DBusPendingCall *pending;
	DBusMessageIter  args;

	if (!conn || !callback)
		return 0;

	msg = dbus_message_new_method_call(
	    service, path, PROPERTIES_INTERFACE, "Get");
	if (!msg)
		return 0;

	dbus_message_iter_init_append(msg, &args);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &interface);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &property);

	if (!dbus_connection_send_with_reply(conn, msg, &pending, -1)) {
		dbus_message_unref(msg);
		return 0;
	}

	dbus_message_unref(msg);

	if (!pending)
		return 0;

	return setup_async_callback(pending, callback, user_data);
}

/* Get int32 property via org.freedesktop.DBus.Properties (BLOCKING) */
int
dbus_helper_get_property_int(DBusConnection *conn, const char *service,
    const char *path, const char *interface, const char *property, int *value)
{
	DBusMessage    *msg, *reply;
	DBusMessageIter args, variant;
	DBusError       err;
	dbus_int32_t    val;

	if (!conn || !value)
		return 0;

	msg = dbus_message_new_method_call(
	    service, path, PROPERTIES_INTERFACE, "Get");
	if (!msg)
		return 0;

	dbus_message_iter_init_append(msg, &args);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &interface);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &property);

	dbus_error_init(&err);
	reply = dbus_connection_send_with_reply_and_block(
	    conn, msg, dbustimeout, &err);
	dbus_message_unref(msg);

	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
		return 0;
	}

	if (!reply)
		return 0;

	if (dbus_message_iter_init(reply, &args) &&
	    dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&args, &variant);
		if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_INT32) {
			dbus_message_iter_get_basic(&variant, &val);
			*value = val;
			dbus_message_unref(reply);
			return 1;
		}
	}

	dbus_message_unref(reply);
	return 0;
}

/* Async int32 property getter */
int
dbus_helper_get_property_int_async(DBusConnection *conn, const char *service,
    const char *path, const char *interface, const char *property,
    dbus_async_reply_callback callback, void *user_data)
{
	DBusMessage     *msg;
	DBusPendingCall *pending;
	DBusMessageIter  args;

	if (!conn || !callback)
		return 0;

	msg = dbus_message_new_method_call(
	    service, path, PROPERTIES_INTERFACE, "Get");
	if (!msg)
		return 0;

	dbus_message_iter_init_append(msg, &args);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &interface);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &property);

	if (!dbus_connection_send_with_reply(conn, msg, &pending, -1)) {
		dbus_message_unref(msg);
		return 0;
	}

	dbus_message_unref(msg);

	if (!pending)
		return 0;

	return setup_async_callback(pending, callback, user_data);
}

/* Async GetAll properties - fetches all properties in one call
 * Reply is a{sv} (dict of string -> variant)
 * Much more efficient than multiple Get() calls
 */
int
dbus_helper_get_all_properties_async(DBusConnection *conn, const char *service,
    const char *path, const char *interface,
    dbus_async_reply_callback callback, void *user_data)
{
	DBusMessage     *msg;
	DBusPendingCall *pending;
	DBusMessageIter  args;

	if (!conn || !callback)
		return 0;

	msg = dbus_message_new_method_call(
	    service, path, PROPERTIES_INTERFACE, "GetAll");
	if (!msg)
		return 0;

	dbus_message_iter_init_append(msg, &args);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &interface);

	if (!dbus_connection_send_with_reply(conn, msg, &pending, -1)) {
		dbus_message_unref(msg);
		return 0;
	}

	dbus_message_unref(msg);

	if (!pending)
		return 0;

	return setup_async_callback(pending, callback, user_data);
}

/* ============================================================================
 * Iterator Unwrapping Helpers
 * ============================================================================
 */

int
dbus_iter_unwrap_variant(DBusMessageIter *iter, DBusMessageIter *variant)
{
	if (!iter || !variant)
		return 0;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_VARIANT)
		return 0;

	dbus_message_iter_recurse(iter, variant);
	return 1;
}

int
dbus_iter_recurse_array(DBusMessageIter *iter, DBusMessageIter *array)
{
	if (!iter || !array)
		return 0;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
		return 0;

	dbus_message_iter_recurse(iter, array);
	return 1;
}

int
dbus_iter_recurse_struct(DBusMessageIter *iter, DBusMessageIter *strct)
{
	if (!iter || !strct)
		return 0;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_STRUCT)
		return 0;

	dbus_message_iter_recurse(iter, strct);
	return 1;
}

int
dbus_iter_recurse_dict_entry(DBusMessageIter *iter, DBusMessageIter *entry)
{
	if (!iter || !entry)
		return 0;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_DICT_ENTRY)
		return 0;

	dbus_message_iter_recurse(iter, entry);
	return 1;
}

/* ============================================================================
 * Variant Value Extraction Helpers
 * ============================================================================
 */

char *
dbus_iter_get_variant_string(DBusMessageIter *variant)
{
	const char *str;
	int         type;

	if (!variant)
		return NULL;

	type = dbus_message_iter_get_arg_type(variant);

	/* Accept both STRING and OBJECT_PATH types */
	if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH)
		return NULL;

	dbus_message_iter_get_basic(variant, &str);
	return strdup(str);
}

int
dbus_iter_get_variant_bool(DBusMessageIter *variant, int *value)
{
	dbus_bool_t b;

	if (!variant || !value)
		return 0;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_BOOLEAN)
		return 0;

	dbus_message_iter_get_basic(variant, &b);
	*value = b ? 1 : 0;
	return 1;
}

int
dbus_iter_get_variant_int32(DBusMessageIter *variant, dbus_int32_t *value)
{
	if (!variant || !value)
		return 0;

	if (dbus_message_iter_get_arg_type(variant) != DBUS_TYPE_INT32)
		return 0;

	dbus_message_iter_get_basic(variant, value);
	return 1;
}

/* ============================================================================
 * Dictionary Parsing Helpers
 * ============================================================================
 */

void
dbus_iter_parse_dict(
    DBusMessageIter *dict, dbus_dict_entry_callback callback, void *user_data)
{
	DBusMessageIter entry_iter, value_iter;
	const char     *key;

	if (!dict || !callback)
		return;

	while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
		dbus_message_iter_recurse(dict, &entry_iter);

		/* Get key (always a string in a{sv}) */
		if (dbus_message_iter_get_arg_type(&entry_iter) != DBUS_TYPE_STRING)
			goto next;

		dbus_message_iter_get_basic(&entry_iter, &key);
		dbus_message_iter_next(&entry_iter);

		/* Get value (wrapped in variant) */
		if (dbus_message_iter_get_arg_type(&entry_iter) != DBUS_TYPE_VARIANT)
			goto next;

		dbus_message_iter_recurse(&entry_iter, &value_iter);

		/* Call callback with key and unwrapped value iterator */
		callback(key, &value_iter, user_data);

	next:
		dbus_message_iter_next(dict);
	}
}

/* ============================================================================
 * Signal Subscription Helper
 * ============================================================================
 */

int
dbus_helper_add_match(DBusConnection *conn, const char *match_rule)
{
	DBusMessage    *msg;
	DBusMessageIter args;
	dbus_bool_t     ok;

	if (!conn || !match_rule)
		return 0;

	msg = dbus_message_new_method_call("org.freedesktop.DBus",
	    "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch");
	if (!msg)
		return 0;

	dbus_message_iter_init_append(msg, &args);
	dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &match_rule);

	ok = dbus_connection_send(conn, msg, NULL);
	dbus_message_unref(msg);

	return ok ? 1 : 0;
}

/* ============================================================================
 * Connection Setup Helpers
 * ============================================================================
 */

DBusConnection *
dbus_helper_session_connect(const char *well_known_name,
    dbus_message_filter_func filter, void *filter_data, char **unique_name_out)
{
	DBusConnection *conn;
	DBusError       err;
	int             ret;
	int             fd;

	if (!filter)
		return NULL;

	dbus_error_init(&err);

	/* Connect to session bus using a private (non-shared) connection so that
	 * dbus_connection_close() + dbus_connection_unref() actually closes the
	 * socket and releases any well-known names.  dbus_bus_get() returns a
	 * process-wide singleton that libdbus keeps alive internally, meaning
	 * release_name + unref does not close the fd. */
	conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (dbus_error_is_set(&err)) {
		awm_error("D-Bus connection error: %s", err.message);
		dbus_error_free(&err);
		return NULL;
	}

	if (!conn)
		return NULL;

	/* Mark the D-Bus fd close-on-exec so it is not inherited by child
	 * processes or the new image after execvp.  Without this the old
	 * connection's fd survives exec and the bus keeps the well-known name
	 * alive, causing the next sni_init() to fail with "not primary owner". */
	if (dbus_connection_get_unix_fd(conn, &fd) && fd >= 0)
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);

	/* Get and store unique name if requested */
	if (unique_name_out) {
		const char *unique = dbus_bus_get_unique_name(conn);
		*unique_name_out   = unique ? strdup(unique) : NULL;
	}

	/* Set up event loop integration */
	dbus_connection_set_exit_on_disconnect(conn, FALSE);

	/* Add message filter */
	if (!dbus_connection_add_filter(conn, filter, filter_data, NULL)) {
		if (unique_name_out && *unique_name_out) {
			free(*unique_name_out);
			*unique_name_out = NULL;
		}
		dbus_connection_close(conn);
		dbus_connection_unref(conn);
		return NULL;
	}

	/* Request well-known name if provided */
	if (well_known_name) {
		ret = dbus_bus_request_name(conn, well_known_name,
		    DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE,
		    &err);
		if (dbus_error_is_set(&err)) {
			awm_error("Failed to register D-Bus name '%s': %s",
			    well_known_name, err.message);
			dbus_error_free(&err);
		}

		if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
		    ret != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
			awm_error("Failed to become primary owner of D-Bus name '%s' — "
			          "aborting connection",
			    well_known_name);
			if (unique_name_out && *unique_name_out) {
				free(*unique_name_out);
				*unique_name_out = NULL;
			}
			dbus_connection_close(conn);
			dbus_connection_unref(conn);
			return NULL;
		}
	}

	return conn;
}

DBusConnection *
dbus_helper_session_connect_dispatcher(const char *well_known_name,
    DBusDispatcher *dispatcher, char **unique_name_out)
{
	if (!dispatcher)
		return NULL;

	return dbus_helper_session_connect(
	    well_known_name, dbus_dispatcher_filter, dispatcher, unique_name_out);
}

/* ============================================================================
 * Message Reply Helpers
 * ============================================================================
 */

int
dbus_helper_send_reply(DBusConnection *conn, DBusMessage *msg)
{
	DBusMessage *reply;

	if (!conn || !msg)
		return 0;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return 0;

	dbus_connection_send(conn, reply, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(reply);

	return 1;
}

int
dbus_helper_send_error(DBusConnection *conn, DBusMessage *msg,
    const char *error_name, const char *error_message)
{
	DBusMessage *reply;

	if (!conn || !msg || !error_name)
		return 0;

	reply = dbus_message_new_error(msg, error_name, error_message);
	if (!reply)
		return 0;

	dbus_connection_send(conn, reply, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(reply);

	return 1;
}

/* ============================================================================
 * Signal Emission Helper
 * ============================================================================
 */

DBusMessage *
dbus_helper_create_signal(
    const char *path, const char *interface, const char *name)
{
	if (!path || !interface || !name)
		return NULL;

	return dbus_message_new_signal(path, interface, name);
}

/* ============================================================================
 * Message Type Checking Helpers
 * ============================================================================
 */

int
dbus_is_method_call(
    DBusMessage *msg, const char *interface, const char *method)
{
	if (!msg || !interface || !method)
		return 0;

	return dbus_message_is_method_call(msg, interface, method);
}

int
dbus_is_signal(DBusMessage *msg, const char *interface, const char *member)
{
	if (!msg || !interface || !member)
		return 0;

	return dbus_message_is_signal(msg, interface, member);
}

/* ============================================================================
 * Generic Message Dispatcher
 * ============================================================================
 */

/* Handler entry structures */
typedef struct MethodHandler {
	char                 *interface;
	char                 *method;
	dbus_method_handler   handler;
	void                 *user_data;
	struct MethodHandler *next;
} MethodHandler;

typedef struct SignalHandler {
	char                 *interface;
	char                 *member;
	dbus_signal_handler   handler;
	void                 *user_data;
	struct SignalHandler *next;
} SignalHandler;

struct DBusDispatcher {
	MethodHandler *methods;
	SignalHandler *signals;
};

DBusDispatcher *
dbus_dispatcher_new(void)
{
	DBusDispatcher *dispatcher = calloc(1, sizeof(DBusDispatcher));
	return dispatcher;
}

void
dbus_dispatcher_free(DBusDispatcher *dispatcher)
{
	MethodHandler *mh, *mh_next;
	SignalHandler *sh, *sh_next;

	if (!dispatcher)
		return;

	/* Free method handlers */
	for (mh = dispatcher->methods; mh; mh = mh_next) {
		mh_next = mh->next;
		free(mh->interface);
		free(mh->method);
		free(mh);
	}

	/* Free signal handlers */
	for (sh = dispatcher->signals; sh; sh = sh_next) {
		sh_next = sh->next;
		free(sh->interface);
		free(sh->member);
		free(sh);
	}

	free(dispatcher);
}

int
dbus_dispatcher_register_method(DBusDispatcher *dispatcher,
    const char *interface, const char *method, dbus_method_handler handler,
    void *user_data)
{
	MethodHandler *mh;

	if (!dispatcher || !interface || !method || !handler)
		return 0;

	mh = calloc(1, sizeof(MethodHandler));
	if (!mh)
		return 0;

	mh->interface = strdup(interface);
	mh->method    = strdup(method);
	if (!mh->interface || !mh->method) {
		free(mh->interface);
		free(mh->method);
		free(mh);
		return 0;
	}
	mh->handler   = handler;
	mh->user_data = user_data;

	/* Add to front of list */
	mh->next            = dispatcher->methods;
	dispatcher->methods = mh;

	return 1;
}

int
dbus_dispatcher_register_signal(DBusDispatcher *dispatcher,
    const char *interface, const char *member, dbus_signal_handler handler,
    void *user_data)
{
	SignalHandler *sh;

	if (!dispatcher || !interface || !member || !handler)
		return 0;

	sh = calloc(1, sizeof(SignalHandler));
	if (!sh)
		return 0;

	sh->interface = strdup(interface);
	sh->member    = strdup(member);
	if (!sh->interface || !sh->member) {
		free(sh->interface);
		free(sh->member);
		free(sh);
		return 0;
	}
	sh->handler   = handler;
	sh->user_data = user_data;

	/* Add to front of list */
	sh->next            = dispatcher->signals;
	dispatcher->signals = sh;

	return 1;
}

void
dbus_dispatcher_unregister_method(
    DBusDispatcher *dispatcher, const char *interface, const char *method)
{
	MethodHandler *mh, *prev = NULL;

	if (!dispatcher || !interface || !method)
		return;

	for (mh = dispatcher->methods; mh; prev = mh, mh = mh->next) {
		if (strcmp(mh->interface, interface) == 0 &&
		    strcmp(mh->method, method) == 0) {
			if (prev)
				prev->next = mh->next;
			else
				dispatcher->methods = mh->next;

			free(mh->interface);
			free(mh->method);
			free(mh);
			return;
		}
	}
}

void
dbus_dispatcher_unregister_signal(
    DBusDispatcher *dispatcher, const char *interface, const char *member)
{
	SignalHandler *sh, *prev = NULL;

	if (!dispatcher || !interface || !member)
		return;

	for (sh = dispatcher->signals; sh; prev = sh, sh = sh->next) {
		if (strcmp(sh->interface, interface) == 0 &&
		    strcmp(sh->member, member) == 0) {
			if (prev)
				prev->next = sh->next;
			else
				dispatcher->signals = sh->next;

			free(sh->interface);
			free(sh->member);
			free(sh);
			return;
		}
	}
}

DBusHandlerResult
dbus_dispatcher_dispatch(
    DBusDispatcher *dispatcher, DBusConnection *conn, DBusMessage *msg)
{
	const char *interface, *member;
	int         msg_type;

	if (!dispatcher || !conn || !msg)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	interface = dbus_message_get_interface(msg);
	member    = dbus_message_get_member(msg);
	msg_type  = dbus_message_get_type(msg);

	if (!interface || !member)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	/* Try method handlers */
	if (msg_type == DBUS_MESSAGE_TYPE_METHOD_CALL) {
		MethodHandler *mh;
		for (mh = dispatcher->methods; mh; mh = mh->next) {
			if (strcmp(mh->interface, interface) == 0 &&
			    strcmp(mh->method, member) == 0) {
				return mh->handler(conn, msg, mh->user_data);
			}
		}
	}
	/* Try signal handlers */
	else if (msg_type == DBUS_MESSAGE_TYPE_SIGNAL) {
		SignalHandler *sh;
		for (sh = dispatcher->signals; sh; sh = sh->next) {
			if (strcmp(sh->interface, interface) == 0 &&
			    strcmp(sh->member, member) == 0) {
				return sh->handler(conn, msg, sh->user_data);
			}
		}
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

DBusHandlerResult
dbus_dispatcher_filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
	DBusDispatcher *dispatcher = user_data;
	return dbus_dispatcher_dispatch(dispatcher, conn, msg);
}
