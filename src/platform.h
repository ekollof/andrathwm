/* See LICENSE file for copyright and license details. */
/* platform.h — X11 platform context (PlatformCtx) declaration.
 *
 * This header is included from awm.h after the atom enums (WMLast,
 * NetLast, XLast) are declared.  Do not include it directly before
 * those enums are visible.
 *
 * PlatformCtx consolidates every X11 connection-level global that was
 * previously scattered as bare extern declarations in awm.h / awm.c.
 * A single instance, g_plat, is defined in platform_x11.c and is the
 * sole source of truth for all X11 connection state.
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
#ifdef XRANDR
#include <xcb/randr.h>
#endif

#ifdef BACKEND_X11
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
#endif /* BACKEND_X11 */

#endif /* PLATFORM_H */
