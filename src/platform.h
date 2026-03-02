/* See LICENSE file for copyright and license details. */
/* platform.h — X11 platform context and WM backend vtable.
 *
 * This header is included from awm.h after the atom enums (WMLast,
 * NetLast, XLast) are declared.  Do not include it directly before
 * those enums are visible.
 *
 * PlatformCtx consolidates every X11 connection-level global that was
 * previously scattered as bare extern declarations in awm.h / awm.c.
 * A single instance, g_plat, is defined in platform_x11.c and is the
 * sole source of truth for all X11 connection state.
 *
 * WmBackend is a vtable of function pointers that abstracts every
 * X11 operation performed by the core WM modules (client.c,
 * monitor.c, events.c).  The sole X11 implementation is
 * wm_backend_x11, defined in platform_x11.c and pointed to by
 * g_wm_backend after setup() runs.
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#ifdef XRANDR
#include <xcb/randr.h>
#endif

#ifdef BACKEND_X11

/* -------------------------------------------------------------------------
 * PlatformCtx — X11 connection state singleton
 * ---------------------------------------------------------------------- */
typedef struct {
	xcb_connection_t  *xc;
	xcb_window_t       root;
	xcb_window_t       wmcheckwin;
	int                screen;
	int                sw, sh; /* screen geometry in pixels */
	int                bh;     /* bar height */
	int                lrpad;  /* left+right text padding */
	xcb_atom_t         wmatom[WMLast];
	xcb_atom_t         netatom[NetLast];
	xcb_atom_t         xatom[XLast];
	xcb_atom_t         utf8string_atom;
	xcb_key_symbols_t *keysyms;
	unsigned int       numlockmask;
	double             ui_dpi;      /* resolved screen DPI (96.0 default) */
	double             ui_scale;    /* ui_dpi / 96.0 */
	unsigned int       ui_borderpx; /* borderpx * ui_scale */
	unsigned int       ui_snap;     /* snap     * ui_scale */
	unsigned int       ui_iconsize; /* iconsize * ui_scale */
	unsigned int       ui_gappx;    /* gappx[0] * ui_scale */
#ifdef XRANDR
	int randrbase, rrerrbase;
#endif
} PlatformCtx;

extern PlatformCtx g_plat;

/* -------------------------------------------------------------------------
 * WmBackend — vtable abstracting all X11 WM operations
 * ---------------------------------------------------------------------- */

/*
 * Forward declarations for types used in the vtable signatures that may
 * not be defined yet at this point in the include chain.  These are
 * opaque from the vtable's perspective; callers know the full types.
 */
struct _Client;
struct _Monitor;

typedef struct {
	/* --- connection ---------------------------------------------------- */
	void (*flush)(PlatformCtx *p);

	/* --- window geometry & stacking ------------------------------------ */
	/* General-purpose xcb_configure_window wrapper.  mask and vals must
	 * be ordered by ascending XCB_CONFIG_WINDOW_* bit position. */
	void (*configure_win)(
	    PlatformCtx *p, xcb_window_t w, uint16_t mask, const uint32_t *vals);

	/* --- window attributes --------------------------------------------- */
	void (*change_attr)(
	    PlatformCtx *p, xcb_window_t w, uint32_t mask, const uint32_t *vals);

	/* --- window visibility --------------------------------------------- */
	void (*map)(PlatformCtx *p, xcb_window_t w);
	void (*unmap)(PlatformCtx *p, xcb_window_t w);
	void (*destroy_win)(PlatformCtx *p, xcb_window_t w);

	/* --- window events ------------------------------------------------- */
	/* Send a synthetic XCB_CONFIGURE_NOTIFY for a managed client. */
	void (*send_configure_notify)(
	    PlatformCtx *p, xcb_window_t w, int x, int y, int bw, int ww, int wh);

	/* --- focus & input ------------------------------------------------- */
	void (*set_input_focus)(PlatformCtx *p, xcb_window_t w, xcb_timestamp_t t);
	void (*warp_pointer)(
	    PlatformCtx *p, xcb_window_t dst, int16_t x, int16_t y);
	/* Allow/replay pointer events (xcb_allow_events). */
	void (*allow_events)(PlatformCtx *p, int mode, xcb_timestamp_t t);

	/* --- pointer grab -------------------------------------------------- */
	/* Returns 1 on success, 0 on failure. */
	int (*grab_pointer)(PlatformCtx *p, xcb_cursor_t cursor);
	void (*ungrab_pointer)(PlatformCtx *p);
	void (*ungrab_button)(PlatformCtx *p, xcb_window_t w);
	/* Query root pointer position.  Returns 1 on success. */
	int (*query_pointer)(PlatformCtx *p, int *x, int *y);

	/* --- server grab --------------------------------------------------- */
	void (*grab_server)(PlatformCtx *p);
	void (*ungrab_server)(PlatformCtx *p);

	/* --- window attributes query --------------------------------------- */
	/* Fetch override_redirect flag.  Returns 1 on success. */
	int (*get_override_redirect)(
	    PlatformCtx *p, xcb_window_t w, int *override_redirect_out);
	/* Fetch current event mask for a window.  Returns 1 on success. */
	int (*get_event_mask)(
	    PlatformCtx *p, xcb_window_t w, uint32_t *event_mask_out);

	/* --- window geometry ----------------------------------------------- */
	/* Fetch geometry.  Returns 1 on success. */
	int (*get_geometry)(PlatformCtx *p, xcb_window_t w, int *x, int *y,
	    int *ww, int *wh, int *bw);

	/* --- window tree --------------------------------------------------- */
	/* Walk window parents up to root to check ancestry. */
	int (*is_window_descendant)(
	    PlatformCtx *p, xcb_window_t w, xcb_window_t ancestor);

	/* --- property reads ------------------------------------------------ */
	/* Read a single-atom property, returning XCB_ATOM_NONE on failure. */
	xcb_atom_t (*get_atom_prop)(
	    PlatformCtx *p, xcb_window_t w, xcb_atom_t prop, xcb_atom_t type);
	/* Read a text property into buf[size].  Returns 1 on success. */
	int (*get_text_prop)(PlatformCtx *p, xcb_window_t w, xcb_atom_t atom,
	    char *buf, unsigned int size);
	/* Read _NET_WM_ICON raw pixel data.  Returns 1 on success; caller
	 * must free(*data_out) when done. */
	int (*get_wm_icon)(
	    PlatformCtx *p, xcb_window_t w, uint32_t **data_out, int *nitems_out);
	/* Read WM_CLASS (instance + class fields).  Returns 1 on success. */
	int (*get_wm_class)(PlatformCtx *p, xcb_window_t w, char *inst_buf,
	    unsigned int inst_size, char *cls_buf, unsigned int cls_size);

	/* --- property writes ----------------------------------------------- */
	void (*change_prop)(PlatformCtx *p, xcb_window_t w, xcb_atom_t prop,
	    xcb_atom_t type, int format, uint8_t mode, int n, const void *data);

	void (*delete_prop)(PlatformCtx *p, xcb_window_t w, xcb_atom_t prop);

	/* --- kill / close -------------------------------------------------- */
	void (*kill_client_hard)(PlatformCtx *p, xcb_window_t w);

	/* --- save-set / reparent (systray) --------------------------------- */
	void (*change_save_set)(PlatformCtx *p, xcb_window_t w, int insert);
	void (*reparent_window)(
	    PlatformCtx *p, xcb_window_t w, xcb_window_t parent, int x, int y);

	/* --- bar window creation ------------------------------------------- */
	/* Creates the per-monitor bar window and returns its XID.
	 * compositor_active selects between back_pixel and back_pixmap. */
	xcb_window_t (*create_bar_win)(
	    PlatformCtx *p, int x, int y, int w, int h, int compositor_active);
	/* Screen depth helper (xcb_screen_root_depth). */
	int (*screen_depth)(PlatformCtx *p);

	/* --- ICCCM helpers ------------------------------------------------- */
	int (*get_wm_normal_hints)(
	    PlatformCtx *p, xcb_window_t w, xcb_size_hints_t *out);
	/* Returns 1 on success, populates *out. */
	int (*get_wm_hints)(
	    PlatformCtx *p, xcb_window_t w, xcb_icccm_wm_hints_t *out);
	void (*set_wm_hints)(
	    PlatformCtx *p, xcb_window_t w, const xcb_icccm_wm_hints_t *hints);
	/* Returns XCB_WINDOW_NONE if no transient-for is set. */
	xcb_window_t (*get_wm_transient_for)(PlatformCtx *p, xcb_window_t w);

	/* --- compound / higher-level operations ---------------------------- */
	/* Grab/ungrab buttons for a client window.
	 * focused=1: per-button config grabs; focused=0: any-button grab. */
	void (*grab_buttons)(PlatformCtx *p, xcb_window_t w, int focused);

	/* Recompute and cache g_plat.numlockmask. */
	void (*update_numlock_mask)(PlatformCtx *p);

	/* Ungrab all keys then re-grab from the keys[] config array. */
	void (*grab_keys_full)(PlatformCtx *p);

	/* Refresh key symbol map after a MappingNotify. */
	void (*refresh_keyboard_mapping)(
	    PlatformCtx *p, xcb_mapping_notify_event_t *ev);

	/* Look up keysym for a keycode (col=0 unshifted). */
	xcb_keysym_t (*get_keysym)(PlatformCtx *p, xcb_keycode_t code, int col);

	/* Probe for another WM (SubstructureRedirect).  Calls die() if one
	 * is found.  Safe to call before g_plat.root is set. */
	void (*check_other_wm)(PlatformCtx *p);

	/* Update monitor geometry from RandR/Xinerama.  Returns 1 if dirty. */
	int (*update_geom)(PlatformCtx *p);

	/* Event loop: poll and wait variants. */
	xcb_generic_event_t *(*poll_event)(PlatformCtx *p);
	xcb_generic_event_t *(*next_event)(PlatformCtx *p);
} WmBackend;

extern WmBackend *g_wm_backend;
extern WmBackend  wm_backend_x11;

#endif /* BACKEND_X11 */

#endif /* PLATFORM_H */
