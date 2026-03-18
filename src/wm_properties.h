/* See LICENSE file for copyright and license details. */

/* -------------------------------------------------------------------------
 * wm_properties.h — narrow backend-agnostic EWMH/ICCCM interface
 *
 * Core WM logic calls these functions instead of the X11-specific ewmh.c
 * helpers directly.  On X11 they forward to ewmh.c.  On future non-X11
 * backends they will be no-ops or xdg-shell equivalents.
 * ---------------------------------------------------------------------- */

#ifndef WM_PROPERTIES_H
#define WM_PROPERTIES_H

#include "awm.h"

/* Initialise desktop/viewport/tag property atoms (call once from setup()). */
void wmprop_setup(void);

/* Focus: tell the root window which client is active. */
void wmprop_set_focus(Client *c);

/* Client list: rebuild _NET_CLIENT_LIST and _NET_CLIENT_LIST_STACKING. */
void wmprop_update_client_list(void);

/* Desktop numbering: push current tag index to _NET_CURRENT_DESKTOP. */
void wmprop_update_current_desktop(void);

/* Per-client _NET_WM_STATE (fullscreen, hidden, demands-attention). */
void wmprop_set_wm_state(Client *c);

/* Per-client _NET_WM_DESKTOP. */
void wmprop_set_client_desktop(Client *c);

/* Workarea: update _NET_WORKAREA for monitor m. */
void wmprop_update_workarea(Monitor *m);

/* Send an ICCCM client-message (WMDelete / WMTakeFocus).
 * Returns non-zero if the protocol was supported by the window. */
int wmprop_send_event(xcb_window_t w, xcb_atom_t proto, int mask, long d0,
    long d1, long d2, long d3, long d4);

#endif /* WM_PROPERTIES_H */
