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
#include "wm_types.h"

#ifdef BACKEND_X11
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#ifdef XRANDR
#include <xcb/randr.h>
#endif
#endif /* BACKEND_X11 */

/* -------------------------------------------------------------------------
 * PlatformCtx — platform connection state singleton.
 * Common fields are always present.  X11-specific fields are guarded.
 * ---------------------------------------------------------------------- */
typedef struct {
	/* --- common fields (all backends) ---------------------------------- */
	int          sw, sh; /* screen geometry in pixels */
	int          bh;     /* bar height */
	int          lrpad;  /* left+right text padding */
	WinId        root;
	WinId        wmcheckwin;
	AtomId       wmatom[WMLast];
	AtomId       netatom[NetLast];
	AtomId       xatom[XLast];
	AtomId       utf8string_atom;
	unsigned int numlockmask;
	double       ui_dpi;      /* resolved screen DPI (96.0 default) */
	double       ui_scale;    /* ui_dpi / 96.0 */
	unsigned int ui_borderpx; /* borderpx * ui_scale */
	unsigned int ui_snap;     /* snap     * ui_scale */
	unsigned int ui_iconsize; /* iconsize * ui_scale */
	unsigned int ui_gappx;    /* gappx[0] * ui_scale */
#ifdef BACKEND_X11
	/* --- X11-specific fields ------------------------------------------ */
	xcb_connection_t  *xc;
	int                screen;
	xcb_key_symbols_t *keysyms;
#ifdef XRANDR
	int randrbase, rrerrbase;
#endif
#endif /* BACKEND_X11 */
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
	/* General-purpose configure_window wrapper.  mask and vals must
	 * be ordered by ascending AWM_CONFIG_WIN_* bit position. */
	void (*configure_win)(
	    PlatformCtx *p, WinId w, uint16_t mask, const uint32_t *vals);

	/* --- window attributes --------------------------------------------- */
	void (*change_attr)(
	    PlatformCtx *p, WinId w, uint32_t mask, const uint32_t *vals);

	/* --- window visibility --------------------------------------------- */
	void (*map)(PlatformCtx *p, WinId w);
	void (*unmap)(PlatformCtx *p, WinId w);
	void (*destroy_win)(PlatformCtx *p, WinId w);

	/* --- window events ------------------------------------------------- */
	/* Send a synthetic CONFIGURE_NOTIFY for a managed client. */
	void (*send_configure_notify)(
	    PlatformCtx *p, WinId w, int x, int y, int bw, int ww, int wh);

	/* --- focus & input ------------------------------------------------- */
	void (*set_input_focus)(PlatformCtx *p, WinId w, WmTimestamp t);
	void (*warp_pointer)(PlatformCtx *p, WinId dst, int16_t x, int16_t y);
	/* Allow/replay pointer events. */
	void (*allow_events)(PlatformCtx *p, int mode, WmTimestamp t);

	/* --- pointer grab -------------------------------------------------- */
	/* Returns 1 on success, 0 on failure. */
	int (*grab_pointer)(PlatformCtx *p, WmCursor cursor);
	void (*ungrab_pointer)(PlatformCtx *p);
	void (*ungrab_button)(PlatformCtx *p, WinId w);
	/* Query root pointer position.  Returns 1 on success. */
	int (*query_pointer)(PlatformCtx *p, int *x, int *y);

	/* --- server grab --------------------------------------------------- */
	void (*grab_server)(PlatformCtx *p);
	void (*ungrab_server)(PlatformCtx *p);

	/* --- window attributes query --------------------------------------- */
	/* Fetch override_redirect flag.  Returns 1 on success. */
	int (*get_override_redirect)(
	    PlatformCtx *p, WinId w, int *override_redirect_out);
	/* Fetch current event mask for a window.  Returns 1 on success. */
	int (*get_event_mask)(PlatformCtx *p, WinId w, uint32_t *event_mask_out);

	/* --- window geometry ----------------------------------------------- */
	/* Fetch geometry.  Returns 1 on success. */
	int (*get_geometry)(
	    PlatformCtx *p, WinId w, int *x, int *y, int *ww, int *wh, int *bw);

	/* --- window tree --------------------------------------------------- */
	/* Walk window parents up to root to check ancestry. */
	int (*is_window_descendant)(PlatformCtx *p, WinId w, WinId ancestor);

	/* --- property reads ------------------------------------------------ */
	/* Read a single-atom property, returning ATOM_NONE on failure. */
	AtomId (*get_atom_prop)(PlatformCtx *p, WinId w, AtomId prop, AtomId type);
	/* Read a text property into buf[size].  Returns 1 on success. */
	int (*get_text_prop)(
	    PlatformCtx *p, WinId w, AtomId atom, char *buf, unsigned int size);
	/* Read _NET_WM_ICON raw pixel data.  Returns 1 on success; caller
	 * must free(*data_out) when done. */
	int (*get_wm_icon)(
	    PlatformCtx *p, WinId w, uint32_t **data_out, int *nitems_out);
	/* Read WM_CLASS (instance + class fields).  Returns 1 on success. */
	int (*get_wm_class)(PlatformCtx *p, WinId w, char *inst_buf,
	    unsigned int inst_size, char *cls_buf, unsigned int cls_size);

	/* --- property writes ----------------------------------------------- */
	void (*change_prop)(PlatformCtx *p, WinId w, AtomId prop, AtomId type,
	    int format, uint8_t mode, int n, const void *data);

	void (*delete_prop)(PlatformCtx *p, WinId w, AtomId prop);

	/* --- kill / close -------------------------------------------------- */
	void (*kill_client_hard)(PlatformCtx *p, WinId w);

	/* --- save-set / reparent (systray) --------------------------------- */
	void (*change_save_set)(PlatformCtx *p, WinId w, int insert);
	void (*reparent_window)(
	    PlatformCtx *p, WinId w, WinId parent, int x, int y);

	/* --- bar window creation ------------------------------------------- */
	/* Creates the per-monitor bar window and returns its handle.
	 * compositor_active selects between back_pixel and back_pixmap. */
	WinId (*create_bar_win)(
	    PlatformCtx *p, int x, int y, int w, int h, int compositor_active);
	/* Screen depth helper. */
	int (*screen_depth)(PlatformCtx *p);

	/* --- ICCCM helpers ------------------------------------------------- */
	int (*get_wm_normal_hints)(PlatformCtx *p, WinId w, AwmSizeHints *out);
	/* Returns 1 on success, populates *out. */
	int (*get_wm_hints)(PlatformCtx *p, WinId w, AwmWmHints *out);
	void (*set_wm_hints)(PlatformCtx *p, WinId w, const AwmWmHints *hints);
	/* Returns WIN_NONE if no transient-for is set. */
	WinId (*get_wm_transient_for)(PlatformCtx *p, WinId w);

	/* --- compound / higher-level operations ---------------------------- */
	/* Grab/ungrab buttons for a client window.
	 * focused=1: per-button config grabs; focused=0: any-button grab. */
	void (*grab_buttons)(PlatformCtx *p, WinId w, int focused);

	/* Recompute and cache g_plat.numlockmask. */
	void (*update_numlock_mask)(PlatformCtx *p);

	/* Ungrab all keys then re-grab from the keys[] config array. */
	void (*grab_keys_full)(PlatformCtx *p);

	/* Refresh key symbol map after a MappingNotify. */
	void (*refresh_keyboard_mapping)(PlatformCtx *p, AwmEvent *ev);

	/* Look up keysym for a keycode (col=0 unshifted). */
	KeySym (*get_keysym)(PlatformCtx *p, WmKeycode code, int col);

	/* Probe for another WM (SubstructureRedirect).  Calls die() if one
	 * is found.  Safe to call before g_plat.root is set. */
	void (*check_other_wm)(PlatformCtx *p);

	/* Update monitor geometry from RandR/Xinerama.  Returns 1 if dirty. */
	int (*update_geom)(PlatformCtx *p);

	/* Event loop: poll and wait variants.  Returns NULL when no event. */
	AwmEvent *(*poll_event)(PlatformCtx *p);
	AwmEvent *(*next_event)(PlatformCtx *p);

	/* --- keyboard grab ------------------------------------------------- */
	/* Grab the keyboard; owner_events=0.  Call flush() after if needed. */
	void (*grab_keyboard)(PlatformCtx *p, WinId w, WmTimestamp t);
	/* Release the keyboard grab. */
	void (*ungrab_keyboard)(PlatformCtx *p, WmTimestamp t);
	/* Ungrab all passive key grabs on a window. */
	void (*ungrab_key)(PlatformCtx *p, WinId w);

	/* --- root window tree ---------------------------------------------- */
	/* Query immediate children of root.  Fills *wins_out (caller must
	 * free(*wins_out)) and *n_out.  Returns 1 on success. */
	int (*query_root_tree)(PlatformCtx *p, WinId **wins_out, int *n_out);

	/* --- window attributes batch --------------------------------------- */
	/* Fetch override_redirect and map_state for w in one round-trip.
	 * Returns 1 on success. */
	int (*get_window_attributes)(PlatformCtx *p, WinId w,
	    int *override_redirect_out, int *map_state_out);

	/* --- raw property read --------------------------------------------- */
	/* Generic get_property wrapper.  Returns a heap-allocated reply
	 * that the caller must free(), or NULL on failure.  value_len and
	 * value pointer are accessed via the returned reply. */
#ifdef BACKEND_X11
	xcb_get_property_reply_t *(*get_prop_raw)(PlatformCtx *p, WinId w,
	    AtomId prop, AtomId type, uint32_t long_length);
#endif

	/* --- connection introspection -------------------------------------- */
	/* Return the file descriptor of the X connection (for fork pre-exec
	 * close).  Returns -1 if no connection. */
	int (*get_connection_fd)(PlatformCtx *p);

	/* --- synchronous round-trip ---------------------------------------- */
	/* Issue a cheap synchronous round-trip to flush all pending requests
	 * to the server and wait for them to be processed. */
	void (*sync)(PlatformCtx *p);

	/* --- pixmap / colormap teardown ------------------------------------ */
	void (*free_pixmap)(PlatformCtx *p, WmPixmap pm);
	void (*free_colormap)(PlatformCtx *p, WmColormap cm);

	/* --- atom interning ------------------------------------------------ */
	/* Batch-intern N atoms in a single async round-trip.  names[] and
	 * atoms_out[] are parallel arrays of length n. */
	void (*intern_atoms_batch)(
	    PlatformCtx *p, const char **names, AtomId *atoms_out, int n);

	/* --- generic window creation --------------------------------------- */
	/* Create an input-output window; returns its handle. */
	WinId (*create_window)(PlatformCtx *p, WinId parent, int x, int y, int w,
	    int h, uint32_t val_mask, const uint32_t *vals);

	/* --- root event mask / cursor selection ---------------------------- */
	/* Set the event mask on the root window. */
	void (*select_root_events)(PlatformCtx *p, uint32_t mask);
	/* Set the cursor on the root window. */
	void (*set_root_cursor)(PlatformCtx *p, WmCursor cursor);

	/* --- RandR setup --------------------------------------------------- */
#ifdef XRANDR
	/* Probe RandR extension: populate randrbase/rrerrbase, subscribe to
	 * screen-change notifications.  No-op if extension absent. */
	void (*randr_init)(PlatformCtx *p);
	/* Query physical DPI from the first active RandR output.
	 * Returns the DPI value, or 0.0 if unavailable. */
	double (*randr_probe_dpi)(PlatformCtx *p);
#endif

	/* --- screen setup -------------------------------------------------- */
	/* Populate sw, sh, root from the setup for the configured screen
	 * number.  Called once at the start of setup(). */
	void (*init_screen)(PlatformCtx *p);

	/* --- key symbol table lifecycle ------------------------------------ */
	/* Allocate the key symbols table. */
	void (*keysyms_alloc)(PlatformCtx *p);
	/* Free the key symbols table. */
	void (*keysyms_free)(PlatformCtx *p);

	/* --- resource manager ---------------------------------------------- */
	/* Fetch the RESOURCE_MANAGER property from the root window and return
	 * it as a NUL-terminated heap string.  Caller must free().
	 * Returns NULL if unavailable. */
	char *(*get_resource_manager)(PlatformCtx *p);
} WmBackend;

extern WmBackend *g_wm_backend;
#ifdef BACKEND_X11
extern WmBackend wm_backend_x11;

/* XCB async error handler — defined in platform_x11.c, called from
 * x_dispatch_cb() in awm.c when response_type == 0. */
int xcb_error_handler(xcb_generic_error_t *e);
#endif /* BACKEND_X11 */

#endif /* PLATFORM_H */
