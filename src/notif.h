/* See LICENSE file for copyright and license details.
 *
 * notif.h — org.freedesktop.Notifications daemon for awm-ui
 *
 * Provides desktop notification popups via the standard D-Bus
 * Notifications interface.  Compiled into awm-ui only.
 */

#ifndef NOTIF_H
#define NOTIF_H

/* Initialise the notification daemon.
 * Registers org.freedesktop.Notifications on the session bus and creates
 * the persistent (initially empty) popup stack.
 * mon_wx/wy/ww/wh describe the primary monitor workarea for popup placement.
 * Returns 0 on success, -1 on failure. */
int notif_init(int mon_wx, int mon_wy, int mon_ww, int mon_wh);

/* Update the monitor workarea used for popup placement.
 * Call whenever a UI_MSG_MONITOR_GEOM message is received. */
void notif_update_geom(int mon_wx, int mon_wy, int mon_ww, int mon_wh);

/* Pump D-Bus dispatching.
 * Must be called periodically (e.g. from a GLib idle/timer source, or
 * from the GMainLoop if dbus_connection_setup_with_g_main was used). */
void notif_dispatch(void);

/* Tear down: close popups, release the D-Bus name, free all state. */
void notif_cleanup(void);

#endif /* NOTIF_H */
